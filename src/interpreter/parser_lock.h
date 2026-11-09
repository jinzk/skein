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
 * inside the import).  The parser releases this gate and `parser_mutex` while
 * evaluating a top-level statement, so workers can parse their scripts during
 * that window.
 */
inline std::recursive_timed_mutex &package_parse_gate() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

/* Number of recursive parse transactions held by this thread. */
inline int &parser_lock_depth() {
    static thread_local int depth = 0;
    return depth;
}

struct parser_lock_depth_guard {
    parser_lock_depth_guard() { ++parser_lock_depth(); }
    ~parser_lock_depth_guard() { --parser_lock_depth(); }
    parser_lock_depth_guard(const parser_lock_depth_guard &) = delete;
    parser_lock_depth_guard &operator=(const parser_lock_depth_guard &) = delete;
};

/*
 * RAII helper that unlocks all recursive parser transactions held by the
 * current thread and reacquires them on scope exit.  Used around top-level
 * statement evaluation so worker threads can parse their entry scripts.
 */
struct parser_lock_suspend {
    int depth = 0;

    parser_lock_suspend() {
        depth = parser_lock_depth();
        for (int i = 0; i < depth; ++i) {
            parser_mutex().unlock();
            package_parse_gate().unlock();
        }
        parser_lock_depth() = 0;
    }

    ~parser_lock_suspend() {
        for (int i = 0; i < depth; ++i) {
            package_parse_gate().lock();
            parser_mutex().lock();
        }
        parser_lock_depth() = depth;
    }

    parser_lock_suspend(const parser_lock_suspend &) = delete;
    parser_lock_suspend &operator=(const parser_lock_suspend &) = delete;
};

#endif
