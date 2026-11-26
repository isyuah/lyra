#include "http_protocol.h"
#include "http_transport.h"
#include "native_async.h"
#include "strings.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lyraRuntime::http {

namespace {

constexpr int64_t forever = INT64_MAX;
constexpr std::size_t readChunk = 8192, writeChunk = 16 * 1024;
constexpr std::size_t ingressBatch = 32, commandBatch = 64, maxSubmissions = 4096;
constexpr int64_t rejectionTimeoutMs = 1000;
constexpr std::string_view overloadResponse = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";

enum class Stop { Open, Drain, Close };
enum class OperationKind { Accept, Respond, Observe, CloseRequest, CloseListener, ListenerScope };
enum class Phase { Handshake, Idle, Reading, Ready, Writing, EndingTls, EndingTcp, RejectDrain, Closing };

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "Lyra HTTP reactor: %s\n", message);
    std::abort();
}

int64_t now() { return static_cast<int64_t>(uv_hrtime() / UINT64_C(1000000)); }

int64_t deadline(int64_t duration, int64_t from = now()) { return duration < 0 ? forever : from + duration; }

bool validWait(int64_t duration) { return duration >= -1 && duration <= INT_MAX; }

void arm(uv_timer_t& timer, uv_timer_cb callback, int64_t due) {
    if (due == forever) { uv_timer_stop(&timer); return; }
    uv_update_time(timer.loop);
    require(uv_timer_start(&timer, callback, static_cast<uint64_t>(std::max<int64_t>(0, due - now())), 0) == 0,
        "cannot schedule deadline");
}

int statusCode(int uvStatus) {
    switch (uvStatus) {
        case UV_EOF: case UV_ECONNRESET: return ECONNRESET;
        case UV_ECANCELED: return ECANCELED;
        case UV_EPIPE: return EPIPE;
        case UV_ETIMEDOUT: return ETIMEDOUT;
        case UV_EBADF: return EBADF;
        case UV_EINVAL: return EINVAL;
        case UV_ENOMEM: return ENOMEM;
        default: return -uvStatus;
    }
}

struct Server;
struct Operation;
struct Pool;

struct Command {
    void (*run)(void*);
    void* data;
    LyraGcRoot* root;
    bool submission;
};

}

struct Reactor {
    Pool* pool{};
    std::size_t index{};
    uv_loop_t loop;
    uv_async_t inbox;
    uv_thread_t thread;
    std::mutex mutex;
    std::deque<Command> commands;
    std::size_t submissions{};
    std::unordered_set<Server*> listeners;

    int initialize();
    void dispatch();

    template<auto Run, class Data>
    bool post(Data& data, bool submission = false, bool managed = true) {
        auto* root = managed ? lyraGcRootCreate(&data) : nullptr;
        {
            std::lock_guard lock(mutex);
            if (!submission || submissions < maxSubmissions) {
                commands.push_back({[](void* payload) { std::invoke(Run, *static_cast<Data*>(payload)); }, &data, root, submission});
                submissions += submission;
                require(uv_async_send(&inbox) == 0, "cannot notify reactor inbox");
                return true;
            }
        }
        if (root) lyraGcRootDrop(root);
        return false;
    }

    template<auto Run, class Data>
    void call(Data& data) {
        struct Call { Data& data; uv_sem_t done; } call{data, {}};
        require(uv_sem_init(&call.done, 0) == 0, "cannot initialize reactor rendezvous");
        post<[](Call& request) { Run(request.data); uv_sem_post(&request.done); }>(call, false, false);
        uv_sem_wait(&call.done);
        uv_sem_destroy(&call.done);
    }
};

struct Endpoint {
    ServerOptions options{};
    Reactor* reactor{};
    Server* server{};
    const Transport* transport{};
    void* tlsContext{};
    std::atomic<Stop> requested{Stop::Open};
    std::atomic<bool> stopped{};
    int port{}, status{};
    const void* detail{};
};

namespace {

struct Pool {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Reactor, 64> shards;
    std::array<std::size_t, 64> loads{};
    std::size_t count{}, activeListeners{}, nextListener{}, nextConnection{1};

    ~Pool();
    int start();
    void stop();
    Reactor& coordinator();
    Reactor& place();
    void release(Reactor& reactor);
    void listenerAdded();
    void listenerRemoved();
};

struct Operation {
    OperationKind kind;
    Reactor* reactor;
    Endpoint* endpoint{};
    Request* request{};
    int64_t argument{};
    const void* mediaType{};
    const void* body{};
    const ResponseFields* headers{};
    bool scoped{}, reportRejections{};
    int64_t expires{forever};
    LyraNativeAsync* task{};

    void start();
    void cancel();
};

struct Slot {
    LyraGcRoot* root;
    std::list<Request*>::iterator ready;
    bool issued{};
};

struct Server {
    Endpoint& endpoint;
    LyraGcRoot* root;
    net::Socket socket;
    uv_poll_t poll;
    uv_timer_t timer;
    Stop stopping{Stop::Open};
    bool polling{}, listenerClosed{}, finishing{}, rejecting{};
    std::size_t ordinary{}, issued{};
    int64_t retryAt{forever}, retryMs{};
    std::unordered_map<Session*, Slot> connections;
    std::list<Request*> ready;
    std::list<Operation*> accepts, closeWaiters;

