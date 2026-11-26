#include "managed.h"
#include "strings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <gc/gc.h>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <zlib.h>

namespace {

constexpr int64_t maxChunkBytes = 16 * 1024 * 1024;
constexpr int gzipWindowBits = 15 + 16;
constexpr uInt flushHeadroom = 8;

enum class Status : int64_t { Ok, Invalid, Closed, Busy, InputTooLarge, OutputTooLarge, InvalidData, Truncated, Native };

struct GzipResult {
    Status status;
    int code;
    const void* value;
};

const GzipResult* result(Status status = Status::Ok, const void* value = nullptr, int code = 0) {
    return ::new (lyraGcAlloc(sizeof(GzipResult))) GzipResult{status, code, value};
}

struct Stream {
    Stream(uInt inputLimit, uInt outputChunk) : inputLimit(inputLimit), outputChunk(outputChunk) {}

    z_stream native{};
    std::atomic_flag busy{};
    const uInt inputLimit, outputChunk;
    bool active{};

    void clearBuffers() {
        native.next_in = native.next_out = nullptr;
        native.avail_in = native.avail_out = 0;
    }
};

struct Encoder : Stream {
    using Stream::Stream;

    void close() {
        if (std::exchange(active, false)) deflateEnd(&native);
    }
};

struct Decoder : Stream {
    Decoder(uInt inputLimit, uInt outputChunk, int64_t remaining)
        : Stream(inputLimit, outputChunk), remaining(remaining) {}

    int64_t remaining;
    const void* input{};
    std::size_t offset{};
    bool memberComplete{};

    void releaseInput() {
        input = nullptr;
        offset = 0;
    }

    void close() {
        releaseInput();
        if (std::exchange(active, false)) inflateEnd(&native);
    }
};

struct Lease {
    explicit Lease(Stream& stream) : stream(stream), held(!stream.busy.test_and_set(std::memory_order_acquire)) {}
    ~Lease() {
        if (held) stream.busy.clear(std::memory_order_release);
        GC_reachable_here(&stream);
    }

    Status status() const {
        if (!held) return Status::Busy;
        return stream.active ? Status::Ok : Status::Closed;
    }

