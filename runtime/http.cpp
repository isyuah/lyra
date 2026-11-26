#include "http_transport.h"
#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <llhttp.h>
#include <new>
#include <string>
#include <string_view>

namespace lyraRuntime::http {

int64_t now() { return static_cast<int64_t>(uv_hrtime() / UINT64_C(1000000)); }

int waitFor(net::Socket descriptor, short event, int64_t deadline) {
    net::PollSlot slot{descriptor, event, 0};
    constexpr int64_t interval = 25;
    for (;;) {
        if (lyraBlockingCancelled()) return ECANCELED;
        auto remaining = deadline - now();
        if (remaining < 0) return ETIMEDOUT;
        auto result = net::poll(slot, static_cast<int>(std::min(remaining, interval)));
        if (result > 0) return 0;
        if (result == 0) {
            if (remaining <= interval) return ETIMEDOUT;
            continue;
        }
        auto status = net::error();
        if (status != EINTR) return status;
    }
}

}

namespace {

namespace net = lyraRuntime::net;
namespace http = lyraRuntime::http;
using http::now;
using http::waitFor;

class SocketAccess {
    static constexpr uint32_t closed = UINT32_MAX;
    static constexpr uint32_t exclusive = closed - 1;
    mutable std::atomic<uint32_t> active{0};

public:
    int acquire(bool shared) const {
        auto uses = active.load(std::memory_order_acquire);
        for (;;) {
            if (uses == closed) return EBADF;
            if (uses >= exclusive || (!shared && uses != 0)) return EBUSY;
            if (active.compare_exchange_weak(uses, shared ? uses + 1 : exclusive,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return 0;
        }
    }

    void release(bool shared, bool stopped) const {
        if (shared) active.fetch_sub(1, std::memory_order_release);
        else active.store(stopped ? closed : 0, std::memory_order_release);
    }
};

class SocketLease {
    const SocketAccess& access;
    const bool shared;
    bool stopped{false};

public:
    const int status;

    explicit SocketLease(const SocketAccess& target, bool read = false) : access(target), shared(read), status(access.acquire(shared)) {}
    SocketLease(const SocketLease&) = delete;
    SocketLease& operator=(const SocketLease&) = delete;
    ~SocketLease() { if (!status) access.release(shared, stopped); }
    void markClosed() { stopped = true; }
};

struct Endpoint {
    net::Socket descriptor{net::invalidSocket};
    int port{};
    int64_t status{};
    const void* detail{};
    const http::Transport* transport{};
    void* listener{};
    int64_t handshakeTimeoutMs{};
    std::atomic<bool> draining{};
    SocketAccess access{};
};
struct Field { const void* name; const void* value; };
struct Segment { const void* text; bool parameter; };
struct PathInteger { int64_t value; bool valid; };
struct RoutePattern { Segment* segments; std::size_t count; };
struct Request {
    net::Socket descriptor;
    const http::Transport* transport;
    void* connection{};
    int64_t handshakeExpires{};
    bool handshaken{};
    uintptr_t monitor;
    int status;
    const void* method{};
    const void* target{};
    const void* body{};
    RoutePattern path{};
    Field* query{};
    std::size_t queryCount{};
    const RoutePattern* route{};
    Field* headers{};
    std::size_t headerCount{};
    std::atomic<bool> locallyClosed{};
    SocketAccess access{};
};
constexpr std::size_t maxWire = 256 * 1024;
constexpr std::size_t maxBody = 128 * 1024;
constexpr std::size_t maxTarget = 8192;
constexpr std::size_t maxHeaderBytes = 32 * 1024;
constexpr std::size_t maxHeaderFields = 100;
constexpr int readChunk = 8192;
constexpr int64_t acceptInterval = 20;
constexpr int64_t messageTimeout = 30000;
constexpr int unsupportedExpectation = 1000004;

struct HeaderSpan { std::size_t nameSize{}, valueSize{}; };
struct ResponseFields { Field* fields; std::size_t count; std::size_t wireSize; };

struct Parsed {
    Request* request;
    char* target;
    char* body;
    std::size_t targetSize{}, bodySize{};
    char* headerBytes;
    std::size_t headerSize{}, headerCount{};
    std::array<HeaderSpan, maxHeaderFields> headers;
    int error{};
    bool headersComplete{};
    bool complete{};
};

struct ParseState {
    llhttp_t parser{};
    Parsed parsed{};
    std::size_t total{};
    char* buffer{};
};

struct AcceptCursor {
    const Endpoint* listener;
    int64_t deadline;
    Request* request{};
    ParseState state{};
    int64_t messageExpires{};
};

int receive(Request* request, char* data, int size, int& count, int64_t deadline) {
    if (request->transport) return request->transport->read(request->connection, data, size, &count, deadline);
    for (;;) {
        if (auto status = waitFor(request->descriptor, POLLIN, deadline)) return status;
        count = net::receive(request->descriptor, data, size);
        if (count >= 0) return 0;
        auto status = net::error();
        if (!net::retry(status)) return status;
    }
}

int sendAll(Request* request, const char* data, std::size_t size, int64_t deadline) {
    while (size) {
        if (lyraBlockingCancelled()) return ECANCELED;
        if (now() > deadline) return ETIMEDOUT;
        auto count = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
        int sent;
        if (request->transport) {
            if (auto status = request->transport->write(request->connection, data, count, &sent, deadline)) return status;
        } else {
            sent = net::send(request->descriptor, data, count);
            if (sent < 0) {
                auto status = net::error();
                if (status == EINTR) continue;
                if (!net::retry(status)) return status;
                if (auto error = waitFor(request->descriptor, POLLOUT, deadline)) return error;
                continue;
            }
        }
        if (sent == 0) return EPIPE;
        data += sent;
        size -= sent;
    }
    return 0;
}

int append(Parsed& parsed, char*& storage, std::size_t& used, std::size_t limit, const char* data, std::size_t size) {
    if (size > limit - used) { parsed.error = EMSGSIZE; return -1; }
    if (!storage && size) storage = static_cast<char*>(lyraGcAllocAtomic(limit));
    if (size) std::memcpy(storage + used, data, size);
    used += size;
    return 0;
}

int appendHeader(llhttp_t* parser, const char* data, std::size_t size, bool name) {
    auto& parsed = *static_cast<Parsed*>(parser->data);
    if (parsed.headersComplete) return 0;
    if (parsed.headerCount == maxHeaderFields) { parsed.error = EMSGSIZE; return -1; }
    auto& header = parsed.headers[parsed.headerCount];
    (name ? header.nameSize : header.valueSize) += size;
    return append(parsed, parsed.headerBytes, parsed.headerSize, maxHeaderBytes, data, size);
}

int parseHeaders(const llhttp_t& parser, const Parsed& parsed);

constexpr auto parserSettings = [] {
    llhttp_settings_t config{};
    config.on_url = [](llhttp_t* parser, const char* bytes, std::size_t count) {
        auto& parsed = *static_cast<Parsed*>(parser->data);
        return append(parsed, parsed.target, parsed.targetSize, maxTarget, bytes, count);
    };
    config.on_body = [](llhttp_t* parser, const char* bytes, std::size_t count) {
        auto& parsed = *static_cast<Parsed*>(parser->data);
        return append(parsed, parsed.body, parsed.bodySize, maxBody, bytes, count);
    };
    config.on_header_field = [](llhttp_t* parser, const char* bytes, std::size_t count) { return appendHeader(parser, bytes, count, true); };
    config.on_header_value = [](llhttp_t* parser, const char* bytes, std::size_t count) { return appendHeader(parser, bytes, count, false); };
    config.on_header_value_complete = [](llhttp_t* parser) {
        auto& parsed = *static_cast<Parsed*>(parser->data);
        if (!parsed.headersComplete) ++parsed.headerCount;
        return 0;
    };
    config.on_headers_complete = [](llhttp_t* parser) {
        auto& parsed = *static_cast<Parsed*>(parser->data);
        parsed.headersComplete = true;
        parsed.error = parseHeaders(*parser, parsed);
        return parsed.error ? -1 : 0;
    };
    config.on_message_complete = [](llhttp_t* parser) { static_cast<Parsed*>(parser->data)->complete = true; return static_cast<int>(HPE_PAUSED); };
    return config;
}();

std::string_view text(const void* value) { return {lyraStringBytes(value), lyraStringLength(value)}; }

std::string_view first(std::string_view input, std::size_t end) { return {input.data(), std::min(end, input.size())}; }

bool fieldEqual(std::string_view left, std::string_view right) {
    auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
    return std::ranges::equal(left, right, [&](char leftByte, char rightByte) { return lower(leftByte) == lower(rightByte); });
}

std::string_view trimField(std::string_view value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == value.npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t") + 1 - begin);
}

bool validFieldValue(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char byte) { return (byte >= 0x20 && byte != 0x7f) || byte == '\t'; });
}

bool validFieldName(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            std::string_view("!#$%&'*+-.^_`|~").find(byte) != std::string_view::npos;
    });
}

int hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

const void* decoded(std::string_view input, bool query) {
    char bytes[maxTarget];
    std::size_t size = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto ch = input[index];
        if (ch == '%') {
            if (input.size() - index < 3) return nullptr;
            auto high = hex(input[index + 1]), low = hex(input[index + 2]);
            if (high < 0 || low < 0) return nullptr;
            ch = static_cast<char>(high * 16 + low);
            index += 2;
        } else if (query && ch == '+') ch = ' ';
        bytes[size++] = ch;
    }
    return lyraUtf8Valid(bytes, size) ? lyraStringFromBytes(bytes, size) : nullptr;
}

bool identifier(std::string_view input) {
    auto letter = [](char ch) { return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); };
    if (input.empty() || !letter(input.front())) return false;
    input.remove_prefix(1);
    return std::ranges::all_of(input, [&](char ch) { return letter(ch) || (ch >= '0' && ch <= '9'); });
}

template<class Visit>
bool split(std::string_view input, char separator, Visit visit) {
    for (;;) {
        auto end = input.find(separator);
        if (!visit(first(input, end))) return false;
        if (end == input.npos) return true;
        input.remove_prefix(end + 1);
    }
}

bool pathParts(RoutePattern& result, std::string_view input, bool pattern) {
    if (!input.starts_with('/') || input.find('#') != input.npos) return false;
    input.remove_prefix(1);
    auto capacity = static_cast<std::size_t>(std::ranges::count(input, '/')) + 1;
    result.segments = static_cast<Segment*>(lyraGcAlloc(capacity * sizeof(Segment)));
    return split(input, '/', [&](std::string_view part) {
        bool parameter = pattern && part.find_first_of("{}") != part.npos;
        if (parameter) {
            if (!part.starts_with('{') || !part.ends_with('}')) return false;
            part.remove_prefix(1);
            part.remove_suffix(1);
            if (!identifier(part)) return false;
            for (std::size_t index = 0; index < result.count; ++index)
                if (result.segments[index].parameter && text(result.segments[index].text) == part) return false;
        }
        auto value = decoded(part, false);
        if (!value) return false;
        result.segments[result.count++] = {value, parameter};
        return true;
    });
}

