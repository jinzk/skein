#ifndef SKEIN_PROCESS_IO_H
#define SKEIN_PROCESS_IO_H

#include <cstddef>
#include <string>

struct value_t;

/*
 * Byte-level I/O adapter over an existing child process's stdin/stdout.
 *
 * The process and its underlying stdio pipes are owned by std.process
 * (process_resource).  This adapter only borrows the already-bound stream
 * handles, buffers bytes read past a line, and exposes
 * write/flush/read_line/read_exact.  It does not spawn, wait for, or kill the
 * process.
 */
struct process_io {
    value_t *stdin_stream = nullptr;
    value_t *stdout_stream = nullptr;
    std::string pending;

    process_io() = default;
    ~process_io() = default;

    process_io(const process_io &) = delete;
    process_io &operator=(const process_io &) = delete;
    process_io(process_io &&other) noexcept;
    process_io &operator=(process_io &&other) noexcept;

    /* Attach to an existing std.process.spawn_command result. */
    bool attach(value_t *process, std::string &error);
    bool usable() const;
    bool write(const std::string &data);
    bool flush();
    value_t *read_line(long long timeout_ms, std::string &error);
    value_t *read_exact(std::size_t size, long long timeout_ms,
                        std::string &error);
    /* Close the stdin side and drop borrows.  Process lifetime stays with
     * std.process. */
    void close_streams();
};

#endif