    Server(Endpoint& endpoint, net::Socket socket) : endpoint(endpoint), root(lyraGcRootCreate(&endpoint)), socket(socket) {}
    int start();
    int pollForAccept();
    void acceptReady();
    void offer(Request& request);
    void released(Request& request);
    void closed(Session& session);
    void dispatch();
    void registerAccept(Operation& operation);
    void expire(int64_t time);
    void refreshTimer();
    void reportRejected(int status);
    void stop(Stop mode, int error = EBADF);
    void maybeFinish();
};

template<class Value>
void* box(Value value) {
    return ::new (lyraGcAlloc(sizeof(Value))) Value(value);
}

Request* acceptError(int status) { return ::new (lyraGcAlloc(sizeof(Request))) Request{.status = status}; }

uint8_t unit{};

void finish(Operation& operation, int status = 0) {
    auto* task = operation.task;
    operation.task = nullptr;
    switch (operation.kind) {
        case OperationKind::Accept: lyraNativeComplete(task, box(acceptError(status))); break;
        case OperationKind::Respond: case OperationKind::Observe: lyraNativeComplete(task, box<int64_t>(status)); break;
        default: lyraNativeComplete(task, &unit); break;
    }
}

LyraTask* nativeTask(Operation input) {
    auto* operation = ::new (lyraGcAlloc(sizeof(Operation))) Operation(input);
    void* rejected = nullptr;
    switch (operation->kind) {
        case OperationKind::Accept: rejected = box(acceptError(EBUSY)); break;
        case OperationKind::Respond: rejected = box<int64_t>(EBUSY); break;
        case OperationKind::Observe: rejected = box<int64_t>(-EBUSY); break;
        default: rejected = &unit; break;
    }
    return lyraTaskNative([](LyraNativeAsync* task, void* data) {
        auto& operation = *static_cast<Operation*>(data);
        if (operation.kind == OperationKind::CloseRequest && operation.request->delivery.load() == Delivery::Closed) return false;
        if ((operation.kind == OperationKind::CloseListener || operation.kind == OperationKind::ListenerScope) && operation.endpoint->stopped.load())
            return false;
        operation.task = task;
        if (operation.kind == OperationKind::Accept || operation.kind == OperationKind::Observe)
            operation.expires = validWait(operation.argument) ? deadline(operation.argument) : forever;
        const bool limited = operation.kind == OperationKind::Accept || operation.kind == OperationKind::Respond || operation.kind == OperationKind::Observe;
        return operation.reactor->post<&Operation::start>(operation, limited);
    }, [](void* data) {
        auto& operation = *static_cast<Operation*>(data);
        operation.reactor->post<&Operation::cancel>(operation);
    }, operation, rejected);
}

LyraTask* requestCloseTask(Request& request) {
    return nativeTask({OperationKind::CloseRequest, request.session->reactor, request.session->endpoint, &request});
}

void applyStop(Endpoint& endpoint) {
    if (endpoint.server) endpoint.server->stop(endpoint.requested.load());
}

void requestStop(Endpoint& endpoint, Stop mode) {
    auto previous = endpoint.requested.load();
    while (previous < mode) if (endpoint.requested.compare_exchange_weak(previous, mode)) {
        endpoint.reactor->post<&applyStop>(endpoint);
        return;
    }
}

void closeRequest(Request& request, bool includeClaimed);

}

struct Connection {
    Session& session;
    Request* request;
    Reactor& reactor;
    const Transport* transport;
    void* tls{};
    uv_tcp_t tcp;
    uv_timer_t timer;
    uv_write_t write;
    uv_shutdown_t shutdown;
    Phase phase{Phase::Reading};
    unsigned closingHandles{};
    int closeStatus{}, ingressError{};
    int64_t phaseDue{forever}, idleDue{forever};
    bool tcpEof{}, readActive{}, writePending{}, pumpQueued{}, bioInputEnded{}, tlsReadEnded{}, tlsShutdownDone{}, tlsWriteRetry{};
    std::size_t inputOffset{}, inputSize{}, completedRequests{}, discarded{};
    std::array<char, readChunk> input;
    struct TlsBuffers { std::array<char, writeChunk> ciphertext; std::array<char, readChunk> plaintext; };
    std::unique_ptr<TlsBuffers> tlsBuffers;
    std::unique_ptr<RequestParser> parser;
    std::string responseHead, pendingInput;
    std::string_view outgoingHead, outgoingBody;
    Operation* response{};
    std::list<Operation*> observers, closeWaiters;

    explicit Connection(Session& session) : session(session), request(session.request), reactor(*session.reactor), transport(session.endpoint->transport) {}
    void open();
    void readFromSocket();
    void readResult(std::ptrdiff_t count);
    void beginMessage();
    void finishResponse();
    void messageBytes(std::string_view bytes);
    void startResponse(Operation& operation);
    void sendRejection(std::string_view wire, int status);
    void pump();
    void pumpTls();
    void queueWrite(std::string_view bytes);
    void finishTcp();
    std::string_view nextChunk() const;
    void consumeChunk(std::size_t count);
    void observe(Operation& operation);
    void refreshTimer();
    void expire();
    void beginClose(int status = 0, bool peer = false);
    void finalize();
};

namespace {

void closeRequest(Request& request, bool includeClaimed) {
    auto delivery = request.delivery.load();
    while (delivery < Delivery::Closing && (includeClaimed || delivery != Delivery::Claimed))
        if (request.delivery.compare_exchange_weak(delivery, Delivery::Closing)) {
            request.session->reactor->post<[](Request& request) { if (auto* connection = request.connection) connection->beginClose(EBADF); }>(request);
            return;
        }
}

int claim(Request& request) {
    auto delivery = Delivery::Offered;
    if (request.delivery.compare_exchange_strong(delivery, Delivery::Claimed) || delivery == Delivery::Claimed) return 0;
    return EBADF;
}

void notifyClosed(Session& session) {
    session.request->delivery.store(Delivery::Closed);
    session.endpoint->reactor->post<[](Session& session) { session.endpoint->server->closed(session); }>(session);
}

std::string_view rejection(int status) {
    switch (status) {
        case EPROTO: case EILSEQ: return "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        case ETIMEDOUT: return "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        case EMSGSIZE: return "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        case unsupportedExpectation: return "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        default: return {};
    }
}

}

void Connection::open() {
    auto socket = session.socket;
    if (session.endpoint->requested.load() != Stop::Open) {
        net::close(socket);
        notifyClosed(session);
        delete this;
        return;
    }
    session.connection = request->connection = this;
    require(uv_tcp_init(&reactor.loop, &tcp) == 0 && uv_timer_init(&reactor.loop, &timer) == 0, "cannot initialize TCP handles");
    tcp.data = timer.data = write.data = shutdown.data = this;
    if (auto status = uv_tcp_open(&tcp, socket)) {
        net::close(socket);
        ingressError = statusCode(status);
        beginClose(ingressError);
        return;
    }
    uv_tcp_nodelay(&tcp, 1);
    if (transport) {
        phase = Phase::Handshake;
        if (!(tls = transport->open(session.endpoint->tlsContext))) {
            ingressError = handshakeFailed;
            beginClose(ingressError);
            return;
        }
        tlsBuffers = std::make_unique_for_overwrite<TlsBuffers>();
        auto timeout = session.endpoint->options.handshakeTimeoutMs;
        phaseDue = deadline(session.overload ? std::min(timeout, rejectionTimeoutMs) : timeout, session.acceptedAt);
    } else beginMessage();
    readFromSocket();
    pump();
    refreshTimer();
}

