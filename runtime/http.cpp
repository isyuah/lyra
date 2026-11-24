#include "managed.h"
#include "net.h"
#include "strings.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <llhttp.h>
#include <new>
#include <string_view>

namespace {

namespace net = lyraRuntime::net;
struct Endpoint { net::Socket descriptor; int port; int status; };
struct QueryField { const void* name; const void* value; };
struct Segment { const void* text; bool parameter; };
struct PathInteger { int64_t value; bool valid; };
struct RoutePattern { Segment* segments; std::size_t count; };
struct Request {
    net::Socket descriptor;
    uintptr_t monitor;
    int status;
    const void* method;
    const void* target;
    const void* body;
    RoutePattern path{};
    QueryField* query{};
    std::size_t queryCount{};
    const RoutePattern* route{};
};
constexpr std::size_t maxWire = 256 * 1024;
constexpr std::size_t maxBody = 128 * 1024;
constexpr std::size_t maxTarget = 8192;
constexpr int64_t acceptInterval = 20;
constexpr int64_t messageTimeout = 30000;

struct Parsed {
    char* target;
    char* body;
    std::size_t targetSize{}, bodySize{};
    int error{};
    bool complete{};
};

struct ParseState {
    llhttp_t parser{};
    Parsed parsed{};
    std::size_t total{};
};

struct AcceptCursor {
    const Endpoint* listener;
    Request* request{};
    ParseState state{};
    int64_t expires{};
};

int64_t now() { return static_cast<int64_t>(uv_hrtime() / UINT64_C(1000000)); }

int waitFor(net::Socket descriptor, short event, int64_t deadline) {
    net::PollSlot slot{descriptor, event, 0};
    for (;;) {
        auto remaining = deadline - now();
        if (remaining < 0) return ETIMEDOUT;
        auto result = net::poll(slot, static_cast<int>(remaining));
        if (result > 0) return 0;
        if (result == 0) return ETIMEDOUT;
        auto status = net::error();
        if (status != EINTR) return status;
    }
}

int sendAll(net::Socket descriptor, const char* data, std::size_t size, int64_t deadline) {
    while (size) {
        auto sent = net::send(descriptor, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)));
        if (sent > 0) { data += sent; size -= sent; continue; }
        if (sent == 0) return EPIPE;
        auto status = net::error();
        if (status == EINTR) continue;
        if (!net::retry(status)) return status;
        if (auto error = waitFor(descriptor, POLLOUT, deadline)) return error;
    }
    return 0;
}

int append(llhttp_t* parser, const char* data, std::size_t size, bool target) {
    auto parsed = static_cast<Parsed*>(parser->data);
    auto& used = target ? parsed->targetSize : parsed->bodySize;
    auto limit = target ? maxTarget : maxBody;
    if (size > limit - used) { parsed->error = EMSGSIZE; return -1; }
    auto& storage = target ? parsed->target : parsed->body;
    if (!storage && size) storage = static_cast<char*>(lyraGcAllocAtomic(limit));
    if (size) std::memcpy(storage + used, data, size);
    used += size;
    return 0;
}

constexpr auto parserSettings = [] {
    llhttp_settings_t config{};
    config.on_url = [](llhttp_t* parser, const char* bytes, std::size_t count) { return append(parser, bytes, count, true); };
    config.on_body = [](llhttp_t* parser, const char* bytes, std::size_t count) { return append(parser, bytes, count, false); };
    config.on_message_complete = [](llhttp_t* parser) { static_cast<Parsed*>(parser->data)->complete = true; return static_cast<int>(HPE_PAUSED); };
    return config;
}();

std::string_view text(const void* value) { return {lyraStringBytes(value), lyraStringLength(value)}; }

std::string_view first(std::string_view input, std::size_t end) { return {input.data(), std::min(end, input.size())}; }

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
    request->query = static_cast<QueryField*>(lyraGcAlloc(capacity * sizeof(QueryField)));
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

void stop(net::Socket& descriptor) {
    if (descriptor != net::invalidSocket) { net::close(descriptor); descriptor = net::invalidSocket; }
}

void stop(Request* request) {
    stop(request->descriptor);
    net::closeMonitor(request->monitor);
    request->monitor = 0;
}

