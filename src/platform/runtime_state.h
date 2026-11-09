#ifndef SKEIN_RUNTIME_STATE_H
#define SKEIN_RUNTIME_STATE_H

#include <csignal>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/*
 * Process-level runtime flags shared by the platform layer, the interpreter
 * and the REPL.  Kept in a small standalone translation unit so the platform
 * library does not depend on the CLI/REPL layer.
 */

extern volatile std::sig_atomic_t g_interrupted;
extern volatile std::sig_atomic_t g_stdin_eof;

extern thread_local bool g_interactive;
extern thread_local bool g_show_prompt;
extern thread_local int g_package_loading;
extern thread_local bool g_keep_going;
extern thread_local bool g_script_stop;
extern thread_local bool g_statement_invalid;

/*
 * Per-thread environment and working directory overlays.  Reads fall back to
 * the process environment/cwd; writes only affect the calling thread.
 */
bool thread_env_valid_name(const std::string &name);
bool thread_env_get(const std::string &name, std::string &value);
void thread_env_set(const std::string &name, const std::string &value);
void thread_env_unset(const std::string &name);
void thread_env_all(
    std::vector<std::pair<std::string, std::string>> &entries);
void thread_env_overrides(
    std::vector<std::pair<std::string, std::string>> &entries);
void thread_env_unsets(std::vector<std::string> &names);

std::string thread_cwd();
const std::string &thread_cwd_override();
bool thread_chdir(const std::string &path, std::string &error);
std::string thread_resolve_path(const std::string &path);

struct thread_env_snapshot;
std::shared_ptr<const thread_env_snapshot> thread_env_capture();
void thread_env_install(
    const std::shared_ptr<const thread_env_snapshot> &snapshot);

#endif
