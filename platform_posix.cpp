#include "platform.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
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
