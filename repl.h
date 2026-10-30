#ifndef CALC_REPL_H
#define CALC_REPL_H

#include <csignal>
#include <cstdio>

/* Set from the SIGINT handler; only async-signal-safe writes are allowed. */
extern volatile std::sig_atomic_t g_interrupted;

/* Set when stdin really reaches EOF (not when Ctrl-C aborts a read). */
extern volatile std::sig_atomic_t g_stdin_eof;

/* Discard buffered input and repoint the scanner at `in`. */
void reset_scanner_state(std::FILE *in);

#endif
