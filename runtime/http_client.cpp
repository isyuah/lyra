#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <curl/curl.h>

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using Multi = std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)>;
using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

enum class Status : int64_t { Ok, InvalidRequest, TimedOut, TooLarge, TooManyRedirects, RedirectRejected, Tls, Network, Cancelled };

struct Failure {
    Status status = Status::Ok;
    int64_t code = 0;
};

struct Field { const void* name; const void* value; };

struct Request {
    const void* method;
    const void* url;
    const void* body;
    const void* proxy;
    const void* caPem;
    int64_t connectTimeoutMs;
    int64_t timeoutMs;
    int64_t maxResponseBytes;
    int64_t maxRedirects;
    bool hasBody;
    bool hasProxy;
    bool hasCa;
    Field* headers;
    size_t headerCount;
};

struct Response {
    Failure failure;
    int64_t status = 0;
    const Field* headers = nullptr;
    size_t headerCount = 0;
    const void* body = nullptr;
};

struct Transfer {
    size_t& remaining;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool readingHeaders = false;
    bool overBudget = false;

    bool consume(size_t count) {
        if (count > remaining) { overBudget = true; return false; }
        remaining -= count;
        return true;
    }
};

template<class Value> Value* gcArray(size_t count) {
    if (count == 0) return nullptr;
    if (count > std::numeric_limits<size_t>::max() / sizeof(Value)) std::abort();
    return static_cast<Value*>(lyraGcAlloc(count * sizeof(Value)));
}

std::string_view bytes(const void* value) { return {lyraStringBytes(value), lyraStringLength(value)}; }

const void* lyraBytes(std::string_view value) { return lyraStringFromBytes(value.data(), value.size()); }

bool sameAscii(std::string_view left, std::string_view right) {
    auto lower = [](char byte) { return byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte; };
    return std::ranges::equal(left, right, [&](char lhs, char rhs) { return lower(lhs) == lower(rhs); });
}

bool token(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            std::string_view("!#$%&'*+-.^_`|~").contains(byte);
    });
}

bool control(std::string_view value, bool allowTab = false) {
    return std::ranges::any_of(value, [=](unsigned char byte) { return (byte < 32 && !(allowTab && byte == '\t')) || byte == 127; });
}

bool managedHeader(std::string_view name) {
    for (const auto* managed : {"Host", "Connection", "Content-Length", "Transfer-Encoding", "Keep-Alive", "TE", "Trailer",
                               "Proxy-Authorization", "Proxy-Connection", "Upgrade"}) {
        if (sameAscii(name, managed)) return true;
    }
    return false;
}

bool addHeader(Headers& list, const std::string& line) {
    auto* head = curl_slist_append(list.get(), line.c_str());
    if (!head) return false;
    list.release();
    list.reset(head);
    return true;
}

Failure requestHeaders(const Request& request, Headers& list, bool sendsBody) {
    bool contentType = false;
    for (size_t index = 0; index < request.headerCount; ++index) {
        const auto& field = request.headers[index];
        const auto name = bytes(field.name);
        const auto value = bytes(field.value);
        if (!token(name) || managedHeader(name) || control(value, true)) return {Status::InvalidRequest};
        contentType |= sameAscii(name, "Content-Type");
        auto line = std::string(name);
        line += value.empty() ? ";" : ": ";
        line += value;
        if (!addHeader(list, line)) return {Status::Network, CURLE_OUT_OF_MEMORY};
    }
    if (sendsBody && !contentType && !addHeader(list, "Content-Type:")) return {Status::Network, CURLE_OUT_OF_MEMORY};
    return {};
}

std::optional<std::string> urlPart(CURLU* url, CURLUPart part, unsigned flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url, part, &value, flags) != CURLUE_OK) return std::nullopt;
    std::unique_ptr<char, decltype(&curl_free)> release(value, curl_free);
    return std::string(value);
}

struct Origin { std::string scheme, host, port, zone; };

