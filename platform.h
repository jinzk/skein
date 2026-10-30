#ifndef CALC_PLATFORM_H
#define CALC_PLATFORM_H

/* One-time process/platform initialization: console encoding and I/O mode. */
void platform_init();

/* Install the Ctrl-C handler used by the REPL. */
void platform_install_interrupt_handler();

#endif
