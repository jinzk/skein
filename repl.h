#ifndef SKEIN_REPL_H
#define SKEIN_REPL_H

#include "runtime_state.h"

#include <cstdio>
#include <string>

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