bool parseTarget(Request* request, std::string_view target) {
    if (target.find('#') != target.npos) return false;
    auto question = target.find('?');
    if (!pathParts(request->path, first(target, question), false)) return false;
    if (question == target.npos) return true;
    target.remove_prefix(question + 1);
    auto capacity = static_cast<std::size_t>(std::ranges::count(target, '&')) + 1;
    request->query = static_cast<Field*>(lyraGcAlloc(capacity * sizeof(Field)));
    return split(target, '&', [&](std::string_view field) {
        if (field.empty()) return true;
        auto equal = field.find('=');
        auto key = decoded(first(field, equal), true);
        if (equal == field.npos) field = {};
        else field.remove_prefix(equal + 1);
        auto value = decoded(field, true);
        if (!key || !value) return false;
        request->query[request->queryCount++] = {key, value};
        return true;
    });
}

bool validPort(std::string_view suffix) {
    if (suffix.empty()) return true;
    if (suffix.front() != ':') return false;
    suffix.remove_prefix(1);
    unsigned port;
    auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), port);
    return error == std::errc{} && end == suffix.data() + suffix.size() && port <= UINT16_MAX;
}

bool validHost(std::string_view authority) {
    if (authority.starts_with('[')) {
        auto end = authority.find(']');
        if (end <= 1 || end > INET6_ADDRSTRLEN || !validPort(authority.substr(end + 1))) return false;
        char literal[INET6_ADDRSTRLEN]{};
        std::memcpy(literal, authority.data() + 1, end - 1);
        in6_addr address;
        return uv_inet_pton(AF_INET6, literal, &address) == 0;
    }
    auto separator = authority.find(':');
    if (separator != authority.npos && !validPort(authority.substr(separator))) return false;
    auto host = first(authority, separator);
    if (host.ends_with('.')) host.remove_suffix(1);
    if (host.size() > 253) return false;
    return split(host, '.', [](std::string_view label) {
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        return std::ranges::all_of(label, [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-';
        });
    });
}

