#pragma once

#include "net.h"

namespace lyraRuntime::http {

inline constexpr int tlsConfig = 1000001;
inline constexpr int handshakeFailed = 1000002;
inline constexpr int handshakeTimeout = 1000003;

struct ServerOptions {
    int64_t capacity, requestTimeoutMs, idleReadTimeoutMs, writeTimeoutMs, handshakeTimeoutMs, graceMs, claimTimeoutMs;
};

enum class TlsProgress { Done, WantInput, WantOutput, End, Failed };
struct TlsStep { TlsProgress progress; std::size_t bytes{}; };

struct Transport {
    void* (*open)(void* listener);
    std::size_t (*feed)(void* connection, const char* data, std::size_t size);
    std::size_t (*flush)(void* connection, char* data, std::size_t size);
    void (*endInput)(void* connection);
    TlsStep (*handshake)(void* connection);
    TlsStep (*read)(void* connection, char* data, std::size_t size);
    TlsStep (*write)(void* connection, const char* data, std::size_t size);
    TlsStep (*shutdown)(void* connection);
    void (*releaseConnection)(void* connection);
    void (*releaseListener)(void* listener);
};

}

extern "C" void* lyraHttpListenWithTransport(const void* host, int64_t port, const lyraRuntime::http::ServerOptions* options,
    const lyraRuntime::http::Transport* transport, void* state);
extern "C" void* lyraHttpListenFailure(int64_t status, const void* detail);
