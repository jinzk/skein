#include "platform.h"

#include <csignal>

#include "repl.h"

static void on_sigint(int) {
    g_interrupted = 1;
}

void platform_init() {
}

void platform_install_interrupt_handler() {
    struct sigaction sa = {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a blocking read must return so we can cancel the line. */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}