void Connection::readFromSocket() {
    if (readActive || tcpEof || phase == Phase::Closing || !pendingInput.empty() || (tls && inputOffset != inputSize)) return;
    auto status = uv_read_start(reinterpret_cast<uv_stream_t*>(&tcp), [](uv_handle_t* handle, std::size_t, uv_buf_t* buffer) {
        auto& connection = *static_cast<Connection*>(handle->data);
        *buffer = uv_buf_init(connection.input.data(), static_cast<unsigned>(connection.input.size()));
    }, [](uv_stream_t* stream, std::ptrdiff_t count, const uv_buf_t*) {
        static_cast<Connection*>(stream->data)->readResult(count);
    });
    if (status) { beginClose(statusCode(status), true); return; }
    readActive = true;
}

void Connection::readResult(std::ptrdiff_t count) {
    if (!count || phase == Phase::Closing) return;
    if (phase == Phase::RejectDrain) {
        if (count < 0) beginClose();
        else {
            discarded += static_cast<std::size_t>(count);
            if (discarded > static_cast<std::size_t>(session.endpoint->options.maxRequestBytes)) beginClose();
        }
        return;
    }
    if (count < 0) {
        readActive = false;
        if (count != UV_EOF) { beginClose(statusCode(static_cast<int>(count)), true); return; }
        tcpEof = true;
        if (!tls && (phase == Phase::Reading || phase == Phase::Idle)) { beginClose(ECONNRESET, true); return; }
    } else if (tls) {
        inputOffset = 0;
        inputSize = static_cast<std::size_t>(count);
    } else messageBytes({input.data(), static_cast<std::size_t>(count)});
    pump();
}

void Connection::beginMessage() {
    if (session.overload) { sendRejection(overloadResponse, 0); return; }
    phase = Phase::Reading;
    const auto& options = session.endpoint->options;
    parser = std::make_unique<RequestParser>(*request, options.maxBodyBytes, options.maxRequestBytes);
    auto time = tls || completedRequests ? now() : session.acceptedAt;
    phaseDue = deadline(options.requestTimeoutMs, time);
    idleDue = deadline(options.idleReadTimeoutMs, time);
    refreshTimer();
}

void Connection::messageBytes(std::string_view bytes) {
    if (phase == Phase::Idle) beginMessage();
    if (phase != Phase::Reading) {
        if (ingressError || session.overload) {
            discarded += bytes.size();
            if (discarded > static_cast<std::size_t>(session.endpoint->options.maxRequestBytes)) beginClose();
            return;
        }
        if (phase == Phase::Ready || phase == Phase::Writing) {
            require(pendingInput.size() + bytes.size() <= readChunk, "pipelined input exceeded its read window");
            pendingInput.append(bytes);
            if (readActive) { uv_read_stop(reinterpret_cast<uv_stream_t*>(&tcp)); readActive = false; }
        }
        return;
    }
    if (auto status = parser->feed(bytes)) { sendRejection(rejection(status), status); return; }
    if (!parser->complete) {
        if (auto idle = session.endpoint->options.idleReadTimeoutMs; idle >= 0) {
            idleDue = deadline(idle);
            refreshTimer();
        }
        return;
    }
    bytes.remove_prefix(parser->consumed);
    parser.reset();
    phase = Phase::Ready;
    phaseDue = idleDue = forever;
    refreshTimer();
    request->session->endpoint->reactor->post<[](Request& request) { request.session->endpoint->server->offer(request); }>(*request);
    if (!bytes.empty()) messageBytes(bytes);
}

void Connection::startResponse(Operation& operation) {
    if (response && phase != Phase::Closing) { finish(operation, EBUSY); return; }
    if (phase != Phase::Ready || request->delivery.load() >= Delivery::Closing) { finish(operation, EBADF); return; }
    request->keepAlive = request->keepAlive && session.endpoint->options.keepAliveTimeoutMs != 0 &&
        completedRequests + 1 < static_cast<std::size_t>(session.endpoint->options.maxRequestsPerConnection) &&
        session.endpoint->requested.load() == Stop::Open && !tcpEof;
    if (auto status = formatResponse(responseHead, operation.argument, operation.mediaType, operation.body, *operation.headers, request->keepAlive)) {
        finish(operation, status);
        return;
    }
    if (auto status = claim(*request)) { finish(operation, status); return; }
    response = &operation;
    outgoingHead = responseHead;
    if (text(request->method) != "HEAD") outgoingBody = text(operation.body);
    phase = Phase::Writing;
    phaseDue = deadline(session.endpoint->options.writeTimeoutMs);
    refreshTimer();
    pump();
}

void Connection::sendRejection(std::string_view wire, int status) {
    ingressError = status;
    parser.reset();
    if (wire.empty()) { beginClose(status); return; }
    phase = Phase::Writing;
    outgoingHead = wire;
    phaseDue = deadline(std::min(session.endpoint->options.writeTimeoutMs, rejectionTimeoutMs));
    idleDue = forever;
    refreshTimer();
}

std::string_view Connection::nextChunk() const {
    return (outgoingHead.empty() ? outgoingBody : outgoingHead).substr(0, writeChunk);
}

void Connection::consumeChunk(std::size_t count) {
    (outgoingHead.empty() ? outgoingBody : outgoingHead).remove_prefix(count);
}

void Connection::queueWrite(std::string_view bytes) {
    auto buffer = uv_buf_init(const_cast<char*>(bytes.data()), static_cast<unsigned>(bytes.size()));
    writePending = true;
    auto status = uv_write(&write, reinterpret_cast<uv_stream_t*>(&tcp), &buffer, 1, [](uv_write_t* write, int status) {
        auto& connection = *static_cast<Connection*>(write->data);
        connection.writePending = false;
        if (connection.phase == Phase::Closing) return;
        if (status) connection.beginClose(statusCode(status), true);
        else connection.pump();
    });
    if (status) { writePending = false; beginClose(statusCode(status), true); }
}

void Connection::finishTcp() {
    phase = Phase::EndingTcp;
    auto status = uv_shutdown(&shutdown, reinterpret_cast<uv_stream_t*>(&tcp), [](uv_shutdown_t* shutdown, int status) {
        auto& connection = *static_cast<Connection*>(shutdown->data);
        if (!status && connection.ingressError && !connection.tcpEof) {
            connection.phase = Phase::RejectDrain;
            connection.phaseDue = deadline(rejectionTimeoutMs);
            connection.idleDue = forever;
            connection.inputOffset = connection.inputSize = 0;
            connection.refreshTimer();
            connection.readFromSocket();
        } else connection.beginClose(statusCode(status), status != 0);
    });
    if (status) beginClose(statusCode(status), true);
}

