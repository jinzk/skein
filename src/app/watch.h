#ifndef SKEIN_APP_WATCH_H
#define SKEIN_APP_WATCH_H

#include <string>
#include <vector>

/*
 * Development watch mode: run `command` as a child process, watch the given
 * source directories, and restart the child when a `.skein` file changes.
 *
 * The watcher never loads the language runtime itself.  It is deliberately a
 * process-level supervisor, so a restart always starts from a clean runtime.
 */
struct watch_options {
    std::string executable;
    std::vector<std::string> command;
    std::vector<std::string> roots;
};

int run_watch_mode(const watch_options &options);

#endif