void initialize(ParseState& state) {
    state.parsed = {};
    state.total = 0;
    llhttp_init(&state.parser, HTTP_REQUEST, &parserSettings);
    state.parser.data = &state.parsed;
}

int parse(Request* request, ParseState& state, int64_t deadline) {
    auto& [parser, parsed, total] = state;
    while (!parsed.complete) {
        if (auto error = waitFor(request->descriptor, POLLIN, deadline)) return error;
        char buffer[8192];
        auto size = net::receive(request->descriptor, buffer, sizeof(buffer));
        if (size == 0) return ECONNRESET;
        if (size < 0) { auto status = net::error(); if (net::retry(status)) continue; return status; }
        total += static_cast<std::size_t>(size);
        if (total > maxWire) return EMSGSIZE;
        auto code = llhttp_execute(&parser, buffer, static_cast<std::size_t>(size));
        if (code != HPE_OK && !(code == HPE_PAUSED && parsed.complete)) return parsed.error ? parsed.error : EPROTO;
    }
    if (parser.http_major != 1 || parser.http_minor > 1 || !parsed.targetSize || parsed.target[0] != '/') return EPROTO;
    if (!lyraUtf8Valid(parsed.target, parsed.targetSize) || !lyraUtf8Valid(parsed.body, parsed.bodySize) ||
        !parseTarget(request, {parsed.target, parsed.targetSize})) return EILSEQ;
    auto method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(&parser)));
    request->method = lyraStringFromBytes(method, std::strlen(method));
    request->target = lyraStringFromBytes(parsed.target, parsed.targetSize);
    request->body = lyraStringFromBytes(parsed.body, parsed.bodySize);
    return 0;
}

Request* result(net::Socket descriptor, int status) {
    auto empty = lyraStringFromBytes(nullptr, 0);
    auto* request = ::new (lyraGcAlloc(sizeof(Request))) Request{descriptor, 0, status, empty, empty, empty};
    if (descriptor != net::invalidSocket) {
        request->monitor = net::disconnectMonitor(descriptor);
        if (!request->monitor) {
            request->status = net::error();
            stop(request);
        }
    }
    return request;
}

net::Socket accept(const Endpoint* server, int64_t deadline, int& error) {
    if (server->descriptor == net::invalidSocket) { error = EINVAL; return net::invalidSocket; }
    for (;;) {
        if ((error = waitFor(server->descriptor, POLLIN, deadline))) return net::invalidSocket;
        auto descriptor = net::accept(server->descriptor);
        if (descriptor != net::invalidSocket) return descriptor;
        error = net::error();
        if (!net::retry(error)) return descriptor;
    }
}

void reject(Request* request, int64_t deadline) {
    if (request->status == EPROTO || request->status == EILSEQ || request->status == EMSGSIZE) {
        const char* response = request->status == EMSGSIZE ? "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(request->descriptor, response, std::strlen(response), deadline);
    }
    stop(request);
}

const char* reason(int64_t status) {
    switch (status) {
        case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
        case 400: return "Bad Request"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 413: return "Content Too Large"; case 500: return "Internal Server Error"; default: return "Response";
    }
}

}

extern "C" Endpoint* lyraHttpListen(int64_t port) {
    auto listener = static_cast<Endpoint*>(lyraGcAlloc(sizeof(Endpoint)));
    *listener = {net::invalidSocket, 0, EINVAL};
    if (port < 0 || port > UINT16_MAX) return listener;
    auto fd = net::open();
    if (fd == net::invalidSocket) { listener->status = net::error(); return listener; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    net::AddressLength length = sizeof(address);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) || ::listen(fd, 64) || ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length)) {
        listener->status = net::error(); net::close(fd); return listener;
    }
    *listener = {fd, ntohs(address.sin_port), 0};
    return listener;
}

extern "C" int64_t lyraHttpPort(const Endpoint* server) { return server->port; }
extern "C" int64_t lyraHttpStatus(const Endpoint* server) { return server->status; }
extern "C" void lyraHttpStop(Endpoint* server) { stop(server->descriptor); }

