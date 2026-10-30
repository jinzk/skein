#include <cstdio>
#include <cstdlib>

#include "parser.hpp"
#include "repl.h"
#include "scopestack.h"

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

int indent_count = 0;

static void on_sigint(int) {
    g_interrupted = 1;
}

static void install_sigint_handler() {
    struct sigaction sa = {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a blocking read must return so we can cancel the line. */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

int main() {
    install_sigint_handler();
    initscope();
    std::printf("> ");
    std::fflush(stdout);

    int status = 0;
    for (;;) {
        g_interrupted = 0;
        status = yyparse();

        if (!g_interrupted || g_stdin_eof)
            break;

        /* Ctrl-C: drop the current line/block and start a fresh statement. */
        g_interrupted = 0;
        indent_count = 0;
        reset_scanner_state(stdin);
        std::printf("\n> ");
        std::fflush(stdout);
    }

    donescope();
    return status;
}
