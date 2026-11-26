#include "net.h"

#include <cerrno>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace lyraRuntime::net {

int error() {
#ifdef _WIN32
    auto status = WSAGetLastError();
    switch (status) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINTR: return EINTR;
        case WSAECONNRESET: return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEACCES: return EACCES;
        case WSAEMFILE: return EMFILE;
        case WSAENOBUFS: return ENOBUFS;
        default: return status;
    }
#else
    return errno;
#endif
}

bool retry(int status) { return status == EINTR || status == EAGAIN || status == EWOULDBLOCK; }

Socket open(int family) {
#ifdef _WIN32
    static const auto startup = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (startup) { WSASetLastError(startup); return invalidSocket; }
    auto socket = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == invalidSocket) return socket;
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == 0) return socket;
    auto status = WSAGetLastError();
    closesocket(socket);
    WSASetLastError(status);
    return invalidSocket;
#else
    return ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#endif
}

Socket accept(Socket socket) {
#ifdef _WIN32
    auto accepted = ::accept(socket, nullptr, nullptr);
    if (accepted == invalidSocket) return accepted;
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0)) {
        auto status = GetLastError();
        closesocket(accepted);
        WSASetLastError(static_cast<int>(status));
        return invalidSocket;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(accepted, FIONBIO, &nonblocking) == 0) return accepted;
    auto status = WSAGetLastError();
    closesocket(accepted);
    WSASetLastError(status);
    return invalidSocket;
#else
    return ::accept4(socket, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
}

void close(Socket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

}
