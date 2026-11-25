#pragma once

#include "net.h"

namespace lyraRuntime::http {

inline constexpr int tlsConfig = 1000001;
inline constexpr int handshakeFailed = 1000002;
inline constexpr int handshakeTimeout = 1000003;

struct Transport {
    int (*open)(void* listener, net::Socket socket, void** connection);
    int (*handshake)(void* connection, int64_t deadline);
    int (*read)(void* connection, char* data, int size, int* count, int64_t deadline);
    int (*write)(void* connection, const char* data, int size, int* count, int64_t deadline);
    void (*shutdown)(void* connection, int64_t deadline);
    void (*releaseConnection)(void* connection);
    void (*releaseListener)(void* listener);
};

int64_t now();
int waitFor(net::Socket socket, short event, int64_t deadline);

}

extern "C" void* lyraHttpListenWithTransport(const void* host, int64_t port,
    const lyraRuntime::http::Transport* transport, void* state, int64_t handshakeTimeoutMs);
extern "C" void* lyraHttpListenFailure(int64_t status, const void* detail);
