#include "managed.h"
#include "strings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

namespace {

enum class Step : int64_t {
    Input, Part, Bytes, End, InvalidOptions, ContentType, Malformed, Truncated,
    TotalLimit, PartLimit, HeaderLimit, FieldLimit, FileLimit, PartPending, Closed,
};

enum class Phase { Preamble, Headers, Body, Epilogue, End };

constexpr std::size_t maxBoundary = 70, maxPadding = 1024, maxHeaderFields = 64;

bool equalAscii(std::string_view first, std::string_view second) {
    auto lower = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
    return std::ranges::equal(first, second, [&](char left, char right) { return lower(left) == lower(right); });
}

bool alphaNumeric(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
}

bool tokenByte(unsigned char byte) {
    return alphaNumeric(byte) || std::string_view("!#$%&'*+-.^_`|~").find(byte) != std::string_view::npos;
}

bool textByte(unsigned char byte) { return (byte >= 0x20 && byte != 0x7f) || byte == '\t'; }

bool space(char byte) { return byte == ' ' || byte == '\t'; }

void trimLeft(std::string_view& input) {
    while (!input.empty() && space(input.front())) input.remove_prefix(1);
}

std::string_view trim(std::string_view input) {
    trimLeft(input);
    while (!input.empty() && space(input.back())) input.remove_suffix(1);
    return input;
}

std::string_view takeToken(std::string_view& input) {
    std::size_t length = 0;
    while (length < input.size() && tokenByte(input[length])) ++length;
    auto token = input.substr(0, length);
    input.remove_prefix(length);
    return token;
}

bool take(std::string_view& input, char expected) {
    if (input.empty() || input.front() != expected) return false;
    input.remove_prefix(1);
    return true;
}

bool takeQuoted(std::string_view& input, std::string& decoded) {
    decoded.clear();
    for (std::size_t offset = 0; offset < input.size(); ++offset) {
        auto byte = input[offset];
        if (byte == '"') { input.remove_prefix(offset + 1); return true; }
        if (byte == '\\') {
            if (++offset == input.size()) return false;
            byte = input[offset];
        }
        if (!textByte(byte)) return false;
        decoded.push_back(byte);
    }
    return false;
}

template<class Consume>
bool parameters(std::string_view input, Consume consume) {
    std::string quoted;
    for (;;) {
        trimLeft(input);
        if (input.empty()) return true;
        if (!take(input, ';')) return false;
        trimLeft(input);
        auto key = takeToken(input);
        trimLeft(input);
        if (key.empty() || !take(input, '=')) return false;
        trimLeft(input);
        std::string_view value;
        if (take(input, '"')) {
            if (!takeQuoted(input, quoted)) return false;
            value = quoted;
        } else {
            value = takeToken(input);
            if (value.empty()) return false;
        }
        if (!consume(key, value)) return false;
    }
}

bool validBoundary(std::string_view boundary) {
    return !boundary.empty() && boundary.size() <= maxBoundary && boundary.back() != ' ' &&
        std::ranges::all_of(boundary, [](unsigned char byte) {
            return alphaNumeric(byte) || std::string_view("'()+_,-./:=? ").find(byte) != std::string_view::npos;
        });
}

std::string boundaryOf(std::string_view media) {
    trimLeft(media);
    if (!equalAscii(takeToken(media), "multipart") || !take(media, '/') || !equalAscii(takeToken(media), "form-data")) return {};
    std::string boundary;
    if (!parameters(media, [&](std::string_view key, std::string_view value) {
        if (!equalAscii(key, "boundary")) return true;
        if (!boundary.empty() || !validBoundary(value)) return false;
        boundary = value;
        return true;
    })) return {};
    return boundary;
}

struct Header { const void* name; const void* value; };

struct Part {
    int64_t number{};
    const void* name{};
    const void* filename{};
    const void* contentType{};
    Header* headers{};
    std::size_t count{};
};

std::string_view text(const void* handle) { return {lyraStringBytes(handle), lyraStringLength(handle)}; }

const void* copy(std::string_view input) { return lyraStringFromBytes(input.data(), input.size()); }

bool disposition(Part& part, std::string_view value) {
    if (!equalAscii(takeToken(value), "form-data")) return false;
    return parameters(value, [&](std::string_view key, std::string_view content) {
        if (equalAscii(key, "filename*")) return false;
        const void** field = nullptr;
        if (equalAscii(key, "name")) field = &part.name;
        else if (equalAscii(key, "filename")) field = &part.filename;
        if (!field) return true;
        if (*field || !lyraUtf8Valid(content.data(), content.size())) return false;
        *field = copy(content);
        return true;
    }) && part.name;
}

bool mediaType(std::string_view input) {
    return !takeToken(input).empty() && take(input, '/') && !takeToken(input).empty() &&
        parameters(input, [](std::string_view, std::string_view) { return true; });
}

struct Buffer {
    char* data{};
    std::size_t begin{}, end{}, capacity{};

