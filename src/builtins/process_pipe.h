#ifndef SKEIN_PROCESS_PIPE_H
#define SKEIN_PROCESS_PIPE_H

#include <cstddef>
#include <string>

struct value_t;

/*
 * Byte-level pipe over a child process's stdin/stdout.
 *
 * This is the transport base used by std.worker.  It owns the process handle
 * and the two stdio streams, buffers bytes read past a line, and exposes
 * write/flush/read_line/read_exact/close without knowing anything about the
 * RPC framing above it.
 */
struct process_pipe {
    value_t *process = nullptr;
    value_t *stdin_stream = nullptr;
    value_t *stdout_stream = nullptr;
    std::string pending;

    process_pipe() = default;
    ~process_pipe();

    process_pipe(const process_pipe &) = delete;
    process_pipe &operator=(const process_pipe &) = delete;
    process_pipe(process_pipe &&other) noexcept;
    process_pipe &operator=(process_pipe &&other) noexcept;

    bool spawn(const char *executable, struct value_t *options,
               std::string &error);
    bool usable() const;
    bool write(const std::string &data);
    bool flush();
    struct value_t *read_line(long long timeout_ms, std::string &error);
    struct value_t *read_exact(std::size_t size, long long timeout_ms,
                               std::string &error);
    void close(bool kill_if_running, bool force = false);
};

#endif
