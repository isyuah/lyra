#pragma once

#include <cstdint>
#include <uv.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <poll.h>
#endif

namespace lyraRuntime::net {

using Socket = uv_os_sock_t;
#ifdef _WIN32
using AddressLength = int;
using PollSlot = WSAPOLLFD;
inline constexpr auto invalidSocket = INVALID_SOCKET;
#else
using AddressLength = socklen_t;
using PollSlot = pollfd;
inline constexpr auto invalidSocket = -1;
#endif

int error();
bool retry(int status);
Socket open();
Socket accept(Socket socket);
void close(Socket socket);
int poll(PollSlot& slot, int timeout);
int receive(Socket socket, char* data, int size, int flags = 0);
int send(Socket socket, const char* data, int size);
uintptr_t disconnectMonitor(Socket socket);
void closeMonitor(uintptr_t monitor);
bool disconnected(Socket socket);
bool waitDisconnect(uintptr_t monitor, int timeout);

}