void Connection::finishResponse() {
    if (!response || !request->keepAlive || tcpEof || tlsReadEnded || session.endpoint->requested.load() != Stop::Open) {
        if (tls) phase = Phase::EndingTls;
        else finishTcp();
        return;
    }
    auto* completed = request;
    auto* operation = std::exchange(response, nullptr);
    completed->connection = nullptr;
    completed->delivery.store(Delivery::Closed);
    completed->session->endpoint->reactor->post<[](Request& request) { request.session->endpoint->server->released(request); }>(*completed);
    auto pendingObservers = std::move(observers);
    for (auto* observer : pendingObservers) finish(*observer);
    ++completedRequests;
    request = session.request = ::new (lyraGcAlloc(sizeof(Request))) Request{.session = &session, .connection = this};
    phase = Phase::Idle;
    phaseDue = deadline(session.endpoint->options.keepAliveTimeoutMs);
    idleDue = forever;
    auto buffered = std::move(pendingInput);
    pendingInput.clear();
    finish(*operation);
    refreshTimer();
    if (!buffered.empty()) messageBytes(buffered);
    readFromSocket();
}

void Connection::pump() {
    if (phase == Phase::Closing || phase == Phase::EndingTcp || phase == Phase::RejectDrain) return;
    if (request->delivery.load() == Delivery::Closing) { beginClose(EBADF); return; }
    if (tls) { pumpTls(); return; }
    if (writePending || phase != Phase::Writing) return;
    auto bytes = nextChunk();
    if (bytes.empty()) finishResponse();
    else { consumeChunk(bytes.size()); queueWrite(bytes); }
}

void Connection::pumpTls() {
    auto flush = [&] {
        auto& buffer = tlsBuffers->ciphertext;
        auto count = transport->flush(tls, buffer.data(), buffer.size());
        if (count) queueWrite({buffer.data(), count});
        return count != 0;
    };
    auto receive = [&] {
        auto& buffer = tlsBuffers->plaintext;
        auto step = transport->read(tls, buffer.data(), buffer.size());
        if (step.progress == TlsProgress::Done) messageBytes({buffer.data(), step.bytes});
        if (step.progress == TlsProgress::End) {
            tlsReadEnded = true;
            if (phase == Phase::Reading || phase == Phase::Idle) beginClose(ECONNRESET, true);
        }
        return step.progress;
    };
    std::size_t stalledInput = SIZE_MAX;
    for (std::size_t stepIndex = 0; stepIndex < 16; ++stepIndex) {
        if (inputOffset != inputSize) inputOffset += transport->feed(tls, input.data() + inputOffset, inputSize - inputOffset);
        if (inputOffset != inputSize && readActive) {
            uv_read_stop(reinterpret_cast<uv_stream_t*>(&tcp));
            readActive = false;
        } else readFromSocket();
        if (phase == Phase::Closing) return;
        if (tcpEof && inputOffset == inputSize && !bioInputEnded) {
            transport->endInput(tls);
            bioInputEnded = true;
        }
        if (writePending || flush()) return;
        TlsProgress progress;
        if (phase == Phase::Handshake) {
            progress = transport->handshake(tls).progress;
            if (progress == TlsProgress::Done) beginMessage();
            else if (progress == TlsProgress::Failed || progress == TlsProgress::End) {
                ingressError = handshakeFailed;
                beginClose(ingressError, true);
                return;
            }
        } else if (phase == Phase::Idle || phase == Phase::Reading || phase == Phase::Ready) {
            if (tlsReadEnded || !pendingInput.empty()) return;
            progress = receive();
        } else if (bioInputEnded && !tlsReadEnded && !tlsWriteRetry) progress = receive();
        else if (phase == Phase::Writing) {
            auto bytes = nextChunk();
            if (bytes.empty()) { finishResponse(); continue; }
            auto step = transport->write(tls, bytes.data(), bytes.size());
            progress = step.progress;
            tlsWriteRetry = progress == TlsProgress::WantInput || progress == TlsProgress::WantOutput;
            if (progress == TlsProgress::Done) consumeChunk(step.bytes);
            if (progress == TlsProgress::End) { beginClose(ECONNRESET, true); return; }
        } else {
            if (tlsShutdownDone) { finishTcp(); return; }
            progress = transport->shutdown(tls).progress;
            tlsShutdownDone = progress == TlsProgress::Done;
        }
        if (phase == Phase::Closing) return;
        if (progress == TlsProgress::Failed) { beginClose(ECONNRESET, true); return; }
        if (flush()) return;
        if (progress == TlsProgress::End && phase == Phase::Ready) return;
        if (progress == TlsProgress::WantOutput) { beginClose(EPROTO, true); return; }
        if (progress == TlsProgress::WantInput) {
            if (inputOffset == inputSize) {
                if (bioInputEnded) beginClose(ECONNRESET, true);
                return;
            }
            if (stalledInput == inputOffset) { beginClose(EPROTO, true); return; }
            stalledInput = inputOffset;
        } else stalledInput = SIZE_MAX;
    }
    if (!pumpQueued) {
        pumpQueued = true;
        reactor.post<[](Session& session) {
            if (auto* connection = session.connection) { connection->pumpQueued = false; connection->pump(); }
        }>(session);
    }
}

void Connection::observe(Operation& operation) {
    if (!validWait(operation.argument)) { finish(operation, -EINVAL); return; }
    if (observers.size() >= 64) { finish(operation, -EBUSY); return; }
    if (operation.expires <= now()) { finish(operation, request->peerLost.load()); return; }
    observers.push_back(&operation);
    refreshTimer();
}

void Connection::refreshTimer() {
    if (phase == Phase::Closing) return;
    auto due = std::min(phaseDue, idleDue);
    for (auto* operation : observers) due = std::min(due, operation->expires);
    arm(timer, [](uv_timer_t* timer) { static_cast<Connection*>(timer->data)->expire(); }, due);
}

