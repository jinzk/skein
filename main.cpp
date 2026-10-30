#include <cstdio>
#include <cstdlib>

#include "parser.hpp"
#include "platform.h"
#include "repl.h"
#include "scopestack.h"

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

int indent_count = 0;

int main() {
    platform_init();
    platform_install_interrupt_handler();
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