    Stream& stream;
    const bool held;
};

bool validChunk(int64_t bytes) { return bytes > 0 && bytes <= maxChunkBytes; }

template <class Codec>
void enable(Codec& codec) {
    codec.active = true;
    GC_REGISTER_FINALIZER_NO_ORDER(&codec, [](void* object, void*) {
        static_cast<Codec*>(object)->close();
    }, nullptr, nullptr, nullptr);
}

template <class Codec>
int64_t close(Codec& codec) {
    const Lease lease(codec);
    if (!lease.held) return static_cast<int64_t>(Status::Busy);
    codec.close();
    return static_cast<int64_t>(Status::Ok);
}

auto buffer(uInt size) {
    return std::unique_ptr<Bytef, decltype(&std::free)>{static_cast<Bytef*>(std::malloc(size)), std::free};
}

const Bytef* bytes(const void* value) { return reinterpret_cast<const Bytef*>(lyraStringBytes(value)); }

std::string_view text(const void* value) { return {lyraStringBytes(value), lyraStringLength(value)}; }

std::string_view trimOws(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t");
    return begin == value.npos ? std::string_view{} : value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}

bool equalAscii(std::string_view first, std::string_view second) {
    const auto lower = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
    return std::ranges::equal(first, second, [&](char left, char right) { return lower(left) == lower(right); });
}

bool directive(std::string_view field, std::string_view name) {
    std::size_t start = 0;
    bool quoted = false, argument = false;
    for (std::size_t index = 0; index < field.size(); ++index) {
        const auto byte = field[index];
        if (quoted) {
            if (byte == '"') quoted = false;
            else if (byte == '\\' && index + 1 < field.size()) ++index;
        } else if (byte == ',') {
            if (!argument && equalAscii(trimOws(field.substr(start, index - start)), name)) return true;
            start = index + 1;
            argument = false;
        } else if (byte == '=' && !argument) {
            if (equalAscii(trimOws(field.substr(start, index - start)), name)) return true;
            argument = true;
        } else if (byte == '"') quoted = true;
    }
    return !argument && equalAscii(trimOws(field.substr(start)), name);
}

const GzipResult* output(const Bytef* data, uInt size) {
    return result(Status::Ok, lyraStringFromBytes(reinterpret_cast<const char*>(data), size));
}

const GzipResult* encode(Encoder& encoder, const void* input, std::size_t offset, std::size_t size, int flush) {
    const Lease lease(encoder);
    if (auto status = lease.status(); status != Status::Ok) return result(status);
    if (input) {
        const auto available = lyraStringLength(input);
        if (offset > available || size > available - offset) return result(Status::Invalid);
    }
    if (size > encoder.inputLimit) return result(Status::InputTooLarge);
    if (!size && flush == Z_NO_FLUSH) return output(nullptr, 0);

    const auto capacity = encoder.outputChunk + flushHeadroom;
    auto destination = buffer(capacity);
    if (!destination) return result(Status::Native, nullptr, Z_MEM_ERROR);
    auto& stream = encoder.native;
    stream.next_in = input ? const_cast<Bytef*>(bytes(input) + offset) : nullptr;
    stream.avail_in = static_cast<uInt>(size);
    stream.next_out = destination.get();
    stream.avail_out = capacity;
    const auto code = deflate(&stream, flush);
    const auto pending = stream.avail_in;
    const auto produced = capacity - stream.avail_out;
    encoder.clearBuffers();
    GC_reachable_here(input);
    if (produced > encoder.outputChunk) {
        encoder.close();
        return result(Status::OutputTooLarge);
    }
    const bool finishing = flush == Z_FINISH;
    const bool complete = finishing ? code == Z_STREAM_END : code == Z_OK || code == Z_BUF_ERROR;
    if (!complete || pending) {
        encoder.close();
        return result(Status::Native, nullptr, code);
    }
    if (finishing) encoder.close();
    return output(destination.get(), produced);
}

const GzipResult* decode(Decoder& decoder) {
    const Lease lease(decoder);
    if (auto status = lease.status(); status != Status::Ok) return result(status);
    if (!decoder.input) return result();

    const auto capacity = decoder.remaining < decoder.outputChunk ? static_cast<uInt>(decoder.remaining) + 1 : decoder.outputChunk;
    auto destination = buffer(capacity);
    if (!destination) return result(Status::Native, nullptr, Z_MEM_ERROR);
    auto& stream = decoder.native;
    const auto inputSize = lyraStringLength(decoder.input);

    while (true) {
        const auto available = static_cast<uInt>(inputSize - decoder.offset);
        if (decoder.memberComplete) {
            if (!available) break;
            if (const auto code = inflateReset(&stream); code != Z_OK) {
                decoder.close();
                return result(Status::Native, nullptr, code);
            }
            decoder.memberComplete = false;
        }
        stream.next_in = const_cast<Bytef*>(bytes(decoder.input) + decoder.offset);
        stream.avail_in = available;
        stream.next_out = destination.get();
        stream.avail_out = capacity;
        const auto code = inflate(&stream, Z_NO_FLUSH);
        decoder.offset += available - stream.avail_in;
        const auto produced = capacity - stream.avail_out;
        decoder.clearBuffers();

        if (code == Z_DATA_ERROR || code == Z_NEED_DICT) {
            decoder.close();
            return result(Status::InvalidData);
        }
        if (code != Z_OK && code != Z_BUF_ERROR && code != Z_STREAM_END) {
            decoder.close();
            return result(Status::Native, nullptr, code);
        }
        if (produced > decoder.remaining) {
            decoder.close();
            return result(Status::OutputTooLarge);
        }
        decoder.memberComplete = code == Z_STREAM_END;
        if (produced) {
            decoder.remaining -= produced;
            return output(destination.get(), produced);
        }
        if (decoder.memberComplete) continue;
        if (decoder.offset == inputSize) break;

        decoder.close();
        return result(Status::Native, nullptr, Z_BUF_ERROR);
    }

    decoder.releaseInput();
    return result();
}

}