void Connection::expire() {
    auto time = now();
    for (auto iterator = observers.begin(); iterator != observers.end();) {
        auto& operation = **iterator;
        if (operation.expires > time) { ++iterator; continue; }
        iterator = observers.erase(iterator);
        finish(operation, request->peerLost.load());
    }
    if (phaseDue <= time || idleDue <= time) {
        switch (phase) {
            case Phase::Handshake: ingressError = handshakeTimeout; beginClose(ingressError); return;
            case Phase::Idle: beginClose(); return;
            case Phase::Reading: sendRejection(rejection(ETIMEDOUT), ETIMEDOUT); pump(); return;
            case Phase::Ready: {
                auto pending = Delivery::Offered;
                if (request->delivery.compare_exchange_strong(pending, Delivery::Closing)) { beginClose(ETIMEDOUT); return; }
                phaseDue = forever;
                break;
            }
            default: beginClose(ETIMEDOUT); return;
        }
    }
    refreshTimer();
}

void Connection::beginClose(int status, bool peer) {
    if (phase == Phase::Closing) return;
    auto previous = request->delivery.exchange(Delivery::Closing);
    if (previous == Delivery::Closing && peer) status = EBADF;
    else if (peer) request->peerLost.store(true);
    if (peer && !ingressError && (phase == Phase::Reading || phase == Phase::Handshake))
        ingressError = phase == Phase::Handshake ? handshakeFailed : status;
    phase = Phase::Closing;
    closeStatus = status;
    parser.reset();
    if (readActive) uv_read_stop(reinterpret_cast<uv_stream_t*>(&tcp));
    uv_timer_stop(&timer);
    closingHandles = 2;
    auto callback = [](uv_handle_t* handle) {
        auto& connection = *static_cast<Connection*>(handle->data);
        if (--connection.closingHandles == 0) connection.finalize();
    };
    uv_close(reinterpret_cast<uv_handle_t*>(&tcp), callback);
    uv_close(reinterpret_cast<uv_handle_t*>(&timer), callback);
}

void Connection::finalize() {
    require(!writePending, "closing TCP stream still owns a write buffer");
    if (tls) transport->releaseConnection(tls);
    auto* closed = &session;
    auto* root = lyraGcRootCreate(closed);
    auto pendingObservers = std::move(observers), pendingCloses = std::move(closeWaiters);
    auto* pendingResponse = response;
    auto status = closeStatus;
    auto lost = request->peerLost.load();
    closed->connection = request->connection = nullptr;
    closed->ingressError = ingressError;
    delete this;
    notifyClosed(*closed);
    for (auto* operation : pendingObservers) finish(*operation, lost);
    if (pendingResponse) finish(*pendingResponse, status);
    for (auto* operation : pendingCloses) finish(*operation);
    lyraGcRootDrop(root);
}

int Reactor::initialize() {
    if (auto status = uv_loop_init(&loop)) return status;
    if (auto status = uv_async_init(&loop, &inbox, [](uv_async_t* inbox) { static_cast<Reactor*>(inbox->data)->dispatch(); })) {
        uv_loop_close(&loop);
        return status;
    }
    inbox.data = this;
    if (auto status = uv_thread_create(&thread, [](void* data) {
        auto& reactor = *static_cast<Reactor*>(data);
        auto registration = lyraGcEnterWorker();
        uv_run(&reactor.loop, UV_RUN_DEFAULT);
        require(uv_loop_close(&reactor.loop) == 0, "reactor loop has unclosed handles");
        lyraGcLeaveWorker(registration);
    }, this)) {
        uv_close(reinterpret_cast<uv_handle_t*>(&inbox), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        return status;
    }
    return 0;
}

void Reactor::dispatch() {
    std::array<Command, commandBatch> batch;
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex);
        for (; count < batch.size() && !commands.empty(); ++count) {
            batch[count] = commands.front();
            submissions -= batch[count].submission;
            commands.pop_front();
        }
        if (!commands.empty()) require(uv_async_send(&inbox) == 0, "cannot continue reactor inbox");
    }
    for (std::size_t index = 0; index < count; ++index) {
        auto& command = batch[index];
        command.run(command.data);
        if (command.root) lyraGcRootDrop(command.root);
    }
}

namespace {

void stopReactor(Reactor& reactor) {
    uv_close(reinterpret_cast<uv_handle_t*>(&reactor.inbox), nullptr);
}

Pool::~Pool() { stop(); }

Pool& processPool() {
    static Pool pool;
    return pool;
}

void Pool::stop() {
    for (std::size_t index = 0; index < count; ++index) shards[index].call<[](Reactor& reactor) {
        for (auto* listener : reactor.listeners) {
            listener->endpoint.requested.store(Stop::Close);
            listener->stop(Stop::Close);
        }
    }>(shards[index]);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return activeListeners == 0; });
    }
    for (std::size_t index = 0; index < count; ++index) shards[index].call<&stopReactor>(shards[index]);
    for (std::size_t index = 0; index < count; ++index) uv_thread_join(&shards[index].thread);
    count = 0;
}

int Pool::start() {
    std::lock_guard lock(mutex);
    if (count) return 0;
    char setting[16];
    std::size_t size = sizeof(setting);
    std::size_t desired;
    auto status = uv_os_getenv("LYRA_HTTP_REACTORS", setting, &size);
    if (status == UV_ENOENT) {
#if UV_VERSION_HEX >= 0x012c00
        desired = std::clamp(uv_available_parallelism(), 1u, 4u);
#else
        uv_cpu_info_t* info;
        int count;
        if (uv_cpu_info(&info, &count)) desired = 1;
        else { desired = std::clamp(count, 1, 4); uv_free_cpu_info(info, count); }
#endif
    } else {
        if (status) return EINVAL;
        auto [end, error] = std::from_chars(setting, setting + size, desired);
        if (error != std::errc{} || end != setting + size || !desired || desired > shards.size()) return EINVAL;
    }
    for (std::size_t index = 0; index < desired; ++index) {
        auto& shard = shards[index];
        shard.pool = this;
        shard.index = index;
        if (auto status = shard.initialize()) {
            for (std::size_t started = 0; started < index; ++started) shards[started].call<&stopReactor>(shards[started]);
            for (std::size_t started = 0; started < index; ++started) uv_thread_join(&shards[started].thread);
            return statusCode(status);
        }
    }
    count = desired;
    return 0;
}

Reactor& Pool::coordinator() {
    std::lock_guard lock(mutex);
    return shards[nextListener++ % count];
}

