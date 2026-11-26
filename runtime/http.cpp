#include "http_protocol.h"
#include "managed.h"
#include "strings.h"

#include "net.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

using namespace lyraRuntime::http;

namespace {

struct PathInteger { int64_t value; bool valid; };
constexpr std::size_t maxBody = 128 * 1024;
constexpr std::size_t maxTarget = 8192;
constexpr std::size_t maxHeaderBytes = 32 * 1024;

int append(RequestParser& parsed, std::string& storage, std::size_t limit, const char* data, std::size_t size) {
    if (size > limit - storage.size()) { parsed.error = EMSGSIZE; return -1; }
    storage.append(data, size);
    return 0;
}

int appendHeader(llhttp_t* parser, const char* data, std::size_t size, bool name) {
    auto& parsed = *static_cast<RequestParser*>(parser->data);
    if (parsed.headersComplete) return 0;
    if (parsed.headerCount == maxHeaderFields) { parsed.error = EMSGSIZE; return -1; }
    auto& header = parsed.headers[parsed.headerCount];
    (name ? header.nameSize : header.valueSize) += size;
    return append(parsed, parsed.headerBytes, maxHeaderBytes, data, size);
}

int parseHeaders(const llhttp_t& parser, const RequestParser& parsed);

constexpr auto parserSettings = [] {
    llhttp_settings_t config{};
    config.on_url = [](llhttp_t* parser, const char* bytes, std::size_t count) {
        auto& parsed = *static_cast<RequestParser*>(parser->data);
        return append(parsed, parsed.target, maxTarget, bytes, count);
    };
    config.on_body = [](llhttp_t* parser, const char* bytes, std::size_t count) {
        auto& parsed = *static_cast<RequestParser*>(parser->data);
        return append(parsed, parsed.body, maxBody, bytes, count);
    };
    config.on_header_field = [](llhttp_t* parser, const char* bytes, std::size_t count) { return appendHeader(parser, bytes, count, true); };
    config.on_header_value = [](llhttp_t* parser, const char* bytes, std::size_t count) { return appendHeader(parser, bytes, count, false); };
    config.on_header_value_complete = [](llhttp_t* parser) {
        auto& parsed = *static_cast<RequestParser*>(parser->data);
        if (!parsed.headersComplete) ++parsed.headerCount;
        return 0;
    };
    config.on_headers_complete = [](llhttp_t* parser) {
        auto& parsed = *static_cast<RequestParser*>(parser->data);
        parsed.headersComplete = true;
        parsed.error = parseHeaders(*parser, parsed);
        return parsed.error ? -1 : 0;
    };
    config.on_message_complete = [](llhttp_t* parser) { static_cast<RequestParser*>(parser->data)->complete = true; return static_cast<int>(HPE_PAUSED); };
    return config;
}();

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

int parseHeaders(const llhttp_t& parser, const RequestParser& parsed) {
    if (parser.http_major != 1 || parser.http_minor > 1) return EPROTO;
    auto& request = parsed.request;
    if (parsed.headerCount) request.headers = static_cast<Field*>(lyraGcAlloc(parsed.headerCount * sizeof(Field)));
    bool host = false, expect = false;
    std::size_t start = 0;
    for (std::size_t index = 0; index < parsed.headerCount; ++index) {
        auto [nameSize, valueSize] = parsed.headers[index];
        std::string_view name{parsed.headerBytes.data() + start, nameSize};
        auto value = trimField({name.data() + nameSize, valueSize});
        if (!validFieldName(name) || !validFieldValue(value)) return EPROTO;
        if (!lyraUtf8Valid(value.data(), value.size())) return EILSEQ;
        if (fieldEqual(name, "Host")) {
            if (host || !validHost(value)) return EPROTO;
            host = true;
        }
        if (fieldEqual(name, "Expect")) expect = true;
        request.headers[request.headerCount++] = {
            lyraStringFromBytes(name.data(), nameSize), lyraStringFromBytes(value.data(), value.size()),
        };
        start += nameSize + valueSize;
    }
    if (parser.http_minor == 1 && !host) return EPROTO;
    return expect ? unsupportedExpectation : 0;
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

namespace lyraRuntime::http {

std::string_view text(const void* value) { return {lyraStringBytes(value), lyraStringLength(value)}; }

RequestParser::RequestParser(Request& message) : request(message) {
    llhttp_init(&parser, HTTP_REQUEST, &parserSettings);
    parser.data = this;
}

int RequestParser::feed(std::string_view bytes) {
    total += bytes.size();
    if (total > maxWire) return EMSGSIZE;
    auto code = llhttp_execute(&parser, bytes.data(), bytes.size());
    if (code != HPE_OK && !(code == HPE_PAUSED && complete)) return error ? error : EPROTO;
    if (!complete) return 0;
    if (!target.starts_with('/') || !lyraUtf8Valid(target.data(), target.size()) || !parseTarget(&request, target)) return EPROTO;
    auto method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(&parser)));
    request.method = lyraStringFromBytes(method, std::strlen(method));
    request.target = lyraStringFromBytes(target.data(), target.size());
    request.body = lyraStringFromBytes(body.data(), body.size());
    return 0;
}

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

extern "C" void lyraHttpRouteBind(Request* req, const RoutePattern* route) { req->route.store(route); }

extern "C" int64_t lyraHttpPathIndex(const Request* req, const void* name) {
    if (auto* route = req->route.load()) for (std::size_t index = 0; index < route->count; ++index) {
        auto part = route->segments[index];
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

int lyraRuntime::http::formatResponse(std::string& wire, int64_t status, const void* mediaType, const void* body, const ResponseFields& headers) {
    auto type = text(mediaType);
    auto length = lyraStringLength(body);
    auto noBody = status == 204 || status == 304;
    if (status < 200 || status > 599 || type.empty() || type.size() > 200 || ((noBody || status == 205) && length) ||
        !std::ranges::all_of(type, [](unsigned char byte) { return byte >= 0x20 && byte <= 0x7e; })) return EINVAL;
    char basic[512];
    auto count = std::snprintf(basic, sizeof(basic), "HTTP/1.1 %lld %s\r\nContent-Type: %.*s\r\nConnection: close\r\n",
        static_cast<long long>(status), reason(status), static_cast<int>(type.size()), type.data());
    wire.assign(basic, count);
    wire.reserve(wire.size() + 40 + headers.wireSize);
    if (!noBody) {
        auto fieldSize = std::snprintf(basic, sizeof(basic), "Content-Length: %zu\r\n", length);
        wire.append(basic, fieldSize);
    }
    for (std::size_t index = 0; index < headers.count; ++index) {
        const auto& [name, value] = headers.fields[index];
        wire.append(text(name)).append(": ").append(text(value)).append("\r\n");
    }
    wire.append("\r\n");
    return 0;
}
