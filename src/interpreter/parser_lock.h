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

#endif