int parseHeaders(const llhttp_t& parser, const Parsed& parsed) {
    if (parser.http_major != 1 || parser.http_minor > 1) return EPROTO;
    auto* request = parsed.request;
    if (parsed.headerCount) request->headers = static_cast<Field*>(lyraGcAlloc(parsed.headerCount * sizeof(Field)));
    bool host = false, expect = false;
    std::size_t start = 0;
    for (std::size_t index = 0; index < parsed.headerCount; ++index) {
        auto [nameSize, valueSize] = parsed.headers[index];
        std::string_view name{parsed.headerBytes + start, nameSize};
        auto value = trimField({name.data() + nameSize, valueSize});
        if (!validFieldName(name) || !validFieldValue(value)) return EPROTO;
        if (!lyraUtf8Valid(value.data(), value.size())) return EILSEQ;
        if (fieldEqual(name, "Host")) {
            if (host || !validHost(value)) return EPROTO;
            host = true;
        }
        if (fieldEqual(name, "Expect")) expect = true;
        request->headers[request->headerCount++] = {
            lyraStringFromBytes(name.data(), nameSize), lyraStringFromBytes(value.data(), value.size()),
        };
        start += nameSize + valueSize;
    }
    if (parser.http_minor == 1 && !host) return EPROTO;
    return expect ? unsupportedExpectation : 0;
}

void stop(net::Socket& descriptor) {
    if (descriptor != net::invalidSocket) { net::close(descriptor); descriptor = net::invalidSocket; }
}

void stop(Request* request) {
    request->locallyClosed.store(true, std::memory_order_release);
    if (request->connection) {
        request->transport->releaseConnection(request->connection);
        request->connection = nullptr;
    }
    if (request->descriptor != net::invalidSocket) net::shutdownConnection(request->descriptor, request->monitor);
    stop(request->descriptor);
    net::closeMonitor(request->monitor);
    request->monitor = 0;
}

void initialize(ParseState& state, Request* request) {
    state.parsed = {.request = request};
    state.total = 0;
    if (!state.buffer) state.buffer = static_cast<char*>(lyraGcAllocAtomic(readChunk));
    llhttp_init(&state.parser, HTTP_REQUEST, &parserSettings);
    state.parser.data = &state.parsed;
}

int parse(Request* request, ParseState& state, int64_t deadline) {
    auto& [parser, parsed, total, buffer] = state;
    while (!parsed.complete) {
        int size;
        if (auto status = receive(request, buffer, readChunk, size, deadline)) return status;
        if (size == 0) return ECONNRESET;
        total += static_cast<std::size_t>(size);
        if (total > maxWire) return EMSGSIZE;
        auto code = llhttp_execute(&parser, buffer, static_cast<std::size_t>(size));
        if (code != HPE_OK && !(code == HPE_PAUSED && parsed.complete)) return parsed.error ? parsed.error : EPROTO;
    }
    if (!parsed.targetSize || parsed.target[0] != '/') return EPROTO;
    if (!lyraUtf8Valid(parsed.target, parsed.targetSize) || !parseTarget(request, {parsed.target, parsed.targetSize})) return EPROTO;
    auto method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(&parser)));
    request->method = lyraStringFromBytes(method, std::strlen(method));
    request->target = lyraStringFromBytes(parsed.target, parsed.targetSize);
    request->body = lyraStringFromBytes(parsed.body, parsed.bodySize);
    return 0;
}

Request* result(const Endpoint* server, net::Socket descriptor, int status) {
    auto* request = ::new (lyraGcAlloc(sizeof(Request))) Request{
        .descriptor = descriptor, .transport = server->transport, .handshaken = !server->transport, .monitor = 0, .status = status,
    };
    if (descriptor != net::invalidSocket) {
        request->monitor = net::disconnectMonitor(descriptor);
        if (!request->monitor) {
            request->status = net::error();
            stop(request);
        } else if (server->transport) {
            request->handshakeExpires = now() + server->handshakeTimeoutMs;
            if (auto opened = server->transport->open(server->listener, descriptor, &request->connection)) {
                request->status = opened == ECANCELED ? opened : http::handshakeFailed;
                stop(request);
            }
        }
    }
    return request;
}

int handshake(Request* request, int64_t sliceEnd) {
    auto deadline = std::min(sliceEnd, request->handshakeExpires);
    auto status = request->transport->handshake(request->connection, deadline);
    if (status == 0) { request->handshaken = true; return 0; }
    if (status == ETIMEDOUT) return request->handshakeExpires <= sliceEnd ? http::handshakeTimeout : ETIMEDOUT;
    return status == ECANCELED ? status : http::handshakeFailed;
}

