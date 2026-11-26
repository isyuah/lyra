#include "http2.h"
#include "http_protocol.h"
#include "http_transport.h"
#include "managed.h"
#include "strings.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lyraRuntime::http::h2 {

namespace {

constexpr std::size_t receiveWindow = 65535, readChunk = 8192, writeChunk = 16384;
constexpr std::size_t maxHeaderBytes = 32768, maxOutbound = 128;

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "Lyra HTTP/2: %s\n", message);
    std::abort();
}

int nativeStatus(int result) {
    switch (result) {
        case 0: return 0;
        case NGHTTP2_ERR_NOMEM: return ENOMEM;
        case NGHTTP2_ERR_DATA_EXIST: return EBUSY;
        case NGHTTP2_ERR_STREAM_CLOSED: case NGHTTP2_ERR_STREAM_CLOSING: return EBADF;
        default: return EPROTO;
    }
}

bool equalAscii(std::string_view first, std::string_view second) {
    auto lower = [](unsigned char byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; };
    return std::ranges::equal(first, second, [&](char left, char right) { return lower(left) == lower(right); });
}

bool connectionField(std::string_view name) {
    return name == "connection" || name == "proxy-connection" || name == "keep-alive" || name == "transfer-encoding" || name == "upgrade";
}

nghttp2_nv field(std::string_view name, std::string_view value) {
    return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
        name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
}

struct Stream {
    Request* request;
    LyraGcRoot* root;
    std::string method, path, authority, scheme, body;
    std::size_t bodyOffset{}, bodyBytes{}, headerBytes{}, wireBytes{}, headerCount{};
    std::optional<uint64_t> contentLength;
    bool ordinaryHeader{}, initialHeaders{}, trailers{}, remoteEnd{}, ready{}, announce{}, dirty{};
    bool responding{}, streaming{}, responseEnd{}, rejecting{}, resetSubmitted{}, wireClosed{}, peer{};
    int error{};
    const void* outgoing{};
    LyraGcRoot* outgoingRoot{};
    std::size_t outgoingOffset{};
    bool writePending{}, startOnly{}, finalWrite{}, finishWhenHeaders{};
    uint64_t writeAfter{}, closeAfter{};
    std::optional<int> completion;

    explicit Stream(Request& request) : request(&request), root(lyraGcRootCreate(&request)) {
        request.headers = static_cast<Field*>(lyraGcAlloc(sizeof(Field) * maxHeaderFields));
        request.body = lyraStringFromBytes(nullptr, 0);
    }

    ~Stream() {
        if (outgoingRoot) lyraGcRootDrop(outgoingRoot);
        lyraGcRootDrop(root);
    }

    void pin(const void* bytes) {
        require(!outgoingRoot, "stream retained more than one outgoing chunk");
        outgoing = bytes;
        outgoingOffset = 0;
        outgoingRoot = lyraGcRootCreate(const_cast<void*>(bytes));
    }

    void unpin() {
        if (outgoingRoot) lyraGcRootDrop(outgoingRoot);
        outgoingRoot = nullptr;
        outgoing = nullptr;
        outgoingOffset = 0;
    }
};

}

struct Session::Impl {
    ServerOptions options;
    Hooks hooks;
    void* user;
    std::size_t requestLimit;
    nghttp2_session* session{};
    std::map<int32_t, std::unique_ptr<Stream>> streams;
    std::string_view pendingOutput;
    uint64_t serial{}, acknowledged{};
    int failure{};
    std::size_t accepted{};
    int32_t lastAccepted{};
    bool draining{}, aborted{}, inSend{};

