#include "http_transport.h"
#include "strings.h"
#include "tasks.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace http = lyraRuntime::http;
namespace net = lyraRuntime::net;

namespace lyraRuntime::http { struct Endpoint; }

extern "C" int64_t lyraHttpStatus(const http::Endpoint* endpoint);
extern "C" int64_t lyraHttpPort(const http::Endpoint* endpoint);
extern "C" LyraTask* lyraHttpListenerCloseAsync(http::Endpoint* endpoint);

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "HTTP reactor test: %s\n", message);
    std::abort();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::thread::id, 2> owners;
    unsigned opened{}, released{};
    bool proceed{}, listenerReleased{};

    void waitFor(unsigned count) {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return opened >= count; }),
            "a second loop could not adopt a socket while the first loop held a connection");
    }
};

constexpr http::Transport transport{
    [](void* data) -> void* {
        auto& gate = *static_cast<Gate*>(data);
        std::unique_lock lock(gate.mutex);
        auto index = gate.opened++;
        gate.owners.at(index) = std::this_thread::get_id();
        gate.changed.notify_all();
        if (!index) gate.changed.wait(lock, [&] { return gate.proceed; });
        return data;
    },
    [](void*, const char*, std::size_t count) -> std::size_t { return count; },
    [](void*, char*, std::size_t) -> std::size_t { return 0; },
    [](void*) {},
    [](void*) -> http::TlsStep { return {http::TlsProgress::WantInput}; },
    [](void*, char*, std::size_t) -> http::TlsStep { return {http::TlsProgress::WantInput}; },
    [](void*, const char*, std::size_t) -> http::TlsStep { return {http::TlsProgress::Failed}; },
    [](void*) -> http::TlsStep { return {http::TlsProgress::Done}; },
    [](void* data) {
        auto& gate = *static_cast<Gate*>(data);
        std::lock_guard lock(gate.mutex);
        ++gate.released;
    },
    [](void* data) {
        auto& gate = *static_cast<Gate*>(data);
        std::lock_guard lock(gate.mutex);
        check(gate.opened == gate.released, "listener released its context while a connection could still use it");
        gate.listenerReleased = true;
    },
};

net::Socket connectTo(int64_t port) {
    auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(socket != net::invalidSocket, "cannot create a test client");
    sockaddr_in address;
    check(uv_ip4_addr("127.0.0.1", static_cast<int>(port), &address) == 0, "cannot create loopback address");
    check(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "cannot connect to test listener");
    return socket;
}

}

int main() {
    auto* host = lyraStringFromBytes("127.0.0.1", 9);
    const http::ServerOptions options{2, 5000, -1, 5000, 15000, 0, 5000};
    check(uv_os_setenv("LYRA_HTTP_REACTORS", "65") == 0, "cannot configure reactor count");
    auto* invalid = static_cast<http::Endpoint*>(lyraHttpListenWithTransport(host, 0, &options, nullptr, nullptr));
    check(lyraHttpStatus(invalid) != 0, "listen accepted an invalid reactor count");
    check(uv_os_setenv("LYRA_HTTP_REACTORS", "2") == 0, "cannot configure reactor count");

    Gate gate;
    auto* listener = static_cast<http::Endpoint*>(lyraHttpListenWithTransport(host, 0, &options, &transport, &gate));
    check(lyraHttpStatus(listener) == 0, "cannot start test listener");
    auto first = connectTo(lyraHttpPort(listener));
    gate.waitFor(1);
    auto second = connectTo(lyraHttpPort(listener));
    gate.waitFor(2);
    {
        std::lock_guard lock(gate.mutex);
        check(gate.owners[0] != gate.owners[1], "connections did not advance on separate OS threads");
        gate.proceed = true;
        gate.changed.notify_one();
    }
    lyraGcCollect();
    check(lyraTaskRunRoot(lyraHttpListenerCloseAsync(listener), nullptr) == 0, "listener close failed");
    check(gate.listenerReleased, "listener returned before its context was released");
    net::close(second);
    net::close(first);
}
