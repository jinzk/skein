#ifndef SKEIN_PARSER_LOCK_H
#define SKEIN_PARSER_LOCK_H

#include <mutex>

/*
 * The generated lexer/parser keeps process-global scanner state.  Parsing is
 * serialized across runtime threads; evaluation happens after the parse lock
 * is released.
 */
inline std::recursive_mutex &parser_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

/*
 * Guards a whole package parse/eval transaction.  Worker parsing must not
 * start while an imported module's top level is still executing, because that
 * top level may be waiting for the worker (for example when a pool is created
 * inside the import).  Workers use a timed attempt on this gate and fail with
 * a diagnostic instead of deadlocking.
 */
inline std::recursive_timed_mutex &package_parse_gate() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

#endif