Reactor& Pool::place() {
    std::lock_guard lock(mutex);
    auto winner = nextConnection % count;
    for (std::size_t offset = 1; offset < count; ++offset) {
        auto candidate = (nextConnection + offset) % count;
        if (loads[candidate] < loads[winner]) winner = candidate;
    }
    nextConnection = (winner + 1) % count;
    ++loads[winner];
    return shards[winner];
}

void Pool::release(Reactor& reactor) {
    std::lock_guard lock(mutex);
    --loads[reactor.index];
}

void Pool::listenerAdded() {
    std::lock_guard lock(mutex);
    ++activeListeners;
}

void Pool::listenerRemoved() {
    std::lock_guard lock(mutex);
    if (--activeListeners == 0) changed.notify_one();
}

bool temporaryExhaustion(int status) { return status == EMFILE || status == ENFILE || status == ENOMEM || status == ENOBUFS; }

int Server::start() {
    auto& reactor = *endpoint.reactor;
    if (auto status = uv_poll_init_socket(&reactor.loop, &poll, socket)) {
        net::close(socket);
        if (endpoint.transport) endpoint.transport->releaseListener(endpoint.tlsContext);
        lyraGcRootDrop(root);
        delete this;
        return statusCode(status);
    }
    require(uv_timer_init(&reactor.loop, &timer) == 0, "cannot initialize listener timer");
    poll.data = timer.data = this;
    endpoint.server = this;
    reactor.listeners.insert(this);
    reactor.pool->listenerAdded();
    if (auto status = pollForAccept()) { stop(Stop::Close, status); return status; }
    return 0;
}

int Server::pollForAccept() {
    if (stopping != Stop::Open) return 0;
    const bool paused = (ordinary >= static_cast<std::size_t>(2 * endpoint.options.capacity) && rejecting) || retryAt != forever;
    if (paused) {
        if (polling) uv_poll_stop(&poll);
        polling = false;
        return 0;
    }
    if (polling) return 0;
    auto status = uv_poll_start(&poll, UV_READABLE, [](uv_poll_t* handle, int status, int events) {
        auto& server = *static_cast<Server*>(handle->data);
        if (server.stopping != Stop::Open || !server.polling) return;
        if (status) server.stop(Stop::Close, statusCode(status));
        else if (events & UV_READABLE) server.acceptReady();
    });
    polling = status == 0;
    return statusCode(status);
}

void Server::acceptReady() {
    auto& pool = *endpoint.reactor->pool;
    for (std::size_t batch = 0; batch < ingressBatch && endpoint.requested.load() == Stop::Open; ++batch) {
        const bool overload = ordinary >= static_cast<std::size_t>(2 * endpoint.options.capacity);
        if (overload && rejecting) break;
        auto accepted = net::accept(socket);
        if (accepted == net::invalidSocket) {
            auto status = net::error();
            if (status == EINTR || status == ECONNABORTED || status == ECONNRESET) continue;
            if (net::retry(status)) break;
            if (temporaryExhaustion(status)) {
                retryMs = retryMs ? std::min<int64_t>(1000, retryMs * 2) : 25;
                retryAt = deadline(retryMs);
                refreshTimer();
                break;
            }
            stop(Stop::Close, status);
            return;
        }
        retryMs = 0;
        auto& reactor = pool.place();
        auto* session = ::new (lyraGcAlloc(sizeof(Session))) Session{
            .endpoint = &endpoint, .reactor = &reactor, .socket = accepted, .overload = overload, .acceptedAt = now(),
        };
        session->request = ::new (lyraGcAlloc(sizeof(Request))) Request{.session = session};
        connections.emplace(session, Slot{lyraGcRootCreate(session), ready.end()});
        if (overload) rejecting = true;
        else ++ordinary;
        reactor.post<[](Session& session) { (new Connection(session))->open(); }>(*session);
    }
    if (endpoint.requested.load() != Stop::Open) applyStop(endpoint);
    else if (auto status = pollForAccept()) stop(Stop::Close, status);
}

void Server::offer(Request& request) {
    if (stopping != Stop::Open || request.delivery.load() != Delivery::Pending) return;
    connections.at(request.session).ready = ready.insert(ready.end(), &request);
    dispatch();
}

void Server::released(Request& request) {
    auto& slot = connections.at(request.session);
    if (slot.ready != ready.end()) ready.erase(slot.ready);
    slot.ready = ready.end();
    if (slot.issued) --issued;
    slot.issued = false;
    dispatch();
}

void Server::closed(Session& session) {
    released(*session.request);
    auto entry = connections.find(&session);
    if (session.overload) rejecting = false;
    else --ordinary;
    endpoint.reactor->pool->release(*session.reactor);
    lyraGcRootDrop(entry->second.root);
    connections.erase(entry);
    if (stopping != Stop::Open) { maybeFinish(); return; }
    if (endpoint.requested.load() != Stop::Open) { applyStop(endpoint); return; }
    if (!session.overload && session.ingressError) reportRejected(session.ingressError);
    dispatch();
    if (auto status = pollForAccept()) stop(Stop::Close, status);
}

void Server::dispatch() {
    if (endpoint.requested.load() != Stop::Open) { applyStop(endpoint); return; }
    expire(now());
    while (!accepts.empty() && !ready.empty() && issued < static_cast<std::size_t>(endpoint.options.capacity)) {
        auto& request = *ready.front();
        auto& slot = connections.at(request.session);
        auto& operation = *accepts.front();
        const auto delivery = operation.scoped ? Delivery::Claimed : Delivery::Offered;
        auto pending = Delivery::Pending;
        if (!request.delivery.compare_exchange_strong(pending, delivery)) {
            ready.pop_front();
            slot.ready = ready.end();
            continue;
        }
        accepts.pop_front();
        auto* cleanup = operation.scoped ? requestCloseTask(request) : nullptr;
        if (!lyraNativeTryComplete(operation.task, box(&request), cleanup)) {
            auto previous = delivery;
            request.delivery.compare_exchange_strong(previous, Delivery::Pending);
            finish(operation, ECANCELED);
            continue;
        }
        operation.task = nullptr;
        ready.pop_front();
        slot.ready = ready.end();
        slot.issued = true;
        ++issued;
        if (!operation.scoped) {
            request.offerExpires = deadline(endpoint.options.claimTimeoutMs);
            request.session->reactor->post<[](Request& request) {
                auto* connection = request.connection;
                if (connection && connection->phase == Phase::Ready && request.delivery.load() == Delivery::Offered) {
                    connection->phaseDue = request.offerExpires;
                    connection->refreshTimer();
                }
            }>(request);
        }
    }
    refreshTimer();
}