net::Socket accept(const Endpoint* server, int64_t deadline, int& error) {
    for (;;) {
        if (server->draining.load(std::memory_order_acquire)) { error = EBADF; return net::invalidSocket; }
        auto until = std::min(deadline, now() + acceptInterval);
        error = waitFor(server->descriptor, POLLIN, until);
        if (error == ETIMEDOUT && until < deadline) continue;
        if (error) return net::invalidSocket;
        if (server->draining.load(std::memory_order_acquire)) { error = EBADF; return net::invalidSocket; }
        auto descriptor = net::accept(server->descriptor);
        if (descriptor != net::invalidSocket) return descriptor;
        error = net::error();
        if (!net::retry(error)) return descriptor;
    }
}

void reject(Request* request) {
    if (request->handshaken) {
        const char* response = nullptr;
        switch (request->status) {
            case EPROTO: case EILSEQ: response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; break;
            case EMSGSIZE: response = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; break;
            case unsupportedExpectation: response = "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; break;
        }
        auto deadline = now() + acceptInterval;
        if (response && sendAll(request, response, std::strlen(response), deadline) == 0 && request->transport)
            request->transport->shutdown(request->connection, deadline);
    }
    stop(request);
}

const char* reason(int64_t status) {
    switch (status) {
        case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content";
        case 206: return "Partial Content"; case 301: return "Moved Permanently"; case 302: return "Found";
        case 303: return "See Other"; case 304: return "Not Modified"; case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect"; case 400: return "Bad Request"; case 401: return "Unauthorized";
        case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 408: return "Request Timeout"; case 409: return "Conflict"; case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type"; case 417: return "Expectation Failed"; case 422: return "Unprocessable Content"; case 429: return "Too Many Requests";
        case 500: return "Internal Server Error"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout"; default: return "Response";
    }
}

}

extern "C" void* lyraHttpListenFailure(int64_t status, const void* detail) {
    return ::new (lyraGcAlloc(sizeof(Endpoint))) Endpoint{.status = status, .detail = detail};
}

extern "C" void* lyraHttpListenWithTransport(const void* host, int64_t port,
    const http::Transport* transport, void* state, int64_t handshakeTimeoutMs) {
    auto fail = [&](int status) {
        if (transport) transport->releaseListener(state);
        return lyraHttpListenFailure(status, nullptr);
    };
    auto hostText = text(host);
    if (port < 0 || port > UINT16_MAX || hostText.empty() || hostText.find('\0') != hostText.npos ||
        (transport && (handshakeTimeoutMs < 1 || handshakeTimeoutMs > INT_MAX))) return fail(EINVAL);
    std::string name(hostText);
    sockaddr_in address4{};
    sockaddr_in6 address6{};
    sockaddr* address;
    net::AddressLength length;
    if (uv_ip4_addr(name.c_str(), static_cast<int>(port), &address4) == 0) {
        address = reinterpret_cast<sockaddr*>(&address4);
        length = sizeof(address4);
    } else if (uv_ip6_addr(name.c_str(), static_cast<int>(port), &address6) == 0) {
        address = reinterpret_cast<sockaddr*>(&address6);
        length = sizeof(address6);
    } else return fail(EINVAL);
    auto family = address->sa_family;
    auto descriptor = net::open(family);
    if (descriptor == net::invalidSocket) return fail(net::error());
    constexpr int enabled = 1;
    auto option = reinterpret_cast<const char*>(&enabled);
#ifdef _WIN32
    constexpr int bindOption = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int bindOption = SO_REUSEADDR;
#endif
    if (setsockopt(descriptor, SOL_SOCKET, bindOption, option, sizeof(enabled)) ||
        (family == AF_INET6 && setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, option, sizeof(enabled))) ||
        ::bind(descriptor, address, length) || ::listen(descriptor, 64) || ::getsockname(descriptor, address, &length)) {
        auto status = net::error();
        net::close(descriptor);
        return fail(status);
    }
    auto bound = ntohs(family == AF_INET ? address4.sin_port : address6.sin6_port);
    return ::new (lyraGcAlloc(sizeof(Endpoint))) Endpoint{
        .descriptor = descriptor, .port = bound, .transport = transport, .listener = state, .handshakeTimeoutMs = handshakeTimeoutMs,
    };
}