std::optional<Origin> origin(CURLU* url) {
    auto scheme = urlPart(url, CURLUPART_SCHEME);
    auto host = urlPart(url, CURLUPART_HOST);
    auto port = urlPart(url, CURLUPART_PORT, CURLU_DEFAULT_PORT);
    if (!scheme || (*scheme != "http" && *scheme != "https") || !host || host->empty() || !port) return std::nullopt;
    return Origin{std::move(*scheme), std::move(*host), std::move(*port), urlPart(url, CURLUPART_ZONEID).value_or("")};
}

bool sameOrigin(const Origin& first, const Origin& second) {
    return first.scheme == second.scheme && sameAscii(first.host, second.host) && first.port == second.port && first.zone == second.zone;
}

std::string_view trimOws(std::string_view value) {
    auto start = value.find_first_not_of(" \t");
    if (start == value.npos) return {};
    return value.substr(start, value.find_last_not_of(" \t") - start + 1);
}

size_t headerCallback(char* bytes, size_t width, size_t items, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    const auto count = width * items;
    if (!transfer.consume(count)) return 0;
    auto line = std::string_view(bytes, count);
    if (line.ends_with('\n')) line.remove_suffix(1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.starts_with("HTTP/")) {
        transfer.headers.clear();
        transfer.body.clear();
        transfer.readingHeaders = true;
    } else if (line.empty()) {
        transfer.readingHeaders = false;
    } else if (transfer.readingHeaders) {
        if (line.starts_with(' ') || line.starts_with('\t')) {
            if (!transfer.headers.empty()) {
                auto& value = transfer.headers.back().second;
                value += ' ';
                value += trimOws(line);
            }
        } else {
            const auto separator = line.find(':');
            const auto name = line.substr(0, separator);
            if (separator == line.npos || !token(name)) return 0;
            transfer.headers.emplace_back(name, trimOws(line.substr(separator + 1)));
        }
    }
    return count;
}

size_t bodyCallback(char* bytes, size_t width, size_t items, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    const auto count = width * items;
    if (!transfer.consume(count)) return 0;
    transfer.body.append(bytes, count);
    return count;
}

int progressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return lyraBlockingCancelled(); }

Failure curlFailure(CURLcode code) {
    switch (code) {
        case CURLE_OK: return {};
        case CURLE_OPERATION_TIMEDOUT: return {Status::TimedOut, code};
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_CLIENTCERT: return {Status::Tls, code};
        default: return {Status::Network, code};
    }
}

Failure pump(CURLM* multi, const Transfer& transfer) {
    for (;;) {
        if (lyraBlockingCancelled()) return {Status::Cancelled};
        int running;
        if (auto code = curl_multi_perform(multi, &running); code != CURLM_OK) return {Status::Network, code};
        if (lyraBlockingCancelled()) return {Status::Cancelled};
        int queued;
        while (auto* event = curl_multi_info_read(multi, &queued)) {
            if (event->msg == CURLMSG_DONE) {
                if (transfer.overBudget) return {Status::TooLarge};
                return curlFailure(event->data.result);
            }
        }
        if (!running) return {Status::Network};
        if (auto code = curl_multi_poll(multi, nullptr, 0, 25, nullptr); code != CURLM_OK) return {Status::Network, code};
    }
}

struct Configurator {
    CURL* easy;
    CURLcode status = CURLE_OK;

    template<class Value> void set(CURLoption option, Value value) {
        if (status == CURLE_OK) status = curl_easy_setopt(easy, option, value);
    }
};

