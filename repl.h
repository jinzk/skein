#ifndef CALC_REPL_H
#define CALC_REPL_H

#include <csignal>
#include <cstdio>
#include <string>

/* Set from the SIGINT handler; only async-signal-safe writes are allowed. */
extern volatile std::sig_atomic_t g_interrupted;

/* Set when stdin really reaches EOF (not when Ctrl-C aborts a read). */
extern volatile std::sig_atomic_t g_stdin_eof;

/* Number of runtime errors reported by yyerror since the last statement. */
extern int g_runtime_error_count;

/* True for the interactive stdin REPL; false when running a script file. */
extern bool g_interactive;

/* True when the REPL should display prompts and use the line editor. */
extern bool g_show_prompt;

/* Nonzero while evaluating an imported package source file. */
extern int g_package_loading;

/* Continue after script errors instead of stopping at the first one. */
extern bool g_keep_going;

/* Set when a script error requires stopping the current parse. */
extern bool g_script_stop;

/* Set when the current statement has a lexical error and must not evaluate. */
extern bool g_statement_invalid;

enum repl_line_status {
    REPL_LINE,
    REPL_EOF,
    REPL_INTERRUPTED
};

struct value_t;

/* Read one logical line (without a trailing newline). */
repl_line_status repl_read_line(std::string &line, bool continuation);

/* Persistent history management. */
void repl_history_load();
void repl_history_save();

/* Update the implicit `ans` and `_` bindings. */
void repl_set_last_result(struct value_t *value);

/* Discard buffered input and repoint the scanner at `in`. */
void reset_scanner_state(std::FILE *in);

/* Save/restore the scanner between outer source and imported packages. */
struct scanner_checkpoint;
scanner_checkpoint *scanner_checkpoint_save();
void scanner_checkpoint_restore(scanner_checkpoint *checkpoint);

#endif