void Server::registerAccept(Operation& operation) {
    if (endpoint.requested.load() != Stop::Open || stopping != Stop::Open) { finish(operation, EBADF); return; }
    if (!validWait(operation.argument)) { finish(operation, EINVAL); return; }
    if (accepts.size() >= static_cast<std::size_t>(endpoint.options.capacity)) { finish(operation, EBUSY); return; }
    if (!operation.argument) operation.expires = forever;
    accepts.push_back(&operation);
    dispatch();
    if (!operation.argument && operation.task) {
        accepts.remove(&operation);
        finish(operation, ETIMEDOUT);
    }
}

void Server::expire(int64_t time) {
    for (auto iterator = accepts.begin(); iterator != accepts.end();) {
        auto& operation = **iterator;
        if (operation.expires > time) { ++iterator; continue; }
        iterator = accepts.erase(iterator);
        finish(operation, ETIMEDOUT);
    }
}

void Server::refreshTimer() {
    if (stopping != Stop::Open) return;
    auto due = retryAt;
    for (auto* operation : accepts) due = std::min(due, operation->expires);
    arm(timer, [](uv_timer_t* timer) {
        auto& server = *static_cast<Server*>(timer->data);
        if (server.retryAt <= now()) {
            server.retryAt = forever;
            if (auto status = server.pollForAccept()) { server.stop(Stop::Close, status); return; }
        }
        server.dispatch();
    }, due);
}

void Server::reportRejected(int status) {
    expire(now());
    auto waiter = std::ranges::find_if(accepts, [](const Operation* operation) { return operation->reportRejections; });
    if (waiter == accepts.end()) return;
    auto* operation = *waiter;
    accepts.erase(waiter);
    finish(*operation, status);
}

void Server::stop(Stop mode, int error) {
    if (mode <= stopping) return;
    if (mode == Stop::Close) endpoint.requested.store(Stop::Close);
    if (stopping == Stop::Open) {
        uv_timer_stop(&timer);
        uv_poll_stop(&poll);
        polling = false;
        uv_close(reinterpret_cast<uv_handle_t*>(&poll), [](uv_handle_t* handle) {
            auto& server = *static_cast<Server*>(handle->data);
            net::close(server.socket);
            server.listenerClosed = true;
            server.maybeFinish();
        });
        for (auto* operation : accepts) finish(*operation, error);
        accepts.clear();
    }
    stopping = mode;
    for (const auto& entry : connections) {
        entry.first->reactor->post<[](Session& session) {
            if (auto* connection = session.connection) {
                auto delivery = connection->request->delivery.load();
                if (session.endpoint->requested.load() == Stop::Close || delivery != Delivery::Claimed)
                    connection->beginClose(EBADF);
            }
        }>(*entry.first);
    }
    maybeFinish();
}

void Server::maybeFinish() {
    if (!listenerClosed || !connections.empty() || finishing) return;
    finishing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(&timer), [](uv_handle_t* handle) {
        auto* server = static_cast<Server*>(handle->data);
        auto& endpoint = server->endpoint;
        auto& reactor = *endpoint.reactor;
        if (endpoint.transport) endpoint.transport->releaseListener(endpoint.tlsContext);
        endpoint.server = nullptr;
        auto pendingCloses = std::move(server->closeWaiters);
        auto* root = server->root;
        reactor.listeners.erase(server);
        delete server;
        endpoint.stopped.store(true);
        for (auto* operation : pendingCloses) finish(*operation);
        lyraGcRootDrop(root);
        reactor.pool->listenerRemoved();
    });
}

void Operation::start() {
    switch (kind) {
        case OperationKind::Accept:
            if (auto* server = endpoint->server) server->registerAccept(*this);
            else finish(*this, EBADF);
            break;
        case OperationKind::Respond:
            if (auto* connection = request->connection) connection->startResponse(*this);
            else finish(*this, EBADF);
            break;
        case OperationKind::Observe:
            if (auto* connection = request->connection) connection->observe(*this);
            else finish(*this, validWait(argument) ? request->peerLost.load() : -EINVAL);
            break;
        case OperationKind::CloseRequest:
            if (auto* connection = request->connection) {
                connection->closeWaiters.push_back(this);
                connection->beginClose(EBADF);
            } else finish(*this);
            break;
        case OperationKind::CloseListener:
        case OperationKind::ListenerScope:
            if (auto* server = endpoint->server) {
                server->closeWaiters.push_back(this);
                if (kind == OperationKind::CloseListener) server->stop(Stop::Close);
            } else finish(*this);
            break;
    }
}

void Operation::cancel() {
    if (!task) return;
    switch (kind) {
        case OperationKind::Accept: {
            auto& server = *endpoint->server;
            server.accepts.remove(this);
            server.refreshTimer();
            finish(*this, ECANCELED);
            break;
        }
        case OperationKind::Respond: request->connection->beginClose(ECANCELED); break;
        case OperationKind::Observe: {
            auto& connection = *request->connection;
            if (connection.phase == Phase::Closing) break;
            connection.observers.remove(this);
            connection.refreshTimer();
            finish(*this, -ECANCELED);
            break;
        }
        case OperationKind::ListenerScope: endpoint->server->stop(Stop::Close); break;
        default: break;
    }
}

bool validOptions(const ServerOptions& options) {
    auto positive = [](int64_t value) { return value >= 1 && value <= INT_MAX; };
    return options.capacity >= 1 && options.capacity <= 64 && positive(options.requestTimeoutMs) &&
        (options.idleReadTimeoutMs == -1 || positive(options.idleReadTimeoutMs)) && positive(options.writeTimeoutMs) &&
        positive(options.handshakeTimeoutMs) && options.graceMs >= 0 && options.graceMs <= INT_MAX && positive(options.claimTimeoutMs) &&
        positive(options.maxBodyBytes) && positive(options.maxRequestBytes) && options.maxRequestBytes >= options.maxBodyBytes &&
        options.keepAliveTimeoutMs >= 0 && options.keepAliveTimeoutMs <= INT_MAX && positive(options.maxRequestsPerConnection);
}

}

}

using namespace lyraRuntime::http;
namespace net = lyraRuntime::net;

