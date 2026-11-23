#include "managed.h"
#include "net.h"
#include "strings.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <llhttp.h>

namespace {

namespace net = lyraRuntime::net;
struct Endpoint { net::Socket descriptor; int port; int status; };
struct Request { net::Socket descriptor; uintptr_t monitor; int status; const void* method; const void* target; const void* body; };
constexpr std::size_t maxWire = 256 * 1024;
constexpr std::size_t maxBody = 128 * 1024;
constexpr std::size_t maxTarget = 8192;

struct Parsed {
    char* target;
    char* body;
    std::size_t targetSize{}, bodySize{};
    int error{};
    bool complete{};
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

void stop(net::Socket& descriptor) {
    if (descriptor != net::invalidSocket) { net::close(descriptor); descriptor = net::invalidSocket; }
}

void stop(Request* request) {
    stop(request->descriptor);
    net::closeMonitor(request->monitor);
    request->monitor = 0;
}

int parse(Request* request, int64_t deadline) {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = [](llhttp_t* parser, const char* bytes, std::size_t count) { return append(parser, bytes, count, true); };
    settings.on_body = [](llhttp_t* parser, const char* bytes, std::size_t count) { return append(parser, bytes, count, false); };
    settings.on_message_complete = [](llhttp_t* parser) { static_cast<Parsed*>(parser->data)->complete = true; return static_cast<int>(HPE_PAUSED); };
    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    Parsed parsed{};
    parser.data = &parsed;
    std::size_t total = 0;
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
    if (!lyraUtf8Valid(parsed.target, parsed.targetSize) || !lyraUtf8Valid(parsed.body, parsed.bodySize)) return EILSEQ;
    auto method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(&parser)));
    request->method = lyraStringFromBytes(method, std::strlen(method));
    request->target = lyraStringFromBytes(parsed.target, parsed.targetSize);
    request->body = lyraStringFromBytes(parsed.body, parsed.bodySize);
    return 0;
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
    auto request = static_cast<Request*>(lyraGcAlloc(sizeof(Request)));
    auto empty = lyraStringFromBytes(nullptr, 0);
    *request = {net::invalidSocket, 0, 0, empty, empty, empty};
    if (server->descriptor == net::invalidSocket || timeoutMs < 0 || timeoutMs > INT_MAX) { request->status = EINVAL; return request; }
    auto deadline = now() + timeoutMs;
    for (;;) {
        if (auto error = waitFor(server->descriptor, POLLIN, deadline)) { request->status = error; return request; }
        request->descriptor = net::accept(server->descriptor);
        if (request->descriptor != net::invalidSocket) break;
        auto status = net::error();
        if (!net::retry(status)) { request->status = status; return request; }
    }
    request->monitor = net::disconnectMonitor(request->descriptor);
    if (!request->monitor) { request->status = net::error(); stop(request); return request; }
    request->status = parse(request, deadline);
    if (request->status) {
        if (request->status == EPROTO || request->status == EILSEQ || request->status == EMSGSIZE) {
            const char* response = request->status == EMSGSIZE ? "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(request->descriptor, response, std::strlen(response), deadline);
        }
        stop(request);
    }
    return request;
}

extern "C" int64_t lyraHttpRequestStatus(const Request* req) { return req->status; }
extern "C" const void* lyraHttpMethod(const Request* req) { return req->method; }
extern "C" const void* lyraHttpTarget(const Request* req) { return req->target; }
extern "C" const void* lyraHttpBody(const Request* req) { return req->body; }
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