    std::string_view view() const { return begin == end ? std::string_view{} : std::string_view{data + begin, end - begin}; }
    void consume(std::size_t count) {
        begin += count;
        if (begin == end) begin = end = 0;
    }

    void append(std::string_view bytes) {
        if (bytes.empty()) return;
        if (bytes.size() > capacity - end) {
            auto remaining = end - begin;
            auto required = remaining + bytes.size();
            if (required > capacity) {
                capacity = std::max(required, capacity * 2);
                auto* expanded = static_cast<char*>(lyraGcAllocAtomic(capacity));
                if (remaining) std::memcpy(expanded, data + begin, remaining);
                data = expanded;
            } else if (remaining) std::memmove(data, data + begin, remaining);
            begin = 0;
            end = remaining;
        }
        std::memcpy(data + end, bytes.data(), bytes.size());
        end += bytes.size();
    }
};

enum class Delimiter { Input, Payload, Next, Final, Missing };
struct Scan { Delimiter kind; std::size_t count{}; };

enum class SuffixState { Start, SecondDash, Padding, LineFeed };

struct Suffix {
    std::size_t offset{}, padding{};
    SuffixState state{SuffixState::Start};
    bool closing{};

    Scan scan(std::string_view input, std::size_t markerSize, bool eof) {
        if (offset == 0) offset = markerSize;
        while (offset < input.size()) {
            auto byte = input[offset++];
            if (state == SuffixState::Start && byte == '-') {
                state = SuffixState::SecondDash;
            } else if (state == SuffixState::SecondDash) {
                if (byte != '-') return {Delimiter::Payload, markerSize};
                closing = true;
                state = SuffixState::Padding;
            } else if (state == SuffixState::LineFeed) {
                if (byte != '\n') return {Delimiter::Payload, markerSize};
                return {closing ? Delimiter::Final : Delimiter::Next, offset};
            } else if (space(byte)) {
                if (++padding > maxPadding) return {Delimiter::Payload, markerSize};
                state = SuffixState::Padding;
            } else if (byte == '\r') {
                state = SuffixState::LineFeed;
            } else return {Delimiter::Payload, markerSize};
        }
        if (!eof) return {Delimiter::Input};
        if (closing && state == SuffixState::Padding) return {Delimiter::Final, offset};
        return {Delimiter::Missing};
    }
};

struct Limits { std::size_t parts, headers, field, file, total; };

struct Multipart {
    std::atomic_flag busy{};
    Limits limits{};
    Buffer buffer{};
    Phase phase{Phase::Preamble};
    Step failure{Step::Input};
    std::array<char, maxBoundary + 4> marker{};
    std::size_t markerSize{}, total{}, headerChecked{}, lineStart{}, partBytes{};
    bool initial{true}, eof{}, sawCarriageReturn{};
    int64_t partCount{};
    Suffix suffix{};
    Part* part{};
    const void* chunk{};

    Step fail(Step status) {
        buffer = {};
        chunk = nullptr;
        return failure = status;
    }

    void feed(std::string_view bytes) {
        if (bytes.size() > limits.total - total) { fail(Step::TotalLimit); return; }
        total += bytes.size();
        if (phase != Phase::Epilogue) buffer.append(bytes);
    }