    Impl(const ServerOptions& options, Hooks hooks, void* user) : options(options), hooks(hooks), user(user),
        requestLimit(options.keepAliveTimeoutMs ? static_cast<std::size_t>(options.maxRequestsPerConnection) : 1) {
        nghttp2_session_callbacks* callbacks = nullptr;
        nghttp2_option* settings = nullptr;
        auto result = nghttp2_session_callbacks_new(&callbacks);
        if (!result) result = nghttp2_option_new(&settings);
        if (!result) {
            nghttp2_option_set_no_auto_window_update(settings, 1);
            nghttp2_option_set_max_outbound_ack(settings, 64);
            nghttp2_option_set_max_settings(settings, 16);
            nghttp2_option_set_max_continuations(settings, 4);
            nghttp2_option_set_stream_reset_rate_limit(settings, 128, 16);
            nghttp2_option_set_glitch_rate_limit(settings, 256, 32);
            nghttp2_option_set_max_send_header_block_length(settings, maxHeaderBytes * 2);
            nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, beginHeaders);
            nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
            nghttp2_session_callbacks_set_on_invalid_header_callback(callbacks, invalidHeader);
            nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks, beginFrame);
            nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, data);
            nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, received);
            nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, sent);
            nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, notSent);
            nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closed);
            result = nghttp2_session_server_new2(&session, callbacks, this, settings);
        }
        nghttp2_session_callbacks_del(callbacks);
        nghttp2_option_del(settings);
        if (result) { failure = nativeStatus(result); return; }
        const std::array<nghttp2_settings_entry, 5> initial{{
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, static_cast<uint32_t>(options.capacity)},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, receiveWindow},
            {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, maxHeaderBytes},
            {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, 4096},
            {NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES, 1},
        }};
        failure = nativeStatus(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, initial.data(), initial.size()));
    }

    ~Impl() { nghttp2_session_del(session); }

    Stream* find(int32_t id) {
        auto entry = streams.find(id);
        return entry == streams.end() ? nullptr : entry->second.get();
    }

    Stream* find(Request& request) {
        auto* stream = find(request.streamId);
        return stream && stream->request == &request ? stream : nullptr;
    }

    void submit(int result) { if (result && !failure) failure = nativeStatus(result); }

    void failStream(Stream& stream, int status, uint32_t reason = NGHTTP2_CANCEL, bool peer = false) {
        if (stream.wireClosed) return;
        if (!stream.error) stream.error = status;
        stream.request->bodyError = stream.error;
        stream.peer = stream.peer || peer;
        stream.dirty = true;
        if (!stream.rejecting && !stream.resetSubmitted) {
            stream.resetSubmitted = true;
            submit(nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream.request->streamId, reason));
        }
    }

    void rejected(Stream& stream, int status) {
        if (stream.error || stream.responding) { failStream(stream, status, NGHTTP2_PROTOCOL_ERROR); return; }
        stream.error = stream.request->bodyError = status;
        stream.rejecting = stream.responding = stream.dirty = true;
        stream.announce = false;
        auto code = status == EMSGSIZE ? "413" : status == unsupportedExpectation ? "417" : "400";
        const std::array<nghttp2_nv, 2> headers{{field(":status", code), field("content-length", "0")}};
        submit(nghttp2_submit_response2(session, stream.request->streamId, headers.data(), headers.size(), nullptr));
    }

    void announce(Stream& stream) {
        if (stream.ready || stream.error) return;
        stream.ready = stream.announce = true;
        if (!stream.request->streamingBody) stream.request->body = lyraStringFromBytes(stream.body.data(), stream.body.size());
    }

    void finishInput(Stream& stream) {
        if (stream.remoteEnd) { failStream(stream, EPROTO, NGHTTP2_PROTOCOL_ERROR, true); return; }
        stream.remoteEnd = stream.dirty = true;
        if (stream.contentLength && *stream.contentLength != stream.bodyBytes) {
            rejected(stream, EPROTO);
            return;
        }
        if (!stream.request->streamingBody) announce(stream);
    }

    void completion(Stream& stream, int result) {
        if (!stream.writePending) return;
        stream.writePending = false;
        stream.writeAfter = 0;
        stream.startOnly = false;
        stream.completion = result;
        stream.unpin();
        stream.dirty = true;
    }

    void flush() {
        for (auto iterator = streams.begin(); iterator != streams.end();) {
            auto& stream = *iterator->second;
            if (stream.writePending && stream.writeAfter && stream.writeAfter <= acknowledged) completion(stream, stream.error);
            if (stream.announce) {
                stream.announce = false;
                hooks.ready(user, *stream.request);
            }
            const bool finished = stream.wireClosed && (!stream.closeAfter || stream.closeAfter <= acknowledged);
            if (finished) completion(stream, stream.error);
            if (stream.dirty) {
                stream.dirty = false;
                hooks.changed(user, *stream.request);
            }
            if (!finished) { ++iterator; continue; }
            hooks.closed(user, *stream.request, stream.error, stream.peer);
            iterator = streams.erase(iterator);
        }
    }

    void drain() {
        if (draining || failure) return;
        draining = true;
        submit(nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, lastAccepted, NGHTTP2_NO_ERROR, nullptr, 0));
    }

    static int beginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* data) {
        auto& self = *static_cast<Impl*>(data);
        const auto id = frame->hd.stream_id;
        if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            auto* stream = self.find(id);
            if (stream && !stream->trailers && !stream->remoteEnd && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                stream->trailers = true;
                return 0;
            }
            if (stream) self.failStream(*stream, EPROTO, NGHTTP2_PROTOCOL_ERROR, true);
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        if (self.draining || self.accepted >= self.requestLimit ||
            self.streams.size() >= static_cast<std::size_t>(self.options.capacity)) {
            self.submit(nghttp2_submit_rst_stream(self.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_REFUSED_STREAM));
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        auto* request = self.hooks.create(self.user, id);
        if (!request) {
            self.submit(nghttp2_submit_rst_stream(self.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_REFUSED_STREAM));
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        request->streamId = id;
        request->streamingBody = self.options.streamRequestBody;
        auto stream = std::make_unique<Stream>(*request);
        stream->wireBytes = frame->hd.length + 9;
        auto* input = stream.get();
        self.streams.emplace(id, std::move(stream));
        self.lastAccepted = id;
        if (input->wireBytes > static_cast<std::size_t>(self.options.maxRequestBytes)) self.rejected(*input, EMSGSIZE);
        ++self.accepted;
        return 0;
    }

    static int beginFrame(nghttp2_session*, const nghttp2_frame_hd* frame, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (auto* stream = self.find(frame->stream_id); stream && !stream->error &&
            (frame->type == NGHTTP2_HEADERS || frame->type == NGHTTP2_CONTINUATION || frame->type == NGHTTP2_DATA)) {
            const auto count = frame->length + 9;
            const auto available = static_cast<std::size_t>(self.options.maxRequestBytes) - stream->wireBytes;
            if (count > available) self.rejected(*stream, EMSGSIZE);
            else stream->wireBytes += count;
        }
        return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }

    static int header(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, std::size_t nameLength,
        const uint8_t* value, std::size_t valueLength, uint8_t, void* data) {
        auto& self = *static_cast<Impl*>(data);
        auto* stream = self.find(frame->hd.stream_id);
        if (!stream || stream->error) return 0;
        std::string_view key{reinterpret_cast<const char*>(name), nameLength}, content{reinterpret_cast<const char*>(value), valueLength};
        const auto budget = maxHeaderBytes - stream->headerBytes;
        if (++stream->headerCount > maxHeaderFields || budget < 32 || key.size() > budget - 32 || content.size() > budget - 32 - key.size()) {
            self.rejected(*stream, EMSGSIZE);
            return 0;
        }
        stream->headerBytes += key.size() + content.size() + 32;
        if (stream->headerBytes > static_cast<std::size_t>(self.options.maxRequestBytes) || !lyraUtf8Valid(content.data(), content.size())) {
            self.rejected(*stream, stream->headerBytes > static_cast<std::size_t>(self.options.maxRequestBytes) ? EMSGSIZE : EILSEQ);
            return 0;
        }
        if (!key.empty() && key.front() == ':') {
            std::string* target = nullptr;
            if (!stream->ordinaryHeader && !stream->trailers) {
                if (key == ":method") target = &stream->method;
                else if (key == ":path") target = &stream->path;
                else if (key == ":authority") target = &stream->authority;
                else if (key == ":scheme") target = &stream->scheme;
            }
            if (!target || !target->empty() || content.empty()) { self.rejected(*stream, EPROTO); return 0; }
            target->assign(content);
            return 0;
        }
        stream->ordinaryHeader = true;
        if (connectionField(key) || (key == "te" && content != "trailers")) { self.rejected(*stream, EPROTO); return 0; }
        if (key == "content-length") {
            uint64_t count;
            auto [end, error] = std::from_chars(content.data(), content.data() + content.size(), count);
            if (stream->trailers || stream->contentLength || error != std::errc{} || end != content.data() + content.size()) {
                self.rejected(*stream, EPROTO);
                return 0;
            }
            stream->contentLength = count;
            if (count > static_cast<std::size_t>(self.options.maxBodyBytes)) { self.rejected(*stream, EMSGSIZE); return 0; }
        }
        if (stream->trailers) return 0;
        auto& request = *stream->request;
        request.headers[request.headerCount++] = {
            lyraStringFromBytes(key.data(), key.size()), lyraStringFromBytes(content.data(), content.size()),
        };
        return 0;
    }

    static int invalidHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t*, std::size_t,
        const uint8_t*, std::size_t, uint8_t, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (auto* stream = self.find(frame->hd.stream_id)) self.failStream(*stream, EPROTO, NGHTTP2_PROTOCOL_ERROR, true);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    static int data(nghttp2_session*, uint8_t, int32_t id, const uint8_t* bytes, std::size_t size, void* data) {
        auto& self = *static_cast<Impl*>(data);
        self.submit(nghttp2_session_consume_connection(self.session, size));
        auto* stream = self.find(id);
        if (!stream || stream->error) return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
        if (size > static_cast<std::size_t>(self.options.maxBodyBytes) - stream->bodyBytes) {
            self.rejected(*stream, EMSGSIZE);
            return 0;
        }
        stream->bodyBytes += size;
        if (stream->request->streamingBody) {
            auto available = receiveWindow - (stream->body.size() - stream->bodyOffset);
            if (size > available) { self.failStream(*stream, EPROTO, NGHTTP2_FLOW_CONTROL_ERROR, true); return 0; }
            if (stream->bodyOffset) {
                stream->body.erase(0, stream->bodyOffset);
                stream->bodyOffset = 0;
            }
        } else self.submit(nghttp2_session_consume_stream(self.session, id, size));
        stream->body.append(reinterpret_cast<const char*>(bytes), size);
        stream->dirty = true;
        return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }

    static int received(nghttp2_session*, const nghttp2_frame* frame, void* data) {
        auto& self = *static_cast<Impl*>(data);
        auto* stream = self.find(frame->hd.stream_id);
        if (frame->hd.type == NGHTTP2_GOAWAY) { self.drain(); return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0; }
        if (!stream) return 0;
        if (frame->hd.type == NGHTTP2_RST_STREAM) {
            stream->peer = stream->dirty = true;
            if (!stream->error) stream->error = stream->request->bodyError = ECONNRESET;
            return 0;
        }
        if (stream->error) return 0;
        if (frame->hd.type == NGHTTP2_HEADERS && !stream->initialHeaders) {
            stream->initialHeaders = true;
            auto& request = *stream->request;
            const Field* host = nullptr;
            for (std::size_t index = 0; index < request.headerCount; ++index) {
                auto& header = request.headers[index];
                if (text(header.name) != "host") continue;
                if (host) { self.rejected(*stream, EPROTO); return 0; }
                host = &header;
            }
            if (stream->scheme != "https" || stream->method.empty() || stream->path.empty() ||
                (stream->authority.empty() && !host) || (host && !stream->authority.empty() && !equalAscii(text(host->value), stream->authority))) {
                self.rejected(*stream, EPROTO);
                return 0;
            }
            if (!host) request.headers[request.headerCount++] = {
                lyraStringFromBytes("host", 4), lyraStringFromBytes(stream->authority.data(), stream->authority.size()),
            };
            auto status = populateRequest(request, stream->method, stream->path, {request.headers, request.headerCount});
            if (status) { self.rejected(*stream, status); return 0; }
            if (request.streamingBody) self.announce(*stream);
        }
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
            self.finishInput(*stream);
        stream->dirty = true;
        return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }

    static int sent(nghttp2_session*, const nghttp2_frame* frame, void* data) {
        auto& self = *static_cast<Impl*>(data);
        auto* stream = self.find(frame->hd.stream_id);
        if (!stream) return 0;
        if (stream->writePending && !stream->writeAfter) {
            const bool headers = frame->hd.type == NGHTTP2_HEADERS && (stream->startOnly || stream->finishWhenHeaders);
            const bool payload = frame->hd.type == NGHTTP2_DATA && !stream->startOnly &&
                (!stream->outgoing || stream->outgoingOffset == lyraStringLength(stream->outgoing));
            if (headers || payload) stream->writeAfter = self.serial;
        }
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            stream->responseEnd = true;
            if (!stream->remoteEnd && !stream->resetSubmitted) {
                stream->resetSubmitted = true;
                self.submit(nghttp2_submit_rst_stream(self.session, NGHTTP2_FLAG_NONE, stream->request->streamId, NGHTTP2_NO_ERROR));
            }
        }
        return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }

    static int notSent(nghttp2_session*, const nghttp2_frame* frame, int result, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (auto* stream = self.find(frame->hd.stream_id)) self.failStream(*stream, nativeStatus(result));
        return self.failure ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }

    static int closed(nghttp2_session*, int32_t id, uint32_t error, void* data) {
        auto& self = *static_cast<Impl*>(data);
        auto* stream = self.find(id);
        if (!stream) return 0;
        stream->wireClosed = true;
        stream->closeAfter = self.inSend ? self.serial : 0;
        if (error != NGHTTP2_NO_ERROR && !stream->error) {
            stream->error = stream->request->bodyError = ECONNRESET;
            stream->peer = true;
        }
        return 0;
    }

    static nghttp2_ssize provide(nghttp2_session*, int32_t, uint8_t* bytes, std::size_t size,
        uint32_t* flags, nghttp2_data_source* source, void*) {
        auto& stream = *static_cast<Stream*>(source->ptr);
        if (!stream.writePending || stream.startOnly || stream.writeAfter) return NGHTTP2_ERR_DEFERRED;
        const auto body = stream.outgoing ? text(stream.outgoing) : std::string_view{};
        auto count = std::min({size, writeChunk, body.size() - stream.outgoingOffset});
        if (count) std::memcpy(bytes, body.data() + stream.outgoingOffset, count);
        stream.outgoingOffset += count;
        if (stream.finalWrite && stream.outgoingOffset == body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<nghttp2_ssize>(count);
    }
};

Session::Session(const ServerOptions& options, Hooks hooks, void* user) : impl(std::make_unique<Impl>(options, hooks, user)) {}
Session::~Session() = default;

int Session::status() const { return impl->failure; }

int Session::feed(std::string_view bytes) {
    auto& state = *impl;
    if (state.failure) return state.failure;
    auto consumed = nghttp2_session_mem_recv2(state.session, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (consumed < 0) state.failure = nativeStatus(static_cast<int>(consumed));
    else require(static_cast<std::size_t>(consumed) == bytes.size(), "nghttp2 paused an unpausable input callback");
    if (state.accepted >= state.requestLimit) state.drain();
    state.flush();
    return state.failure;
}

std::string_view Session::output() {
    auto& state = *impl;
    if (state.failure || state.aborted) return {};
    if (state.pendingOutput.empty()) {
        const uint8_t* bytes = nullptr;
        ++state.serial;
        state.inSend = true;
        auto count = nghttp2_session_mem_send2(state.session, &bytes);
        state.inSend = false;
        if (count < 0) state.failure = nativeStatus(static_cast<int>(count));
        else if (count) state.pendingOutput = {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(count)};
        else state.acknowledged = state.serial;
        state.flush();
    }
    return state.pendingOutput.substr(0, writeChunk);
}

void Session::consumeOutput(std::size_t count) {
    auto& state = *impl;
    require(count <= std::min(state.pendingOutput.size(), writeChunk), "output completion exceeded its published byte count");
    state.pendingOutput.remove_prefix(count);
    if (state.pendingOutput.empty()) {
        state.acknowledged = state.serial;
        state.flush();
    }
}

bool Session::wantsRead() const {
    return !impl->failure && !impl->aborted && nghttp2_session_want_read(impl->session) && nghttp2_session_get_outbound_queue_size(impl->session) < maxOutbound;
}

bool Session::finished() const {
    return impl->streams.empty() && impl->pendingOutput.empty() &&
        (impl->aborted || impl->failure || (!nghttp2_session_want_read(impl->session) && !nghttp2_session_want_write(impl->session)));
}

std::size_t Session::activeStreams() const { return impl->streams.size(); }

bool Session::inputComplete(Request& request) const {
    auto* stream = impl->find(request);
    return stream && stream->remoteEnd;
}

void Session::drain() { impl->drain(); impl->flush(); }

void Session::abort(int status) {
    impl->aborted = true;
    for (auto& entry : impl->streams) {
        auto& stream = *entry.second;
        stream.error = stream.request->bodyError = status;
        stream.wireClosed = true;
        stream.closeAfter = 0;
    }
    impl->flush();
}

BodyRead Session::read(Request& request) {
    auto& state = *impl;
    auto* stream = state.find(request);
    if (!stream) return {ReadState::Error, nullptr, request.bodyError ? request.bodyError : EBADF};
    if (stream->error) return {ReadState::Error, nullptr, stream->error};
    if (!request.streamingBody) return {ReadState::Error, nullptr, EINVAL};
    const auto count = std::min(readChunk, stream->body.size() - stream->bodyOffset);
    if (count) {
        auto* bytes = lyraStringFromBytes(stream->body.data() + stream->bodyOffset, count);
        stream->bodyOffset += count;
        if (stream->bodyOffset == stream->body.size()) { stream->body.clear(); stream->bodyOffset = 0; }
        state.submit(nghttp2_session_consume_stream(state.session, request.streamId, count));
        return {ReadState::Bytes, bytes};
    }
    return {stream->remoteEnd ? ReadState::End : ReadState::Pending};
}

int Session::respond(Request& request, int64_t code, const void* mediaType, const void* body, const ResponseFields& headers, bool streaming) {
    auto& state = *impl;
    auto* stream = state.find(request);
    if (!stream || stream->error || stream->wireClosed || stream->responding) return EBADF;
    std::string checked;
    if (auto status = formatResponse(checked, code, mediaType, body, headers, true, false, streaming)) return status;
    const auto statusText = std::to_string(code), lengthText = std::to_string(lyraStringLength(body));
    std::vector<nghttp2_nv> fields{field(":status", statusText), field("content-type", text(mediaType))};
    if (!streaming && code != 204 && code != 304) fields.push_back(field("content-length", lengthText));
    for (std::size_t index = 0; index < headers.count; ++index) fields.push_back(field(text(headers.fields[index].name), text(headers.fields[index].value)));
    stream->responding = true;
    stream->streaming = streaming;
    stream->writePending = true;
    stream->startOnly = streaming;
    stream->finalWrite = !streaming;
    const bool hasBody = text(request.method) != "HEAD" && lyraStringLength(body) != 0;
    stream->finishWhenHeaders = !streaming && !hasBody;
    if (hasBody) stream->pin(body);
    const nghttp2_data_provider2 provider{{.ptr = stream}, Impl::provide};
    auto result = nghttp2_submit_response2(state.session, request.streamId, fields.data(), fields.size(),
        streaming || hasBody ? &provider : nullptr);
    if (result) {
        stream->responding = stream->writePending = false;
        stream->unpin();
        return nativeStatus(result);
    }
    return 0;
}

int Session::write(Request& request, const void* bytes, bool final) {
    auto& state = *impl;
    auto* stream = state.find(request);
    if (!stream || stream->error || stream->wireClosed || !stream->streaming || stream->responseEnd) return EBADF;
    if (stream->writePending || stream->completion) return EBUSY;
    stream->writePending = true;
    stream->finalWrite = final;
    stream->startOnly = false;
    stream->finishWhenHeaders = false;
    if (text(request.method) != "HEAD") stream->pin(bytes);
    if (!final && (!stream->outgoing || text(stream->outgoing).empty())) {
        state.completion(*stream, 0);
        state.flush();
        return 0;
    }
    auto result = nghttp2_session_resume_data(state.session, request.streamId);
    if (result) {
        stream->writePending = false;
        stream->unpin();
        return nativeStatus(result);
    }
    return 0;
}

std::optional<int> Session::takeWriteCompletion(Request& request) {
    auto* stream = impl->find(request);
    if (!stream) return {};
    return std::exchange(stream->completion, {});
}

void Session::reset(Request& request, int status, bool peer) {
    if (auto* stream = impl->find(request); stream && !stream->responseEnd) impl->failStream(*stream, status, NGHTTP2_CANCEL, peer);
    impl->flush();
}

}