extern "C" void* lyraHttpListen(const void* host, int64_t port) {
    return lyraHttpListenWithTransport(host, port, nullptr, nullptr, 0);
}
extern "C" int64_t lyraHttpPort(const Endpoint* server) { return server->port; }
extern "C" int64_t lyraHttpStatus(const Endpoint* server) { return server->status; }
extern "C" const void* lyraHttpStatusDetail(const Endpoint* server) {
    return server->detail ? server->detail : lyraStringFromBytes(nullptr, 0);
}
extern "C" void lyraHttpBeginDrain(Endpoint* server) { server->draining.store(true, std::memory_order_release); }
extern "C" int64_t lyraHttpStop(Endpoint* server) {
    SocketLease lease(server->access);
    if (lease.status == EBADF) return 0;
    if (lease.status) return lease.status;
    stop(server->descriptor);
    if (server->transport) server->transport->releaseListener(server->listener);
    server->listener = nullptr;
    lease.markClosed();
    return 0;
}

extern "C" int64_t lyraHttpErrorKind(int64_t status) {
    switch (status) {
        case EINVAL: return 1;
        case EBADF: return 2;
        case ETIMEDOUT: return 3;
        case EPROTO: return 4;
        case EMSGSIZE: return 5;
        case EILSEQ: return 6;
        case http::tlsConfig: return 7;
        case http::handshakeFailed: return 8;
        case http::handshakeTimeout: return 9;
        case EBUSY: return 10;
        case unsupportedExpectation: return 11;
        default: return 0;
    }
}

extern "C" Request* lyraHttpAccept(const Endpoint* server, int64_t timeoutMs) {
    if (timeoutMs < 0 || timeoutMs > INT_MAX) return result(server, net::invalidSocket, EINVAL);
    SocketLease lease(server->access, true);
    if (lease.status) return result(server, net::invalidSocket, lease.status);
    auto deadline = now() + timeoutMs;
    int error = 0;
    auto descriptor = accept(server, deadline, error);
    auto request = result(server, descriptor, error);
    if (request->status) return request;
    if (!request->handshaken) {
        request->status = handshake(request, deadline);
        if (request->status) {
            if (request->status == ETIMEDOUT) request->status = http::handshakeTimeout;
            stop(request);
            return request;
        }
    }
    ParseState state;
    initialize(state, request);
    request->status = parse(request, state, deadline);
    if (request->status) reject(request);
    return request;
}

extern "C" AcceptCursor* lyraHttpAcceptCursor(const Endpoint* server, int64_t timeoutMs) {
    return ::new (lyraGcAlloc(sizeof(AcceptCursor))) AcceptCursor{server, timeoutMs < 0 ? INT64_MAX : now() + timeoutMs};
}

extern "C" int64_t lyraHttpAcceptAdvance(AcceptCursor* cursor) {
    if (cursor->listener->draining.load(std::memory_order_acquire)) return -EBADF;
    SocketLease lease(cursor->listener->access, true);
    if (lease.status) return -lease.status;
    const bool single = cursor->deadline != INT64_MAX;
    auto report = [&](int status) -> int64_t { return single ? -static_cast<int64_t>(status) : 0; };
    const auto until = std::min(cursor->deadline, now() + acceptInterval);
    if (!cursor->request) {
        int error = 0;
        auto descriptor = accept(cursor->listener, until, error);
        if (error == ETIMEDOUT) return until == cursor->deadline ? report(error) : 0;
        if (error == ECONNABORTED || error == ECONNRESET) return report(error);
        if (error) return -static_cast<int64_t>(error);
        cursor->request = result(cursor->listener, descriptor, 0);
        if (auto status = cursor->request->status) { cursor->request = nullptr; return report(status); }
        cursor->messageExpires = single ? cursor->deadline : now() + messageTimeout;
        initialize(cursor->state, cursor->request);
    }
    auto request = cursor->request;
    if (!request->handshaken) {
        auto status = handshake(request, until);
        if (status == ETIMEDOUT && until < cursor->deadline) return 0;
        if (status != 0) {
            stop(request);
            cursor->request = nullptr;
            cursor->state.parsed = {};
            return report(status == ETIMEDOUT ? http::handshakeTimeout : status);
        }
        if (!single) cursor->messageExpires = now() + messageTimeout;
    }
    request->status = parse(request, cursor->state, std::min(cursor->messageExpires, until));
    if (request->status == 0) { cursor->state.parsed = {}; return 1; }
    if (request->status == ETIMEDOUT && now() < cursor->messageExpires) return 0;
    reject(request);
    cursor->request = nullptr;
    cursor->state.parsed = {};
    return report(request->status);
}

extern "C" Request* lyraHttpAcceptTake(AcceptCursor* cursor) {
    if (cursor->listener->draining.load(std::memory_order_acquire)) {
        auto request = cursor->request;
        cursor->request = nullptr;
        if (request) stop(request);
        return nullptr;
    }
    auto request = cursor->request;
    cursor->request = nullptr;
    return request;
}

