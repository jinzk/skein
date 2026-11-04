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

extern bool g_interactive;
extern bool g_show_prompt;
extern int g_package_loading;
extern bool g_keep_going;
extern bool g_script_stop;
extern bool g_statement_invalid;

#endif
