#include "platform.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include "repl.h"

namespace {

class scoped_environment {
    struct saved_value {
        std::string name;
        std::string value;
        bool existed;
    };

    std::vector<saved_value> saved_;
    bool ok_ = true;

public:
    explicit scoped_environment(
        const std::vector<std::pair<std::string, std::string>> &env) {
        for (const auto &entry : env) {
            const char *old = std::getenv(entry.first.c_str());
            saved_.push_back(
                {entry.first, old ? old : "", old != nullptr});
            if (setenv(entry.first.c_str(), entry.second.c_str(), 1) != 0) {
                ok_ = false;
                break;
            }
        }
    }

    ~scoped_environment() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (it->existed)
                setenv(it->name.c_str(), it->value.c_str(), 1);
            else
                unsetenv(it->name.c_str());
        }
    }

    bool ok() const { return ok_; }
};

}  // namespace

static void on_sigint(int) {
    g_interrupted = 1;
}

void platform_init() {
}

std::vector<std::string> platform_command_line_args(int argc, char **argv) {
    if (argc <= 0 || !argv)
        return {};
    return std::vector<std::string>(argv, argv + argc);
}

std::string platform_executable_path() {
#if defined(__linux__)
    char buffer[4096];
    const ssize_t length =
        readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (length > 0)
        return std::string(buffer, (size_t)length);
#endif
    return std::string();
}

bool platform_get_env(const std::string &name, std::string &value) {
    const char *env = std::getenv(name.c_str());
    if (!env)
        return false;
    value = env;
    return true;
}

bool platform_set_env(const std::string &name, const std::string &value) {
    if (name.empty() || name.find('=') != std::string::npos)
        return false;
    return setenv(name.c_str(), value.c_str(), 1) == 0;
}

bool platform_unset_env(const std::string &name) {
    if (name.empty() || name.find('=') != std::string::npos)
        return false;
    return unsetenv(name.c_str()) == 0;
}

bool platform_environ(
    std::vector<std::pair<std::string, std::string>> &entries) {
    entries.clear();
    extern char **environ;
    if (!environ)
        return true;
    for (char **entry = environ; *entry; ++entry) {
        const char *separator = std::strchr(*entry, '=');
        if (!separator)
            continue;
        entries.push_back(std::make_pair(
            std::string(*entry, (size_t)(separator - *entry)),
            std::string(separator + 1)));
    }
    return true;
}

bool platform_stdin_is_tty() {
    return isatty(fileno(stdin)) != 0;
}

void platform_install_interrupt_handler() {
    struct sigaction sa = {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a blocking read must return so we can cancel the line. */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

bool platform_run_command(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    std::string &output, int &status) {
    output.clear();
    status = -1;

    scoped_environment scope(env);
    if (!scope.ok())
        return false;

    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
        return false;

    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
        output.append(buffer, n);

    int rc = pclose(pipe);
    if (rc != -1) {
        if (WIFEXITED(rc))
            status = WEXITSTATUS(rc);
        else if (WIFSIGNALED(rc))
            status = 128 + WTERMSIG(rc);
    }

    /* Command substitution convention: drop trailing line endings. */
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return true;
}

bool platform_run_command_status(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    int &status) {
    status = -1;

    scoped_environment scope(env);
    if (!scope.ok())
        return false;

    int rc = std::system(command.c_str());
    if (rc == -1)
        return false;
    if (WIFEXITED(rc))
        status = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc))
        status = 128 + WTERMSIG(rc);
    else
        status = rc;
    return true;
}

FILE *platform_open_file(const std::string &path, const char *mode) {
    return std::fopen(path.c_str(), mode);
}

bool platform_read_file(const std::string &path, std::string &data,
                        std::string &error) {
    data.clear();
    error.clear();

    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = std::strerror(errno);
        return false;
    }

    char buffer[4096];
    size_t received;
    while ((received = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        data.append(buffer, received);

    if (std::ferror(file)) {
        int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return false;
    }
    if (std::fclose(file) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool platform_write_file(const std::string &path, const std::string &data,
                         bool append, std::string &error) {
    error.clear();

    FILE *file = std::fopen(path.c_str(), append ? "ab" : "wb");
    if (!file) {
        error = std::strerror(errno);
        return false;
    }

    size_t written = std::fwrite(data.data(), 1, data.size(), file);
    if (written != data.size()) {
        int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return false;
    }
    if (std::fclose(file) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool platform_file_exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

struct platform_file {
    FILE *stream = nullptr;
    std::string path;

    ~platform_file() {
        if (stream)
            std::fclose(stream);
    }
};

platform_file_ptr platform_file_open(const std::string &path,
                                     const std::string &mode,
                                     std::string &error) {
    error.clear();
    FILE *stream = std::fopen(path.c_str(), mode.c_str());
    if (!stream) {
        error = std::strerror(errno);
        return nullptr;
    }
    std::setvbuf(stream, nullptr, _IONBF, 0);
    std::shared_ptr<platform_file> file = std::make_shared<platform_file>();
    file->stream = stream;
    file->path = path;
    return file;
}

bool platform_file_read(const platform_file_ptr &file, size_t limit,
                        std::string &data, std::string &error) {
    data.clear();
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }

    char buffer[4096];
    size_t total = 0;
    while (limit == 0 || total < limit) {
        size_t want = sizeof buffer;
        if (limit != 0 && limit - total < want)
            want = limit - total;
        size_t received = std::fread(buffer, 1, want, file->stream);
        if (received > 0) {
            data.append(buffer, received);
            total += received;
        }
        if (received < want) {
            if (std::ferror(file->stream)) {
                error = std::strerror(errno);
                return false;
            }
            break;
        }
    }
    return true;
}

bool platform_file_write(const platform_file_ptr &file,
                         const std::string &data, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    size_t written = 0;
    while (written < data.size()) {
        size_t n = std::fwrite(data.data() + written, 1,
                               data.size() - written, file->stream);
        if (n == 0) {
            error = std::strerror(errno);
            return false;
        }
        written += n;
    }
    return true;
}

bool platform_file_seek(const platform_file_ptr &file, long long offset,
                        int whence, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    if (whence < 0 || whence > 2) {
        error = "invalid whence";
        return false;
    }
    if (fseeko(file->stream, (off_t)offset, whence) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

long long platform_file_tell(const platform_file_ptr &file,
                             std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return -1;
    }
    off_t position = ftello(file->stream);
    if (position < 0) {
        error = std::strerror(errno);
        return -1;
    }
    return (long long)position;
}

bool platform_file_eof(const platform_file_ptr &file) {
    return file && file->stream && std::feof(file->stream) != 0;
}

bool platform_file_flush(const platform_file_ptr &file, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    if (std::fflush(file->stream) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

void platform_file_close(const platform_file_ptr &file, std::string &error) {
    error.clear();
    if (!file || !file->stream)
        return;
    if (std::fclose(file->stream) != 0)
        error = std::strerror(errno);
    file->stream = nullptr;
}

bool platform_file_is_open(const platform_file_ptr &file) {
    return file && file->stream != nullptr;
}

int platform_file_fd(const platform_file_ptr &file) {
    if (!file || !file->stream)
        return -1;
    return fileno(file->stream);
}

std::string platform_file_path(const platform_file_ptr &file) {
    return file ? file->path : std::string();
}