extern "C" void lyraHttpAcceptDrop(AcceptCursor* cursor) {
    if (cursor->request) stop(cursor->request);
    cursor->request = nullptr;
}

extern "C" int64_t lyraHttpRequestStatus(const Request* req) { return req->status; }
extern "C" const void* lyraHttpMethod(const Request* req) { return req->method; }
extern "C" const void* lyraHttpTarget(const Request* req) { return req->target; }
extern "C" const void* lyraHttpBody(const Request* req) { return req->body; }
extern "C" int64_t lyraHttpQueryCount(const Request* req) { return req->queryCount; }
extern "C" const void* lyraHttpQueryName(const Request* req, int64_t index) { return req->query[index].name; }
extern "C" const void* lyraHttpQueryValue(const Request* req, int64_t index) { return req->query[index].value; }
extern "C" int64_t lyraHttpHeaderCount(const Request* req) { return req->headerCount; }
extern "C" uint8_t lyraHttpHeaderMatches(const Request* req, int64_t index, const void* name) {
    return fieldEqual(text(req->headers[index].name), text(name));
}
extern "C" const void* lyraHttpHeaderValue(const Request* req, int64_t index) { return req->headers[index].value; }
extern "C" const void* lyraHttpCookie(const Request* req, const void* name) {
    auto key = text(name);
    for (std::size_t index = 0; index < req->headerCount; ++index) {
        auto& field = req->headers[index];
        if (!fieldEqual(text(field.name), "Cookie")) continue;
        const void* found = nullptr;
        split(text(field.value), ';', [&](std::string_view item) {
            auto equal = item.find('=');
            if (equal == item.npos || trimField(first(item, equal)) != key || key.empty()) return true;
            auto value = trimField(item.substr(equal + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            found = lyraStringFromBytes(value.data(), value.size());
            return false;
        });
        if (found) return found;
    }
    return nullptr;
}
extern "C" const void* lyraHttpCookieValue(const void* value) { return value; }
extern "C" uint8_t lyraHttpRouteMethodValid(const void* method) {
    auto name = text(method);
    if (name == "CONNECT") return false;
#define LYRA_HTTP_METHOD(number, identifier, spelling) if (name == #spelling) return true;
    HTTP_METHOD_MAP(LYRA_HTTP_METHOD)
#undef LYRA_HTTP_METHOD
    return false;
}
extern "C" RoutePattern* lyraHttpRoutePattern(const void* route) {
    auto* pattern = ::new (lyraGcAlloc(sizeof(RoutePattern))) RoutePattern{};
    auto source = text(route);
    if (source.size() > maxTarget || source.find('?') != source.npos || !pathParts(*pattern, source, true)) pattern->count = 0;
    return pattern;
}

extern "C" uint8_t lyraHttpRouteValid(const RoutePattern* route) { return route->count > 0; }

extern "C" uint8_t lyraHttpRouteMatch(const Request* req, const RoutePattern* route) {
    if (req->path.count != route->count) return false;
    for (std::size_t index = 0; index < route->count; ++index) {
        auto part = route->segments[index];
        auto received = req->path.segments[index].text;
        if (part.parameter ? lyraStringLength(received) == 0 : !lyraStringEqual(part.text, received)) return false;
    }
    return true;
}

extern "C" void lyraHttpRouteBind(Request* req, const RoutePattern* route) { req->route = route; }

extern "C" int64_t lyraHttpPathIndex(const Request* req, const void* name) {
    if (req->route) for (std::size_t index = 0; index < req->route->count; ++index) {
        auto part = req->route->segments[index];
        if (part.parameter && lyraStringEqual(part.text, name)) return static_cast<int64_t>(index);
    }
    return -1;
}

extern "C" const void* lyraHttpPathValue(const Request* req, int64_t index) { return req->path.segments[index].text; }
extern "C" PathInteger* lyraHttpParseInt(const void* value) {
    auto input = text(value);
    auto* integer = ::new (lyraGcAllocAtomic(sizeof(PathInteger))) PathInteger{};
    auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), integer->value);
    integer->valid = error == std::errc{} && end == input.data() + input.size();
    return integer;
}
extern "C" uint8_t lyraHttpIntegerValid(const PathInteger* parsed) { return parsed->valid; }
extern "C" int64_t lyraHttpIntegerValue(const PathInteger* parsed) { return parsed->value; }
extern "C" uint8_t lyraHttpWatchMonitor(void* request, int (*watch)(uintptr_t, void*, int (*)(const void*))) {
    auto* req = static_cast<Request*>(request);
    SocketLease lease(req->access, true);
    return !lease.status && watch(req->monitor, req, [](const void* target) -> int {
        return static_cast<const Request*>(target)->locallyClosed.load(std::memory_order_acquire);
    }) != 0;
}