extern "C" const GzipResult* lyraGzipEncoderOpen(int64_t level, int64_t maxInputChunkBytes, int64_t maxOutputChunkBytes) {
    if (level < Z_DEFAULT_COMPRESSION || level > Z_BEST_COMPRESSION || !validChunk(maxInputChunkBytes) || !validChunk(maxOutputChunkBytes))
        return result(Status::Invalid);
    auto* encoder = ::new (lyraGcAllocAtomic(sizeof(Encoder))) Encoder(static_cast<uInt>(maxInputChunkBytes), static_cast<uInt>(maxOutputChunkBytes));
    const auto code = deflateInit2(&encoder->native, static_cast<int>(level), Z_DEFLATED, gzipWindowBits, 8, Z_DEFAULT_STRATEGY);
    if (code != Z_OK) return result(Status::Native, nullptr, code);
    enable(*encoder);
    return result(Status::Ok, encoder);
}

extern "C" const GzipResult* lyraGzipEncode(Encoder* encoder, const void* bytes, uint8_t flush) {
    return encode(*encoder, bytes, 0, lyraStringLength(bytes), flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
}

extern "C" const GzipResult* lyraGzipEncodeRange(Encoder* encoder, const void* bytes, int64_t start, int64_t end, uint8_t flush) {
    if (start < 0 || end < start) return result(Status::Invalid);
    return encode(*encoder, bytes, static_cast<std::size_t>(start), static_cast<std::size_t>(end - start), flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
}

extern "C" const GzipResult* lyraGzipEncoderFinish(Encoder* encoder) { return encode(*encoder, nullptr, 0, 0, Z_FINISH); }
extern "C" int64_t lyraGzipEncoderClose(Encoder* encoder) { return close(*encoder); }

extern "C" const GzipResult* lyraGzipDecoderOpen(int64_t maxOutputBytes, int64_t maxInputChunkBytes, int64_t outputChunkBytes) {
    if (maxOutputBytes < 0 || !validChunk(maxInputChunkBytes) || !validChunk(outputChunkBytes)) return result(Status::Invalid);
    auto* decoder = ::new (lyraGcAlloc(sizeof(Decoder))) Decoder(static_cast<uInt>(maxInputChunkBytes), static_cast<uInt>(outputChunkBytes), maxOutputBytes);
    const auto code = inflateInit2(&decoder->native, gzipWindowBits);
    if (code != Z_OK) return result(Status::Native, nullptr, code);
    enable(*decoder);
    return result(Status::Ok, decoder);
}

extern "C" int64_t lyraGzipDecoderPush(Decoder* decoder, const void* bytes) {
    const Lease lease(*decoder);
    if (auto status = lease.status(); status != Status::Ok) return static_cast<int64_t>(status);
    if (decoder->input) return static_cast<int64_t>(Status::Busy);
    const auto size = lyraStringLength(bytes);
    if (size > decoder->inputLimit) return static_cast<int64_t>(Status::InputTooLarge);
    if (size) decoder->input = bytes;
    return static_cast<int64_t>(Status::Ok);
}

extern "C" const GzipResult* lyraGzipDecoderRead(Decoder* decoder) { return decode(*decoder); }

extern "C" int64_t lyraGzipDecoderFinish(Decoder* decoder) {
    const Lease lease(*decoder);
    if (auto status = lease.status(); status != Status::Ok) return static_cast<int64_t>(status);
    if (decoder->input) return static_cast<int64_t>(Status::Busy);
    const auto status = decoder->memberComplete ? Status::Ok : Status::Truncated;
    decoder->close();
    return static_cast<int64_t>(status);
}

extern "C" int64_t lyraGzipDecoderClose(Decoder* decoder) { return close(*decoder); }

extern "C" int64_t lyraGzipResultStatus(const GzipResult* result) { return static_cast<int64_t>(result->status); }
extern "C" int64_t lyraGzipResultCode(const GzipResult* result) { return result->code; }
extern "C" const void* lyraGzipResultHandle(const GzipResult* result) { return result->value; }
extern "C" const void* lyraGzipResultBytes(const GzipResult* result) { return result->value; }
extern "C" uint8_t lyraGzipResultHasBytes(const GzipResult* result) { return result->value != nullptr; }

extern "C" uint8_t lyraGzipNoTransform(const void* field) { return directive(text(field), "no-transform"); }