    Scan scan() {
        auto input = buffer.view();
        auto delimiter = std::string_view{marker.data(), markerSize};
        if (initial) {
            auto opening = delimiter.substr(2);
            if (input.starts_with(opening)) {
                auto result = suffix.scan(input, opening.size(), eof);
                if (result.kind != Delimiter::Input) { initial = false; suffix = {}; }
                return result;
            }
            if (!eof && opening.starts_with(input)) return {Delimiter::Input};
            initial = false;
        }
        auto offset = input.find(delimiter);
        if (offset != input.npos) {
            if (offset) return {Delimiter::Payload, offset};
            auto result = suffix.scan(input, markerSize, eof);
            if (result.kind != Delimiter::Input) suffix = {};
            return result;
        }
        if (eof) return {Delimiter::Missing};
        auto tail = input.rfind('\r');
        auto usable = tail != input.npos && delimiter.starts_with(input.substr(tail)) ? tail : input.size();
        return usable ? Scan{Delimiter::Payload, usable} : Scan{Delimiter::Input};
    }

    void advance(Scan result) {
        buffer.consume(result.count);
        if (result.kind == Delimiter::Next) {
            phase = Phase::Headers;
            headerChecked = lineStart = 0;
            sawCarriageReturn = false;
        } else {
            phase = Phase::Epilogue;
            buffer = {};
        }
    }

    Step parseHeaders(std::string_view block) {
        std::array<Header, maxHeaderFields> headers;
        auto* parsed = ::new (lyraGcAlloc(sizeof(Part))) Part{};
        while (!block.empty()) {
            if (parsed->count == headers.size()) return fail(Step::HeaderLimit);
            auto lineEnd = block.find("\r\n");
            auto line = block.substr(0, lineEnd);
            block.remove_prefix(lineEnd + 2);
            auto key = takeToken(line);
            if (key.empty() || !take(line, ':')) return fail(Step::Malformed);
            auto value = trim(line);
            if (!std::ranges::all_of(value, textByte) || !lyraUtf8Valid(value.data(), value.size())) return fail(Step::Malformed);
            if (equalAscii(key, "Content-Disposition")) {
                if (parsed->name || !disposition(*parsed, value)) return fail(Step::Malformed);
            } else if (equalAscii(key, "Content-Type")) {
                if (parsed->contentType || !mediaType(value)) return fail(Step::Malformed);
                parsed->contentType = copy(value);
            } else if (equalAscii(key, "Content-Transfer-Encoding")) return fail(Step::Malformed);
            headers[parsed->count++] = {copy(key), copy(value)};
        }
        if (!parsed->name) return fail(Step::Malformed);
        parsed->headers = static_cast<Header*>(lyraGcAlloc(parsed->count * sizeof(Header)));
        std::copy_n(headers.begin(), parsed->count, parsed->headers);
        parsed->number = ++partCount;
        part = parsed;
        partBytes = 0;
        phase = Phase::Body;
        return Step::Part;
    }

    Step readHeaders() {
        if (static_cast<std::size_t>(partCount) == limits.parts) return fail(Step::PartLimit);
        auto input = buffer.view();
        while (headerChecked < input.size()) {
            if (headerChecked == limits.headers) return fail(Step::HeaderLimit);
            auto byte = input[headerChecked++];
            if (sawCarriageReturn) {
                if (byte != '\n') return fail(Step::Malformed);
                if (headerChecked == lineStart + 2) {
                    auto status = parseHeaders(input.substr(0, lineStart));
                    if (status == Step::Part) buffer.consume(headerChecked);
                    return status;
                }
                lineStart = headerChecked;
                sawCarriageReturn = false;
            } else if (byte == '\r') sawCarriageReturn = true;
            else if (byte == '\n') return fail(Step::Malformed);
        }
        if (headerChecked == limits.headers) return fail(Step::HeaderLimit);
        return eof ? fail(Step::Truncated) : Step::Input;
    }

    Step next() {
        if (failure != Step::Input) return failure;
        if (phase == Phase::Body) return Step::PartPending;
        part = nullptr;
        chunk = nullptr;
        while (phase == Phase::Preamble) {
            auto result = scan();
            if (result.kind == Delimiter::Input) return Step::Input;
            if (result.kind == Delimiter::Missing) return fail(Step::Truncated);
            if (result.kind == Delimiter::Payload) buffer.consume(result.count);
            else advance(result);
        }
        if (phase == Phase::Headers) return readHeaders();
        if (!eof) return Step::Input;
        phase = Phase::End;
        return Step::End;
    }