extern "C" int64_t lyraHttpDrop(Request* req) {
    SocketLease lease(req->access);
    if (lease.status == EBADF) return 0;
    if (lease.status) return lease.status;
    stop(req);
    lease.markClosed();
    return 0;
}

extern "C" int64_t lyraHttpDisconnected(const Request* req) {
    SocketLease lease(req->access, true);
    if (lease.status == EBADF) return 1;
    if (lease.status) return -lease.status;
    return net::disconnected(req->descriptor);
}

extern "C" int64_t lyraHttpWaitDisconnect(const Request* req, int64_t timeoutMs) {
    SocketLease lease(req->access, true);
    if (lease.status == EBADF) return 1;
    if (lease.status) return -lease.status;
    if (timeoutMs < 0 || timeoutMs > INT_MAX) return 0;
    return net::waitDisconnect(req->monitor, static_cast<int>(timeoutMs));
}

extern "C" ResponseFields* lyraHttpResponseHeaders(int64_t count) {
    if (count < 0 || count > static_cast<int64_t>(maxHeaderFields)) return nullptr;
    auto* fields = count ? static_cast<Field*>(lyraGcAlloc(count * sizeof(Field))) : nullptr;
    return ::new (lyraGcAlloc(sizeof(ResponseFields))) ResponseFields{fields, static_cast<std::size_t>(count), 0};
}

extern "C" int64_t lyraHttpResponseHeaderSet(ResponseFields* headers, int64_t index, const void* fieldName, const void* fieldValue) {
    auto name = text(fieldName);
    auto value = text(fieldValue);
    constexpr std::array reserved{"Connection", "Content-Length", "Content-Type", "Transfer-Encoding", "Keep-Alive",
        "Upgrade", "Trailer", "TE", "Proxy-Connection"};
    if (!validFieldName(name) || !validFieldValue(value) ||
        std::ranges::any_of(reserved, [&](auto header) { return fieldEqual(name, header); })) return EINVAL;
    auto remaining = maxHeaderBytes - headers->wireSize;
    constexpr std::size_t delimiters = 4;
    if (remaining < delimiters || name.size() > remaining - delimiters || value.size() > remaining - delimiters - name.size()) return EMSGSIZE;
    headers->wireSize += name.size() + value.size() + delimiters;
    headers->fields[index] = {fieldName, fieldValue};
    return 0;
}

extern "C" int64_t lyraHttpRespond(Request* req, int64_t status, const void* mediaType, const void* body, const ResponseFields* headers) {
    SocketLease lease(req->access);
    if (lease.status) return lease.status;
    auto type = text(mediaType);
    auto length = lyraStringLength(body);
    auto noBody = status == 204 || status == 304;
    if (status < 200 || status > 599 || type.empty() || type.size() > 200 || ((noBody || status == 205) && length) ||
        !std::ranges::all_of(type, [](unsigned char byte) { return byte >= 0x20 && byte <= 0x7e; })) return EINVAL;
    char basic[512];
    auto count = std::snprintf(basic, sizeof(basic), "HTTP/1.1 %lld %s\r\nContent-Type: %.*s\r\nConnection: close\r\n",
        static_cast<long long>(status), reason(status), static_cast<int>(type.size()), type.data());
    std::string wire(basic, count);
    wire.reserve(wire.size() + 40 + headers->wireSize);
    if (!noBody) {
        auto fieldSize = std::snprintf(basic, sizeof(basic), "Content-Length: %zu\r\n", length);
        wire.append(basic, fieldSize);
    }
    for (std::size_t index = 0; index < headers->count; ++index) {
        const auto& [name, value] = headers->fields[index];
        wire.append(text(name)).append(": ").append(text(value)).append("\r\n");
    }
    wire.append("\r\n");
    auto deadline = now() + 5000;
    auto error = sendAll(req, wire.data(), wire.size(), deadline);
    if (!error && text(req->method) != "HEAD") error = sendAll(req, lyraStringBytes(body), length, deadline);
    if (!error && req->transport) req->transport->shutdown(req->connection, deadline);
    stop(req);
    lease.markClosed();
    return error;
}
