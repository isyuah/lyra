#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <llhttp.h>
#include <span>
#include <string>
#include <string_view>

#include "net.h"

namespace lyraRuntime::http {

struct Endpoint;
struct Connection;
struct Reactor;
struct Request;
struct Field { const void* name; const void* value; };
struct Segment { const void* text; bool parameter; };
struct RoutePattern { Segment* segments; std::size_t count; };
struct ResponseFields { Field* fields; std::size_t count; std::size_t wireSize; };

enum class Delivery { Pending, Offered, Claimed, Closing, Closed };

struct Session {
    Endpoint* endpoint{};
    Reactor* reactor{};
    Connection* connection{};
    Request* request{};
    net::Socket socket{net::invalidSocket};
    bool overload{};
    int ingressError{};
    int64_t acceptedAt{};
};

struct Request {
    Session* session{};
    Connection* connection{};
    std::atomic<Delivery> delivery{Delivery::Pending};
    std::atomic<bool> peerLost{};
    int64_t offerExpires{};
    bool keepAlive{};
    bool http10{};
    bool streamingBody{};
    int status{};
    int bodyError{};
    const void* method{};
    const void* target{};
    const void* body{};
    RoutePattern path{};
    Field* query{};
    std::size_t queryCount{};
    std::atomic<const RoutePattern*> route{};
    Field* headers{};
    std::size_t headerCount{};
    int32_t streamId{};
};

inline constexpr std::size_t maxHeaderFields = 100;
inline constexpr int unsupportedExpectation = 1000004;

struct RequestParser {
    struct HeaderSpan { std::size_t nameSize{}, valueSize{}; };
    llhttp_t parser{};
    Request& request;
    std::string target, body, headerBytes;
    std::array<HeaderSpan, maxHeaderFields> headers{};
    std::size_t total{}, headerCount{}, consumed{}, maxBodyBytes, maxWireBytes, bodyBytes{};
    int error{};
    bool headersComplete{}, headersPaused{}, complete{};

    explicit RequestParser(Request&, std::size_t maxBodyBytes, std::size_t maxWireBytes);
    int feed(std::string_view bytes);
    void resumeBody();
    RequestParser(const RequestParser&) = delete;
};

std::string_view text(const void* value);
int populateRequest(Request&, std::string_view method, std::string_view target, std::span<const Field> fields);
int formatResponse(std::string& wire, int64_t status, const void* type, const void* body, const ResponseFields& headers,
    bool keepAlive, bool chunked, bool streaming = false);

}
