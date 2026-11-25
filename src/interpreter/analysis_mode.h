#ifndef SKEIN_ANALYSIS_MODE_H
#define SKEIN_ANALYSIS_MODE_H

/*
 * Analysis-only modes used by `skein lint` and future language services.
 *
 * When g_check_only is true the parser still builds the AST and runs the
 * static typechecker, but skips evaluation of statements that have runtime
 * side effects.  Declaration-like statements are still evaluated so imports,
 * struct/enum/protocol definitions and packages can populate the symbol and type
 * tables needed by the checker.  The flag is thread-local so nested parses
 * (package imports) inherit it.
 */
extern thread_local bool g_check_only;

struct check_only_guard {
    bool previous;

    check_only_guard();
    ~check_only_guard();
};

#endif
