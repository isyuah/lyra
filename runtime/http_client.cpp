#include "native_async.h"
#include "sigpipe.h"
#include "strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <curl/curl.h>
#include <uv.h>

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

enum class Status : int64_t { Ok, InvalidRequest, TimedOut, TooLarge, TooManyRedirects, RedirectRejected, Tls, Network, Cancelled, Busy };

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
    size_t remaining;
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

struct Configurator {
    CURL* easy;
    CURLcode status = CURLE_OK;

    template<class Value> void set(CURLoption option, Value value) {
        if (status == CURLE_OK) status = curl_easy_setopt(easy, option, value);
    }
};

Failure configure(CURL* easy, CURLU* url, const Request& request, const std::string& method, const std::string& proxy,
                  const Headers& headers, int64_t timeout, Transfer& transfer) {
    Configurator settings{easy};
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
    settings.set(CURLOPT_PROXY, proxy.c_str());
    settings.set(CURLOPT_NOPROXY, "");
    settings.set(CURLOPT_SSL_VERIFYPEER, 1L);
    settings.set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (request.hasProxy) {
        settings.set(CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
        settings.set(CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
    }
    if (request.hasCa) {
        curl_blob ca{const_cast<char*>(lyraStringBytes(request.caPem)), lyraStringLength(request.caPem), CURL_BLOB_COPY};
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
    return curlFailure(settings.status);
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

struct Job {
    const Request* request;
    const Response* result{};
    LyraNativeAsync* task{};
    Clock::time_point deadline{};
    CURL* active{};
};

struct Active {
    Job& job;
    LyraGcRoot* root;
    std::string method, proxy;
    Url url{curl_url(), curl_url_cleanup};
    Easy easy{curl_easy_init(), curl_easy_cleanup};
    Headers headers{nullptr, curl_slist_free_all};
    std::optional<Origin> firstOrigin;
    Transfer transfer;
    int64_t followed{};

    explicit Active(Job& job) : job(job), root(lyraGcRootCreate(&job)), method(bytes(job.request->method)), proxy(bytes(job.request->proxy)),
        transfer{static_cast<size_t>(job.request->maxResponseBytes), {}, {}} {}

    ~Active() { lyraGcRootDrop(root); }

    Failure prepare() {
        const auto& request = *job.request;
        const auto address = std::string(bytes(request.url));
        const auto readOnly = method == "GET" || method == "HEAD";
        if (!token(method) || sameAscii(method, "CONNECT") || (readOnly && request.hasBody) || control(address) || control(proxy))
            return {Status::InvalidRequest};
        if (!url || !easy) return {Status::Network, CURLE_OUT_OF_MEMORY};
        if (curl_url_set(url.get(), CURLUPART_URL, address.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return {Status::InvalidRequest};
        firstOrigin = origin(url.get());
        if (!firstOrigin) return {Status::InvalidRequest};
        if (request.hasProxy) {
            Url parsed(curl_url(), curl_url_cleanup);
            if (!parsed) return {Status::Network, CURLE_OUT_OF_MEMORY};
            if (curl_url_set(parsed.get(), CURLUPART_URL, proxy.c_str(), 0) != CURLUE_OK || !origin(parsed.get())) return {Status::InvalidRequest};
        }
        return requestHeaders(request, headers, request.hasBody || method == "POST");
    }

    Failure next(long status, bool& again) {
        if ((method != "GET" && method != "HEAD") || job.request->maxRedirects == 0 || !redirect(status)) return {};
        const std::string* location = nullptr;
        for (const auto& [name, value] : transfer.headers) {
            if (!sameAscii(name, "Location")) continue;
            if (location) return {Status::RedirectRejected};
            location = &value;
        }
        if (!location) return {};
        if (followed == job.request->maxRedirects) return {Status::TooManyRedirects};
        if (location->empty() || control(*location) || curl_url_set(url.get(), CURLUPART_URL, location->c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
            return {Status::RedirectRejected};
        const auto target = origin(url.get());
        if (!target || !sameOrigin(*firstOrigin, *target)) return {Status::RedirectRejected};
        ++followed;
        again = true;
        return {};
    }
};

class ClientLoop {
    static constexpr std::size_t maxJobs = 128, batchSize = 32;
    static constexpr long maxConnections = 64, maxPerHost = 16;

    struct Command { Job* job; LyraGcRoot* root; bool cancel; };
    struct Watch { uv_poll_t poll{}; ClientLoop* client; curl_socket_t socket; };

    std::mutex mutex;
    std::once_flag once;
    Failure startup{};
    std::deque<Command> inbox;
    std::size_t outstanding{};
    uv_loop_t loop{};
    uv_async_t wake{};
    uv_timer_t timer{};
    uv_thread_t thread{};
    CURLM* multi{};
    bool running{}, stopping{};
    std::unordered_map<CURL*, std::unique_ptr<Active>> transfers;
    std::unordered_map<curl_socket_t, Watch*> sockets;

    void notify() { if (uv_async_send(&wake) != 0) std::abort(); }

    void removeSocket(curl_socket_t socket) {
        auto watch = sockets.extract(socket);
        if (watch.empty()) return;
        auto* value = watch.mapped();
        uv_poll_stop(&value->poll);
        uv_close(reinterpret_cast<uv_handle_t*>(&value->poll), [](uv_handle_t* handle) { delete static_cast<Watch*>(handle->data); });
    }

    int watchSocket(curl_socket_t socket, int action) {
        if (action == CURL_POLL_REMOVE) { removeSocket(socket); return 0; }
        if (action == CURL_POLL_NONE) {
            if (auto found = sockets.find(socket); found != sockets.end()) uv_poll_stop(&found->second->poll);
            return 0;
        }
        auto found = sockets.find(socket);
        if (found == sockets.end()) {
            auto entry = std::make_unique<Watch>(Watch{{}, this, socket});
            if (uv_poll_init_socket(&loop, &entry->poll, socket) != 0) return -1;
            entry->poll.data = entry.get();
            found = sockets.emplace(socket, entry.release()).first;
        }
        int events = 0;
        if (action & CURL_POLL_IN) events |= UV_READABLE;
        if (action & CURL_POLL_OUT) events |= UV_WRITABLE;
        return uv_poll_start(&found->second->poll, events, [](uv_poll_t* handle, int status, int ready) {
            const auto& active = *static_cast<Watch*>(handle->data);
            int events = status < 0 ? CURL_CSELECT_ERR : 0;
            if (ready & UV_READABLE) events |= CURL_CSELECT_IN;
            if (ready & UV_WRITABLE) events |= CURL_CSELECT_OUT;
            active.client->advance(active.socket, events);
        }) == 0 ? 0 : -1;
    }

    int setTimer(long delay) {
        if (delay < 0) return uv_timer_stop(&timer) == 0 ? 0 : -1;
        uv_update_time(&loop);
        return uv_timer_start(&timer, [](uv_timer_t* timer) {
            static_cast<ClientLoop*>(timer->data)->advance(CURL_SOCKET_TIMEOUT, 0);
        }, static_cast<uint64_t>(delay), 0) == 0 ? 0 : -1;
    }

    Failure open() {
        if (auto code = curl_global_init(CURL_GLOBAL_DEFAULT); code != CURLE_OK) return curlFailure(code);
        multi = curl_multi_init();
        auto startedLoop = uv_loop_init(&loop);
        if (startedLoop != 0) {
            if (multi) curl_multi_cleanup(multi);
            curl_global_cleanup();
            return {Status::Network, startedLoop};
        }
        if (!multi) { uv_loop_close(&loop); curl_global_cleanup(); return {Status::Network, CURLE_OUT_OF_MEMORY}; }
        auto opened = uv_async_init(&loop, &wake, [](auto* handle) { static_cast<ClientLoop*>(handle->data)->dispatch(); });
        bool wakeOpen = opened == 0;
        if (!opened) opened = uv_timer_init(&loop, &timer);
        bool timerOpen = opened == 0;
        if (timerOpen) {
            wake.data = this;
            timer.data = this;
            auto set = [&](CURLMoption option, auto value) { if (!opened) opened = curl_multi_setopt(multi, option, value); };
            set(CURLMOPT_SOCKETFUNCTION, +[](CURL*, curl_socket_t socket, int action, void* data, void*) {
                return static_cast<ClientLoop*>(data)->watchSocket(socket, action);
            });
            set(CURLMOPT_SOCKETDATA, this);
            set(CURLMOPT_TIMERFUNCTION, +[](CURLM*, long delay, void* data) { return static_cast<ClientLoop*>(data)->setTimer(delay); });
            set(CURLMOPT_TIMERDATA, this);
            set(CURLMOPT_MAX_TOTAL_CONNECTIONS, maxConnections);
            set(CURLMOPT_MAX_HOST_CONNECTIONS, maxPerHost);
            set(CURLMOPT_MAXCONNECTS, maxConnections);
        }
        if (!opened) opened = uv_thread_create(&thread, [](void* argument) {
            auto& owner = *static_cast<ClientLoop*>(argument);
            const auto registration = lyraGcEnterWorker();
            [[maybe_unused]] lyraRuntime::SuppressSigpipe ignoreSigpipe;
            uv_run(&owner.loop, UV_RUN_DEFAULT);
            if (uv_loop_close(&owner.loop) != 0) std::abort();
            lyraGcLeaveWorker(registration);
        }, this);
        if (opened) {
            curl_multi_cleanup(multi);
            if (timerOpen) uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
            if (wakeOpen) uv_close(reinterpret_cast<uv_handle_t*>(&wake), nullptr);
            uv_run(&loop, UV_RUN_DEFAULT);
            uv_loop_close(&loop);
            curl_global_cleanup();
            return {Status::Network, opened};
        }
        running = true;
        return {};
    }

    void dispatch() {
        std::array<Command, batchSize> commands;
        std::size_t count = 0;
        bool shutdown;
        {
            std::lock_guard lock(mutex);
            for (; count < commands.size() && !inbox.empty(); ++count) {
                commands[count] = inbox.front();
                inbox.pop_front();
            }
            shutdown = stopping && inbox.empty();
            if (!inbox.empty()) notify();
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto& command = commands[index];
            if (command.cancel) cancel(*command.job);
            else start(*command.job);
            lyraGcRootDrop(command.root);
        }
        if (shutdown) {
            if (!transfers.empty()) std::abort();
            curl_multi_cleanup(multi);
            while (!sockets.empty()) removeSocket(sockets.begin()->first);
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
            uv_close(reinterpret_cast<uv_handle_t*>(&wake), nullptr);
        }
    }

    void finish(Job& job, const Response* response) {
        auto* task = std::exchange(job.task, nullptr);
        job.result = response;
        {
            std::lock_guard lock(mutex);
            --outstanding;
        }
        lyraNativeComplete(task, &job.result);
    }

    std::unique_ptr<Active> take(CURL* handle) {
        auto entry = transfers.extract(handle);
        if (entry.empty()) std::abort();
        auto active = std::move(entry.mapped());
        active->job.active = nullptr;
        if (curl_multi_remove_handle(multi, handle) != CURLM_OK) std::abort();
        return active;
    }

    void cancel(Job& job) {
        if (!job.task) return;
        take(job.active).reset();
        finish(job, failed({Status::Cancelled}));
    }

    Failure attach(std::unique_ptr<Active>& active) {
        auto& job = active->job;
        auto timeout = std::chrono::ceil<milliseconds>(job.deadline - Clock::now()).count();
        if (timeout <= 0) return {Status::TimedOut};
        auto& transfer = active->transfer;
        transfer.headers.clear();
        transfer.body.clear();
        transfer.readingHeaders = transfer.overBudget = false;
        auto* easy = active->easy.get();
        curl_easy_reset(easy);
        auto error = configure(easy, active->url.get(), *job.request, active->method, active->proxy, active->headers, timeout, transfer);
        if (error.status != Status::Ok) return error;
        if (auto code = curl_multi_add_handle(multi, easy); code != CURLM_OK) return {Status::Network, code};
        job.active = easy;
        transfers.emplace(easy, std::move(active));
        return {};
    }

    void start(Job& job) {
        const auto& request = *job.request;
        if (request.connectTimeoutMs <= 0 || request.connectTimeoutMs > INT32_MAX || request.maxResponseBytes <= 0 ||
            request.maxRedirects < 0 || request.maxRedirects > 20 || (request.hasCa && bytes(request.caPem).empty())) {
            finish(job, failed({Status::InvalidRequest}));
            return;
        }
        auto active = std::make_unique<Active>(job);
        auto failure = active->prepare();
        if (failure.status == Status::Ok) failure = attach(active);
        if (failure.status != Status::Ok) { active.reset(); finish(job, failed(failure)); }
    }

    void completed(CURL* handle, CURLcode result) {
        long status = 0;
        if (result == CURLE_OK) result = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        auto active = take(handle);
        auto& job = active->job;
        auto failure = active->transfer.overBudget ? Failure{Status::TooLarge} : curlFailure(result);
        if (failure.status == Status::Ok && status < 100) failure = {Status::Network};
        bool again = false;
        if (failure.status == Status::Ok) failure = active->next(status, again);
        if (failure.status == Status::Ok && again) {
            failure = attach(active);
            if (failure.status == Status::Ok) return;
        }
        auto* response = failure.status == Status::Ok ? received(status, active->transfer) : failed(failure);
        active.reset();
        finish(job, response);
    }

    void advance(curl_socket_t socket, int events) {
        int pending = 0;
        if (auto code = curl_multi_socket_action(multi, socket, events, &pending); code != CURLM_OK) {
            while (!transfers.empty()) {
                auto active = take(transfers.begin()->first);
                auto& job = active->job;
                active.reset();
                finish(job, failed({Status::Network, code}));
            }
            return;
        }
        int messages = 0;
        while (auto* message = curl_multi_info_read(multi, &messages)) {
            if (message->msg == CURLMSG_DONE) completed(message->easy_handle, message->data.result);
        }
    }

public:
    ~ClientLoop() {
        if (!running) return;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            notify();
        }
        uv_thread_join(&thread);
        curl_global_cleanup();
    }

    bool submit(LyraNativeAsync* task, Job& job) {
        job.deadline = Clock::now() + milliseconds(job.request->timeoutMs);
        std::call_once(once, [&] { startup = open(); });
        if (startup.status != Status::Ok) { job.result = failed(startup); return false; }
        {
            std::lock_guard lock(mutex);
            if (outstanding < maxJobs) {
                ++outstanding;
                job.task = task;
                inbox.push_back({&job, lyraGcRootCreate(&job), false});
                notify();
                return true;
            }
        }
        job.result = failed({Status::Busy});
        return false;
    }

    void requestCancel(Job& job) {
        std::lock_guard lock(mutex);
        inbox.push_back({&job, lyraGcRootCreate(&job), true});
        notify();
    }
};

ClientLoop& clientLoop() {
    static ClientLoop loop;
    return loop;
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

extern "C" LyraTask* lyraHttpClientSendAsync(const Request* request) {
    auto* job = ::new (lyraGcAlloc(sizeof(Job))) Job{request};
    return lyraTaskNative([](LyraNativeAsync* task, void* data) { return clientLoop().submit(task, *static_cast<Job*>(data)); },
        [](void* data) { clientLoop().requestCancel(*static_cast<Job*>(data)); }, job, &job->result);
}
extern "C" int64_t lyraHttpClientErrorKind(const Response* response) { return static_cast<int64_t>(response->failure.status); }
extern "C" int64_t lyraHttpClientErrorCode(const Response* response) { return response->failure.code; }
extern "C" int64_t lyraHttpClientStatus(const Response* response) { return response->status; }
extern "C" int64_t lyraHttpClientHeaderCount(const Response* response) { return static_cast<int64_t>(response->headerCount); }
extern "C" const void* lyraHttpClientHeaderName(const Response* response, int64_t index) { return response->headers[index].name; }
extern "C" const void* lyraHttpClientHeaderValue(const Response* response, int64_t index) { return response->headers[index].value; }
extern "C" const void* lyraHttpClientBody(const Response* response) { return response->body; }
extern "C" uint8_t lyraHttpClientHeaderMatches(const void* first, const void* second) { return sameAscii(bytes(first), bytes(second)); }
