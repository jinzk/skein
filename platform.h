#ifndef SKEIN_PLATFORM_H
#define SKEIN_PLATFORM_H

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* One-time process/platform initialization: console encoding and I/O mode. */
void platform_init();

/* Return process arguments as UTF-8 strings.  On Windows this uses the wide
 * command line instead of the narrow argv, which is encoded in the active
 * ANSI code page. */
std::vector<std::string> platform_command_line_args(int argc, char **argv);

/* Read an environment variable as UTF-8.  Returns false when it is unset. */
bool platform_get_env(const std::string &name, std::string &value);

/* True when stdin is an interactive terminal. */
bool platform_stdin_is_tty();

/* Install the Ctrl-C handler used by the REPL. */
void platform_install_interrupt_handler();

/*
 * Run a shell command and capture its standard output.  Trailing line
 * endings are removed.  `env` entries are applied over the inherited
 * environment for the duration of the command.  Returns false if the command
 * could not be started.  `status` receives the exit status when available
 * (-1 otherwise).
 */
bool platform_run_command(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    std::string &output, int &status);

/*
 * Run a shell command with inherited standard streams and return its exit
 * status.  Returns false if the command processor could not be started.
 */
bool platform_run_command_status(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    int &status);

/* Read an entire file as bytes.  On failure, `error` describes the problem. */
bool platform_read_file(const std::string &path, std::string &data,
                        std::string &error);

/* Open a file using the platform's UTF-8 path handling. */
FILE *platform_open_file(const std::string &path, const char *mode);

/* Write or append `data` to a file.  On failure, `error` is set. */
bool platform_write_file(const std::string &path, const std::string &data,
                         bool append, std::string &error);

/* Return true when `path` exists. */
bool platform_file_exists(const std::string &path);

/* Opaque streaming file handle. */
struct platform_file;
using platform_file_ptr = std::shared_ptr<platform_file>;

platform_file_ptr platform_file_open(const std::string &path,
                                     const std::string &mode,
                                     std::string &error);
bool platform_file_read(const platform_file_ptr &file, size_t limit,
                        std::string &data, std::string &error);
bool platform_file_write(const platform_file_ptr &file,
                         const std::string &data, std::string &error);
bool platform_file_seek(const platform_file_ptr &file, long long offset,
                        int whence, std::string &error);
long long platform_file_tell(const platform_file_ptr &file,
                             std::string &error);
bool platform_file_eof(const platform_file_ptr &file);
bool platform_file_flush(const platform_file_ptr &file, std::string &error);
void platform_file_close(const platform_file_ptr &file, std::string &error);
bool platform_file_is_open(const platform_file_ptr &file);
std::string platform_file_path(const platform_file_ptr &file);
int platform_file_fd(const platform_file_ptr &file);

#endif