Failure perform(CURLM* multi, CURLU* url, const Request& request, const std::string& method, const std::string& proxy,
                const Headers& headers, int64_t timeout, Transfer& transfer, long& responseStatus) {
    Easy easy(curl_easy_init(), curl_easy_cleanup);
    if (!easy) return {Status::Network, CURLE_FAILED_INIT};
    Configurator settings{easy.get()};
    settings.set(CURLOPT_CURLU, url);
    settings.set(CURLOPT_PROTOCOLS_STR, "http,https");
    settings.set(CURLOPT_CUSTOMREQUEST, method.c_str());
    settings.set(CURLOPT_HTTPHEADER, headers.get());
    settings.set(CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
    settings.set(CURLOPT_NOSIGNAL, 1L);
    settings.set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min(request.connectTimeoutMs, timeout)));
    settings.set(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
    settings.set(CURLOPT_HEADERFUNCTION, headerCallback);
    settings.set(CURLOPT_HEADERDATA, &transfer);
    settings.set(CURLOPT_WRITEFUNCTION, bodyCallback);
    settings.set(CURLOPT_WRITEDATA, &transfer);
    settings.set(CURLOPT_XFERINFOFUNCTION, progressCallback);
    settings.set(CURLOPT_NOPROGRESS, 0L);
    settings.set(CURLOPT_PROXY, proxy.c_str());
    settings.set(CURLOPT_NOPROXY, "");
    settings.set(CURLOPT_SSL_VERIFYPEER, 1L);
    settings.set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (request.hasProxy) {
        settings.set(CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
        settings.set(CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
    }
    if (request.hasCa) {
        curl_blob ca{const_cast<char*>(lyraStringBytes(request.caPem)), lyraStringLength(request.caPem), CURL_BLOB_NOCOPY};
        settings.set(CURLOPT_CAINFO, static_cast<const char*>(nullptr));
        settings.set(CURLOPT_CAPATH, static_cast<const char*>(nullptr));
        settings.set(CURLOPT_CAINFO_BLOB, &ca);
        if (request.hasProxy) {
            settings.set(CURLOPT_PROXY_CAINFO, static_cast<const char*>(nullptr));
            settings.set(CURLOPT_PROXY_CAPATH, static_cast<const char*>(nullptr));
            settings.set(CURLOPT_PROXY_CAINFO_BLOB, &ca);
        }
    }
#ifdef _WIN32
    else {
        settings.set(CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        if (request.hasProxy) settings.set(CURLOPT_PROXY_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    }
#endif
    if (method == "HEAD") settings.set(CURLOPT_NOBODY, 1L);
    else if (request.hasBody || method == "POST") {
        const auto body = bytes(request.body);
        settings.set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        settings.set(CURLOPT_POSTFIELDS, body.empty() ? "" : body.data());
    }
    if (settings.status != CURLE_OK) return curlFailure(settings.status);
    if (auto code = curl_multi_add_handle(multi, easy.get()); code != CURLM_OK) return {Status::Network, code};
    auto failure = pump(multi, transfer);
    curl_multi_remove_handle(multi, easy.get());
    if (failure.status != Status::Ok) return failure;
    if (auto code = curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &responseStatus); code != CURLE_OK) return curlFailure(code);
    return responseStatus >= 100 ? Failure{} : Failure{Status::Network};
}

bool redirect(long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const Response* failed(Failure failure) {
    auto* output = gcArray<Response>(1);
    *output = Response{failure};
    return output;
}

const Response* received(long status, const Transfer& transfer) {
    auto* fields = gcArray<Field>(transfer.headers.size());
    for (size_t index = 0; index < transfer.headers.size(); ++index) {
        const auto& [name, value] = transfer.headers[index];
        fields[index] = Field{lyraBytes(name), lyraBytes(value)};
    }
    auto* output = gcArray<Response>(1);
    *output = Response{{}, status, fields, transfer.headers.size(), lyraBytes(transfer.body)};
    return output;
}

const Response* send(const Request& request) {
    if (lyraBlockingCancelled()) return failed({Status::Cancelled});
    if (request.connectTimeoutMs <= 0 || request.connectTimeoutMs > INT32_MAX || request.maxResponseBytes <= 0 ||
        request.maxRedirects < 0 || request.maxRedirects > 20 || (request.hasCa && bytes(request.caPem).empty())) return failed({Status::InvalidRequest});
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return failed(curlFailure(initialized));
    const auto method = std::string(bytes(request.method));
    const auto url = std::string(bytes(request.url));
    const auto proxy = std::string(bytes(request.proxy));
    const auto readOnly = method == "GET" || method == "HEAD";
    if (!token(method) || sameAscii(method, "CONNECT") || (readOnly && request.hasBody) || control(url) || control(proxy)) {
        return failed({Status::InvalidRequest});
    }
    Url current(curl_url(), curl_url_cleanup);
    if (!current) return failed({Status::Network, CURLE_OUT_OF_MEMORY});
    if (curl_url_set(current.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return failed({Status::InvalidRequest});
    const auto firstOrigin = origin(current.get());
    if (!firstOrigin) return failed({Status::InvalidRequest});
    if (request.hasProxy) {
        Url parsed(curl_url(), curl_url_cleanup);
        if (!parsed) return failed({Status::Network, CURLE_OUT_OF_MEMORY});
        if (curl_url_set(parsed.get(), CURLUPART_URL, proxy.c_str(), 0) != CURLUE_OK || !origin(parsed.get())) return failed({Status::InvalidRequest});
    }
    Headers headers(nullptr, curl_slist_free_all);
    if (auto failure = requestHeaders(request, headers, request.hasBody || method == "POST"); failure.status != Status::Ok) return failed(failure);
    Multi multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi) return failed({Status::Network, CURLE_FAILED_INIT});
    size_t remaining = static_cast<size_t>(request.maxResponseBytes);
    const auto deadline = Clock::now() + milliseconds(request.timeoutMs);
    for (int64_t followed = 0;; ++followed) {
        auto timeout = std::chrono::ceil<milliseconds>(deadline - Clock::now()).count();
        if (timeout <= 0) return failed({Status::TimedOut});
        Transfer transfer{remaining, {}, {}};
        long status;
        auto outcome = perform(multi.get(), current.get(), request, method, proxy, headers, timeout, transfer, status);
        if (outcome.status != Status::Ok) return failed(outcome);
        if (!readOnly || request.maxRedirects == 0 || !redirect(status)) return received(status, transfer);
        const std::string* location = nullptr;
        for (const auto& [name, value] : transfer.headers) {
            if (!sameAscii(name, "Location")) continue;
            if (location) return failed({Status::RedirectRejected});
            location = &value;
        }
        if (!location) return received(status, transfer);
        if (followed == request.maxRedirects) return failed({Status::TooManyRedirects});
        if (location->empty() || control(*location) || curl_url_set(current.get(), CURLUPART_URL, location->c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) {
            return failed({Status::RedirectRejected});
        }
        const auto target = origin(current.get());
        if (!target || !sameOrigin(*firstOrigin, *target)) return failed({Status::RedirectRejected});
    }
}

}

extern "C" Request* lyraHttpClientPrepare(const void* method, const void* url, const void* body, const void* proxy, const void* caPem,
                                          int64_t connectTimeoutMs, int64_t timeoutMs, int64_t maxResponseBytes, int64_t maxRedirects,
                                          uint8_t hasBody, uint8_t hasProxy, uint8_t hasCa, int64_t count) {
    auto* request = gcArray<Request>(1);
    auto size = static_cast<size_t>(count);
    *request = {method, url, body, proxy, caPem, connectTimeoutMs, timeoutMs, maxResponseBytes, maxRedirects,
        hasBody != 0, hasProxy != 0, hasCa != 0, gcArray<Field>(size), size};
    return request;
}

extern "C" void lyraHttpClientSetHeader(Request* request, int64_t index, const void* name, const void* value) {
    request->headers[index] = {name, value};
}

extern "C" const Response* lyraHttpClientSend(const Request* request) { return send(*request); }
extern "C" int64_t lyraHttpClientErrorKind(const Response* response) { return static_cast<int64_t>(response->failure.status); }
extern "C" int64_t lyraHttpClientErrorCode(const Response* response) { return response->failure.code; }
extern "C" int64_t lyraHttpClientStatus(const Response* response) { return response->status; }
extern "C" int64_t lyraHttpClientHeaderCount(const Response* response) { return static_cast<int64_t>(response->headerCount); }
extern "C" const void* lyraHttpClientHeaderName(const Response* response, int64_t index) { return response->headers[index].name; }
extern "C" const void* lyraHttpClientHeaderValue(const Response* response, int64_t index) { return response->headers[index].value; }
extern "C" const void* lyraHttpClientBody(const Response* response) { return response->body; }
extern "C" uint8_t lyraHttpClientHeaderMatches(const void* first, const void* second) { return sameAscii(bytes(first), bytes(second)); }
