#pragma once

#include <cstdint>
#include <uv.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace lyraRuntime::net {

using Socket = uv_os_sock_t;
#ifdef _WIN32
using AddressLength = int;
inline constexpr auto invalidSocket = INVALID_SOCKET;
#else
using AddressLength = socklen_t;
inline constexpr auto invalidSocket = -1;
#endif

int error();
bool retry(int status);
Socket open(int family);
Socket accept(Socket socket);
void close(Socket socket);

}
