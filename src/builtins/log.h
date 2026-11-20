#ifndef SKEIN_LOG_H
#define SKEIN_LOG_H

#include <vector>

struct value_t;

/* std.log: structured logging with text/JSON output, level filtering and
 * file or standard-stream sinks. */
struct value_t *call_log_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
