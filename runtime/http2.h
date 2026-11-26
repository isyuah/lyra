#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace lyraRuntime::http {

struct Request;
struct ResponseFields;
struct ServerOptions;

namespace h2 {

enum class ReadState { Pending, Bytes, End, Error };

struct BodyRead {
    ReadState state{ReadState::Pending};
    const void* bytes{};
    int status{};
};

struct Hooks {
    Request* (*create)(void*, int32_t streamId);
    void (*ready)(void*, Request&);
    void (*changed)(void*, Request&);
    void (*closed)(void*, Request&, int status, bool peer);
};

class Session {
public:
    Session(const ServerOptions&, Hooks, void* user);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    int status() const;
    int feed(std::string_view bytes);
    std::string_view output();
    void consumeOutput(std::size_t bytes);
    bool wantsRead() const;
    bool finished() const;
    std::size_t activeStreams() const;
    bool inputComplete(Request&) const;
    void drain();
    void abort(int status);

    BodyRead read(Request&);
    int respond(Request&, int64_t status, const void* mediaType, const void* body, const ResponseFields&, bool streaming);
    int write(Request&, const void* bytes, bool final);
    std::optional<int> takeWriteCompletion(Request&);
    void reset(Request&, int status, bool peer = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

}
