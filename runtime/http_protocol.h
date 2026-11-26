#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <llhttp.h>
#include <string>
#include <string_view>

#include "net.h"

namespace lyraRuntime::http {

struct Endpoint;
struct Connection;
struct Reactor;
struct Field { const void* name; const void* value; };
struct Segment { const void* text; bool parameter; };
struct RoutePattern { Segment* segments; std::size_t count; };
struct ResponseFields { Field* fields; std::size_t count; std::size_t wireSize; };

enum class Delivery { Pending, Offered, Claimed, Closing, Closed };

struct Request {
    Endpoint* endpoint{};
    Reactor* reactor{};
    Connection* connection{};
    net::Socket socket{net::invalidSocket};
    std::atomic<Delivery> delivery{Delivery::Pending};
    std::atomic<bool> peerLost{};
    bool overload{};
    int ingressError{};
    int64_t acceptedAt{}, offerExpires{};
    int status{};
    const void* method{};
    const void* target{};
    const void* body{};
    RoutePattern path{};
    Field* query{};
    std::size_t queryCount{};
    std::atomic<const RoutePattern*> route{};
    Field* headers{};
    std::size_t headerCount{};
};

inline constexpr std::size_t maxHeaderFields = 100, maxWire = 256 * 1024;
inline constexpr int unsupportedExpectation = 1000004;

struct RequestParser {
    struct HeaderSpan { std::size_t nameSize{}, valueSize{}; };
    llhttp_t parser{};
    Request& request;
    std::string target, body, headerBytes;
    std::array<HeaderSpan, maxHeaderFields> headers{};
    std::size_t total{}, headerCount{};
    int error{};
    bool headersComplete{}, complete{};

    explicit RequestParser(Request&);
    int feed(std::string_view bytes);
    RequestParser(const RequestParser&) = delete;
};

std::string_view text(const void* value);
int formatResponse(std::string& wire, int64_t status, const void* type, const void* body, const ResponseFields& headers);

}
