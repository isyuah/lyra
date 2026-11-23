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
        default: return status;
    }
#else
    return errno;
#endif
}

bool retry(int status) { return status == EINTR || status == EAGAIN || status == EWOULDBLOCK; }

Socket open() {
#ifdef _WIN32
    static const auto startup = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (startup) { WSASetLastError(startup); return invalidSocket; }
    auto socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == invalidSocket) return socket;
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == 0) return socket;
    auto status = WSAGetLastError();
    closesocket(socket);
    WSASetLastError(status);
    return invalidSocket;
#else
    return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#endif
}

Socket accept(Socket socket) {
#ifdef _WIN32
    auto accepted = ::accept(socket, nullptr, nullptr);
    if (accepted != invalidSocket && !SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0)) {
        auto status = GetLastError();
        closesocket(accepted);
        WSASetLastError(static_cast<int>(status));
        return invalidSocket;
    }
    return accepted;
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

int poll(PollSlot& slot, int timeout) {
#ifdef _WIN32
    return WSAPoll(&slot, 1, timeout);
#else
    return ::poll(&slot, 1, timeout);
#endif
}

int receive(Socket socket, char* data, int size, int flags) {
    return static_cast<int>(::recv(socket, data, size, flags));
}

int send(Socket socket, const char* data, int size) {
#ifdef _WIN32
    return ::send(socket, data, size, 0);
#else
    return static_cast<int>(::send(socket, data, size, MSG_NOSIGNAL));
#endif
}

uintptr_t disconnectMonitor(Socket socket) {
#ifdef _WIN32
    auto event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) return 0;
    if (WSAEventSelect(socket, event, FD_CLOSE) == 0) return reinterpret_cast<uintptr_t>(event);
    auto status = WSAGetLastError();
    WSACloseEvent(event);
    WSASetLastError(status);
    return 0;
#else
    return static_cast<uintptr_t>(socket) + 1;
#endif
}

void closeMonitor(uintptr_t monitor) {
#ifdef _WIN32
    if (monitor) WSACloseEvent(reinterpret_cast<HANDLE>(monitor));
#else
    (void)monitor;
#endif
}

bool disconnected(Socket socket) {
    char next;
    for (;;) {
        auto count = receive(socket, &next, 1, MSG_PEEK);
        if (count >= 0) return count == 0;
        auto status = error();
        if (status != EINTR) return !retry(status);
    }
}

bool waitDisconnect(uintptr_t monitor, int timeout) {
#ifdef _WIN32
    return WaitForSingleObject(reinterpret_cast<HANDLE>(monitor), timeout) == WAIT_OBJECT_0;
#else
    auto deadline = uv_hrtime() / 1000000 + static_cast<unsigned>(timeout);
    PollSlot slot{static_cast<Socket>(monitor - 1), POLLRDHUP, 0};
    for (;;) {
        auto now = uv_hrtime() / 1000000;
        if (now > deadline) return false;
        auto result = poll(slot, static_cast<int>(deadline - now));
        if (result < 0 && error() == EINTR) continue;
        return result > 0 && (slot.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL));
    }
#endif
}

}