extern "C" const ServerOptions* lyraHttpOptions(int64_t capacity, int64_t requestTimeoutMs, int64_t idleReadTimeoutMs,
    int64_t writeTimeoutMs, int64_t handshakeTimeoutMs, int64_t graceMs, int64_t claimTimeoutMs,
    int64_t maxBodyBytes, int64_t maxRequestBytes, int64_t keepAliveTimeoutMs, int64_t maxRequestsPerConnection) {
    return ::new (lyraGcAllocAtomic(sizeof(ServerOptions))) ServerOptions{
        capacity, requestTimeoutMs, idleReadTimeoutMs, writeTimeoutMs, handshakeTimeoutMs, graceMs, claimTimeoutMs,
        maxBodyBytes, maxRequestBytes, keepAliveTimeoutMs, maxRequestsPerConnection,
    };
}

extern "C" void* lyraHttpListenFailure(int64_t status, const void* detail) {
    return ::new (lyraGcAlloc(sizeof(Endpoint))) Endpoint{.status = static_cast<int>(status), .detail = detail};
}

extern "C" void* lyraHttpListenWithTransport(const void* host, int64_t port, const ServerOptions* options,
    const Transport* transport, void* state) {
    auto fail = [&](int status, std::string_view detail = {}) {
        if (transport) transport->releaseListener(state);
        return lyraHttpListenFailure(status, detail.empty() ? nullptr : lyraStringFromBytes(detail.data(), detail.size()));
    };
    if (!validOptions(*options)) return fail(EINVAL, "invalid HTTP server options");
    auto hostText = text(host);
    if (port < 0 || port > UINT16_MAX || hostText.empty() || hostText.find('\0') != hostText.npos) return fail(EINVAL);
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
        ::bind(descriptor, address, length) || ::listen(descriptor, SOMAXCONN) || ::getsockname(descriptor, address, &length)) {
        auto status = net::error();
        net::close(descriptor);
        return fail(status);
    }
    auto bound = ntohs(family == AF_INET ? address4.sin_port : address6.sin6_port);
    auto* endpoint = ::new (lyraGcAlloc(sizeof(Endpoint))) Endpoint{
        .options = *options, .transport = transport, .tlsContext = state, .port = bound,
    };
    auto& pool = processPool();
    if (transport) {
        static const auto lateShutdown = std::atexit([] { processPool().stop(); });
        if (lateShutdown != 0) { net::close(descriptor); return fail(ENOMEM, "cannot register TLS reactor shutdown"); }
    }
    if (auto status = pool.start()) {
        net::close(descriptor);
        return fail(status, status == EINVAL ? "LYRA_HTTP_REACTORS must be an integer from 1 to 64" : "cannot start HTTP reactor pool");
    }
    endpoint->reactor = &pool.coordinator();
    struct Setup { Endpoint& endpoint; net::Socket socket; int status{}; } setup{*endpoint, descriptor};
    endpoint->reactor->call<[](Setup& setup) { setup.status = (new Server(setup.endpoint, setup.socket))->start(); }>(setup);
    endpoint->status = setup.status;
    return endpoint;
}

extern "C" void* lyraHttpListen(const void* host, int64_t port, const ServerOptions* options) {
    return lyraHttpListenWithTransport(host, port, options, nullptr, nullptr);
}
extern "C" int64_t lyraHttpPort(const Endpoint* endpoint) { return endpoint->port; }
extern "C" int64_t lyraHttpStatus(const Endpoint* endpoint) { return endpoint->status; }
extern "C" const void* lyraHttpStatusDetail(const Endpoint* endpoint) {
    return endpoint->detail ? endpoint->detail : lyraStringFromBytes(nullptr, 0);
}
extern "C" int64_t lyraHttpCapacity(const Endpoint* endpoint) { return endpoint->options.capacity; }
extern "C" int64_t lyraHttpGraceMs(const Endpoint* endpoint) { return endpoint->options.graceMs; }
extern "C" void lyraHttpBeginDrain(Endpoint* endpoint) { requestStop(*endpoint, Stop::Drain); }
extern "C" void lyraHttpStop(Endpoint* endpoint) { requestStop(*endpoint, Stop::Close); }
extern "C" void lyraHttpDrop(Request* request) { closeRequest(*request, true); }
extern "C" int64_t lyraHttpClaim(Request* request) { return claim(*request); }
extern "C" uint8_t lyraHttpDisconnected(const Request* request) { return request->peerLost.load(); }

extern "C" LyraTask* lyraHttpAcceptAsync(Endpoint* endpoint, int64_t timeoutMs, uint8_t scoped, uint8_t reportRejections) {
    return nativeTask({.kind = OperationKind::Accept, .reactor = endpoint->reactor, .endpoint = endpoint, .argument = timeoutMs,
        .scoped = scoped != 0, .reportRejections = reportRejections != 0});
}

extern "C" LyraTask* lyraHttpRespondAsync(Request* request, int64_t code, const void* mediaType, const void* body, const ResponseFields* headers) {
    return nativeTask({OperationKind::Respond, request->session->reactor, request->session->endpoint, request, code, mediaType, body, headers});
}

extern "C" LyraTask* lyraHttpWaitDisconnectAsync(Request* request, int64_t timeoutMs) {
    return nativeTask({OperationKind::Observe, request->session->reactor, request->session->endpoint, request, timeoutMs});
}

extern "C" LyraTask* lyraHttpRequestCloseAsync(Request* request) { return requestCloseTask(*request); }

extern "C" LyraTask* lyraHttpListenerCloseAsync(Endpoint* endpoint) {
    return nativeTask({OperationKind::CloseListener, endpoint->reactor, endpoint});
}

extern "C" LyraTask* lyraHttpListenerScopeAsync(Endpoint* endpoint) {
    return nativeTask({OperationKind::ListenerScope, endpoint->reactor, endpoint});
}

extern "C" void lyraHttpKeepListener(Endpoint* endpoint) { lyraNativeKeepCleanup(lyraHttpListenerCloseAsync(endpoint)); }

extern "C" int64_t lyraHttpErrorKind(int64_t status) {
    switch (status) {
        case EINVAL: return 1;
        case EBADF: return 2;
        case ETIMEDOUT: return 3;
        case EPROTO: return 4;
        case EMSGSIZE: return 5;
        case EILSEQ: return 6;
        case tlsConfig: return 7;
        case handshakeFailed: return 8;
        case handshakeTimeout: return 9;
        case EBUSY: return 10;
        case unsupportedExpectation: return 11;
        default: return 0;
    }
}
