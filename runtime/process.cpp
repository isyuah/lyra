#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <uv.h>
#include <vector>

namespace {

enum Status { invalid = -1, timedOut = -2, outputLimit = -3, cancelled = -4 };

struct ProcessResult {
    std::int64_t status{}, exitCode{}, signal{};
    const void* output{};
    const void* errors{};
};

struct Process;

struct Capture {
    uv_pipe_t pipe{};
    Process* owner{};
    bool opened{};
    std::array<char, 8192> input;
    std::vector<char> bytes;
};

struct Process {
    uv_loop_t loop{};
    uv_timer_t timer{};
    uv_process_t child{};
    Capture output, errors;
    ProcessResult result;
    std::size_t bytesLeft;
    std::uint64_t deadline{};
    bool running{}, timerOpen{};
};

template <class Handle>
void close(Handle& handle) { uv_close(reinterpret_cast<uv_handle_t*>(&handle), nullptr); }

void closeCapture(Capture& capture) {
    if (!capture.opened) return;
    capture.opened = false;
    close(capture.pipe);
}

void finishWhenDrained(Process& process) {
    if (process.timerOpen && !process.running && !process.output.opened && !process.errors.opened) {
        process.timerOpen = false;
        close(process.timer);
    }
}

void fail(Process& process, int status) {
    if (process.result.status) return;
    process.result.status = status;
    closeCapture(process.output);
    closeCapture(process.errors);
    if (process.running) {
        auto stopped = uv_process_kill(&process.child, SIGKILL);
        if (stopped != 0 && stopped != UV_ESRCH) process.result.status = -stopped;
    }
    finishWhenDrained(process);
}

void drain(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    auto& capture = *static_cast<Capture*>(stream->data);
    auto& process = *capture.owner;
    if (count == UV_EOF) {
        closeCapture(capture);
        finishWhenDrained(process);
    } else if (count < 0) {
        fail(process, -count);
    } else if (static_cast<std::size_t>(count) > process.bytesLeft) {
        fail(process, outputLimit);
    } else if (count != 0) {
        process.bytesLeft -= static_cast<std::size_t>(count);
        capture.bytes.insert(capture.bytes.end(), buffer->base, buffer->base + count);
    }
}

int openCapture(Capture& capture, Process& owner) {
    auto status = uv_pipe_init(&owner.loop, &capture.pipe, 0);
    if (status == 0) {
        capture.pipe.data = &capture;
        capture.owner = &owner;
        capture.opened = true;
    }
    return status;
}

int startCapture(Capture& capture) {
    return uv_read_start(reinterpret_cast<uv_stream_t*>(&capture.pipe), [](auto* handle, auto, auto* buffer) {
        auto& input = static_cast<Capture*>(handle->data)->input;
        *buffer = uv_buf_init(input.data(), input.size());
    }, drain);
}

void tick(uv_timer_t* timer) {
    auto& process = *static_cast<Process*>(timer->data);
    if (lyraBlockingCancelled()) fail(process, cancelled);
    else if (uv_now(&process.loop) >= process.deadline) fail(process, timedOut);
}

void childExited(uv_process_t* child, std::int64_t code, int signal) {
    auto& process = *static_cast<Process*>(child->data);
    process.running = false;
    process.result.exitCode = code;
    process.result.signal = signal;
    close(*child);
    finishWhenDrained(process);
}

void run(Process& process, char** argv, const char* cwd, std::uint64_t timeout) {
    auto status = uv_loop_init(&process.loop);
    if (status != 0) { process.result.status = -status; return; }
    process.deadline = uv_now(&process.loop) + timeout;
    status = openCapture(process.output, process);
    if (!status) status = openCapture(process.errors, process);
    if (!status) {
        status = uv_timer_init(&process.loop, &process.timer);
        process.timerOpen = status == 0;
        process.timer.data = &process;
    }
    if (!status) {
        uv_stdio_container_t streams[3]{};
        streams[1].flags = streams[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
        streams[1].data.stream = reinterpret_cast<uv_stream_t*>(&process.output.pipe);
        streams[2].data.stream = reinterpret_cast<uv_stream_t*>(&process.errors.pipe);
        uv_process_options_t options{};
        options.file = argv[0];
        options.args = argv;
        options.cwd = cwd;
        options.stdio = streams;
        options.stdio_count = 3;
        options.exit_cb = childExited;
#ifdef _WIN32
        options.flags = UV_PROCESS_WINDOWS_HIDE;
#endif
        status = uv_spawn(&process.loop, &process.child, &options);
        process.child.data = &process;
        process.running = uv_is_active(reinterpret_cast<uv_handle_t*>(&process.child)) != 0;
        if (status && !process.running && process.child.type == UV_PROCESS && process.child.loop == &process.loop)
            close(process.child);
    }
    if (!status) status = startCapture(process.output);
    if (!status) status = startCapture(process.errors);
    if (!status) {
        uv_update_time(&process.loop);
        auto now = uv_now(&process.loop);
        auto remaining = process.deadline > now ? process.deadline - now : 0;
        status = uv_timer_start(&process.timer, tick, std::min<std::uint64_t>(remaining, 20), 20);
    }
    if (status) fail(process, -status);
    uv_run(&process.loop, UV_RUN_DEFAULT);
    if (uv_loop_close(&process.loop) != 0) std::abort();
}

const ProcessResult* save(ProcessResult result) {
    auto* stored = static_cast<ProcessResult*>(lyraGcAlloc(sizeof(ProcessResult)));
    *stored = result;
    return stored;
}

}

extern "C" const void** lyraProcessArgs(std::int64_t count) {
    return static_cast<const void**>(lyraGcAlloc(static_cast<std::size_t>(count) * sizeof(void*)));
}

extern "C" void lyraProcessArg(const void** args, std::int64_t index, const void* value) { args[index] = value; }

extern "C" const ProcessResult* lyraProcessRun(const void* const* args, std::int64_t count, const void* cwdValue,
                                                 std::int64_t timeout, std::int64_t maxBytes) {
    if (lyraBlockingCancelled()) return save({cancelled});
    std::vector<std::string> strings;
    strings.reserve(count);
    for (std::int64_t index = 0; index < count; ++index) {
        auto& value = strings.emplace_back(lyraStringBytes(args[index]), lyraStringLength(args[index]));
        if (value.find('\0') != value.npos) return save({invalid});
    }
    if (strings.front().empty()) return save({invalid});
    std::string cwd(lyraStringBytes(cwdValue), lyraStringLength(cwdValue));
    if (cwd.find('\0') != cwd.npos) return save({invalid});
    std::vector<char*> argv;
    argv.reserve(strings.size() + 1);
    for (auto& value : strings) argv.push_back(value.data());
    argv.push_back(nullptr);
    Process process{};
    process.bytesLeft = maxBytes;
    run(process, argv.data(), cwd.empty() ? nullptr : cwd.c_str(), timeout);
    if (process.result.status == 0) {
        process.result.output = lyraStringFromBytes(process.output.bytes.data(), process.output.bytes.size());
        process.result.errors = lyraStringFromBytes(process.errors.bytes.data(), process.errors.bytes.size());
    }
    return save(process.result);
}

extern "C" std::int64_t lyraProcessStatus(const ProcessResult* result) { return result->status; }
extern "C" std::int64_t lyraProcessExitCode(const ProcessResult* result) { return result->exitCode; }
extern "C" std::int64_t lyraProcessSignal(const ProcessResult* result) { return result->signal; }
extern "C" const void* lyraProcessStdout(const ProcessResult* result) { return result->output; }
extern "C" const void* lyraProcessStderr(const ProcessResult* result) { return result->errors; }
