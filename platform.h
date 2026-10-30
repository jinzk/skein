#ifndef CALC_PLATFORM_H
#define CALC_PLATFORM_H

#include <string>
#include <utility>
#include <vector>

/* One-time process/platform initialization: console encoding and I/O mode. */
void platform_init();

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

#endif
