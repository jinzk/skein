#ifndef SKEIN_RUNTIME_STATE_H
#define SKEIN_RUNTIME_STATE_H

#include <csignal>

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

#endif