    Step read(int64_t number, bool discard) {
        if (!part || part->number != number) return Step::Closed;
        if (failure != Step::Input) return failure;
        chunk = nullptr;
        if (phase != Phase::Body) return Step::End;
        auto limit = part->filename ? limits.file : limits.field;
        for (;;) {
            auto result = scan();
            switch (result.kind) {
                case Delimiter::Input: return Step::Input;
                case Delimiter::Missing: return fail(Step::Truncated);
                case Delimiter::Next: case Delimiter::Final: advance(result); return Step::End;
                case Delimiter::Payload:
                    if (result.count > limit - partBytes) return fail(part->filename ? Step::FileLimit : Step::FieldLimit);
                    if (!discard) chunk = copy(buffer.view().substr(0, result.count));
                    partBytes += result.count;
                    buffer.consume(result.count);
                    if (!discard) return Step::Bytes;
                    break;
            }
        }
    }
};

}

extern "C" Multipart* lyraMultipartCreate(const void* media, int64_t parts, int64_t headers, int64_t field, int64_t file, int64_t total) {
    auto* reader = ::new (lyraGcAlloc(sizeof(Multipart))) Multipart{};
    auto bytes = [](int64_t value) { return value >= 0 && value <= INT_MAX; };
    if (!bytes(parts) || headers < 1 || headers > 65536 || !bytes(field) || !bytes(file) || !bytes(total)) {
        reader->fail(Step::InvalidOptions);
        return reader;
    }
    auto boundary = boundaryOf(text(media));
    if (boundary.empty()) { reader->fail(Step::ContentType); return reader; }
    reader->limits = {static_cast<std::size_t>(parts), static_cast<std::size_t>(headers), static_cast<std::size_t>(field),
        static_cast<std::size_t>(file), static_cast<std::size_t>(total)};
    std::memcpy(reader->marker.data(), "\r\n--", 4);
    std::memcpy(reader->marker.data() + 4, boundary.data(), boundary.size());
    reader->markerSize = boundary.size() + 4;
    return reader;
}

extern "C" int64_t lyraMultipartStatus(const Multipart* reader) { return static_cast<int64_t>(reader->failure); }
extern "C" uint8_t lyraMultipartEnter(Multipart* reader) { return !reader->busy.test_and_set(std::memory_order_acquire); }
extern "C" void lyraMultipartLeave(Multipart* reader) { reader->busy.clear(std::memory_order_release); }
extern "C" void lyraMultipartFeed(Multipart* reader, const void* bytes) { reader->feed(text(bytes)); }
extern "C" void lyraMultipartFinish(Multipart* reader) { reader->eof = true; }
extern "C" int64_t lyraMultipartNext(Multipart* reader) { return static_cast<int64_t>(reader->next()); }
extern "C" int64_t lyraMultipartRead(Multipart* reader, int64_t number, uint8_t discard) { return static_cast<int64_t>(reader->read(number, discard)); }
extern "C" const Part* lyraMultipartPart(const Multipart* reader) { return reader->part; }
extern "C" const void* lyraMultipartChunk(const Multipart* reader) { return reader->chunk; }
extern "C" int64_t lyraMultipartPartNumber(const Part* part) { return part->number; }
extern "C" const void* lyraMultipartPartName(const Part* part) { return part->name; }
extern "C" uint8_t lyraMultipartHasFilename(const Part* part) { return part->filename != nullptr; }
extern "C" const void* lyraMultipartFilename(const Part* part) { return part->filename; }
extern "C" uint8_t lyraMultipartHasContentType(const Part* part) { return part->contentType != nullptr; }
extern "C" const void* lyraMultipartContentType(const Part* part) { return part->contentType; }
extern "C" int64_t lyraMultipartHeaderCount(const Part* part) { return static_cast<int64_t>(part->count); }
extern "C" const void* lyraMultipartHeaderName(const Part* part, int64_t index) { return part->headers[index].name; }
extern "C" const void* lyraMultipartHeaderValue(const Part* part, int64_t index) { return part->headers[index].value; }
