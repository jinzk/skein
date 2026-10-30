#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "parser.hpp"
#include "platform.h"
#include "repl.h"
#include "scopestack.h"

extern FILE *yyin;

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

bool g_interactive = true;
int indent_count = 0;

int main(int argc, char **argv) {
    platform_init();
    platform_install_interrupt_handler();

    FILE *script = NULL;
    if (argc > 1) {
        g_interactive = false;
        if (std::strcmp(argv[1], "-") != 0) {
            script = std::fopen(argv[1], "rb");
            if (!script) {
                std::perror(argv[1]);
                return 1;
            }
            yyin = script;
        }
    }

    initscope();
    if (g_interactive) {
        std::printf("> ");
        std::fflush(stdout);
    }

    int status = 0;
    for (;;) {
        g_interrupted = 0;
        status = yyparse();

        if (!g_interrupted || g_stdin_eof)
            break;
        if (!g_interactive)
            break;

        /* Ctrl-C: drop the current line/block and start a fresh statement. */
        g_interrupted = 0;
        indent_count = 0;
        reset_scanner_state(yyin ? yyin : stdin);
        std::printf("\n> ");
        std::fflush(stdout);
    }

    donescope();
    if (script)
        std::fclose(script);
    return status;
}