extern "C" Request* lyraHttpAccept(const Endpoint* server, int64_t timeoutMs) {
    if (timeoutMs < 0 || timeoutMs > INT_MAX) return result(net::invalidSocket, EINVAL);
    auto deadline = now() + timeoutMs;
    int error = 0;
    auto descriptor = accept(server, deadline, error);
    auto request = result(descriptor, error);
    if (request->status) return request;
    ParseState state;
    initialize(state);
    request->status = parse(request, state, deadline);
    if (request->status) reject(request, deadline);
    return request;
}

extern "C" AcceptCursor* lyraHttpAcceptCursor(const Endpoint* server) {
    return ::new (lyraGcAlloc(sizeof(AcceptCursor))) AcceptCursor{server};
}

extern "C" int64_t lyraHttpAcceptAdvance(AcceptCursor* cursor) {
    auto until = now() + acceptInterval;
    if (!cursor->request) {
        int error = 0;
        auto descriptor = accept(cursor->listener, until, error);
        if (error) return error == ETIMEDOUT || error == ECONNABORTED || error == ECONNRESET ? 0 : -static_cast<int64_t>(error);
        cursor->request = result(descriptor, 0);
        if (cursor->request->status) { cursor->request = nullptr; return 0; }
        cursor->expires = now() + messageTimeout;
        initialize(cursor->state);
    }
    auto request = cursor->request;
    request->status = parse(request, cursor->state, std::min(cursor->expires, until));
    if (request->status == 0) { cursor->state.parsed = {}; return 1; }
    if (request->status == ETIMEDOUT && now() < cursor->expires) return 0;
    reject(request, until);
    cursor->request = nullptr;
    cursor->state.parsed = {};
    return 0;
}

extern "C" Request* lyraHttpAcceptTake(AcceptCursor* cursor) {
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
extern "C" uintptr_t lyraHttpDisconnectMonitor(const void* request) { return static_cast<const Request*>(request)->monitor; }
extern "C" void lyraHttpDrop(Request* req) { stop(req); }

extern "C" uint8_t lyraHttpDisconnected(const Request* req) {
    return !req->monitor || net::disconnected(req->descriptor);
}

extern "C" uint8_t lyraHttpWaitDisconnect(const Request* req, int64_t timeoutMs) {
    if (!req->monitor) return true;
    if (timeoutMs < 0 || timeoutMs > INT_MAX) return false;
    return net::waitDisconnect(req->monitor, static_cast<int>(timeoutMs));
}

extern "C" int64_t lyraHttpRespond(Request* req, int64_t status, const void* mediaType, const void* body) {
    if (req->descriptor == net::invalidSocket) return EBADF;
    auto type = lyraStringBytes(mediaType);
    auto size = lyraStringLength(mediaType);
    auto length = lyraStringLength(body);
    if (status < 200 || status > 599 || !size || size > 200 || ((status == 204 || status == 304) && length)) return EINVAL;
    for (std::size_t index = 0; index < size; ++index) if (type[index] < 0x20 || type[index] > 0x7e) return EINVAL;
    char header[512];
    auto count = std::snprintf(header, sizeof(header), "HTTP/1.1 %lld %s\r\nContent-Type: %.*s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                               static_cast<long long>(status), reason(status), static_cast<int>(size), type, length);
    auto deadline = now() + 5000;
    auto error = sendAll(req->descriptor, header, static_cast<std::size_t>(count), deadline);
    bool head = lyraStringLength(req->method) == 4 && std::memcmp(lyraStringBytes(req->method), "HEAD", 4) == 0;
    if (!error && !head) error = sendAll(req->descriptor, lyraStringBytes(body), length, deadline);
    stop(req);
    return error;
}

extern "C" int64_t lyraHttpPathInt(const void* target, const void* prefix, int64_t fallback) {
    auto size = lyraStringLength(target), begin = lyraStringLength(prefix);
    auto bytes = lyraStringBytes(target);
    if (begin >= size || std::memcmp(bytes, lyraStringBytes(prefix), begin)) return fallback;
    if (auto query = static_cast<const char*>(std::memchr(bytes + begin, '?', size - begin))) size = static_cast<std::size_t>(query - bytes);
    if (size == begin) return fallback;
    int64_t value = 0;
    for (auto index = begin; index < size; ++index) {
        auto digit = bytes[index] - '0';
        if (digit < 0 || digit > 9 || value > (INT64_MAX - digit) / 10) return fallback;
        value = value * 10 + digit;
    }
    return value;
}
