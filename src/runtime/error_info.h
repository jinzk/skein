#ifndef SKEIN_ERROR_INFO_H
#define SKEIN_ERROR_INFO_H

#include <string>
#include <vector>

/* One frame of a captured error call stack.  `function` is the callee;
 * `source`, `line` and `column` identify the call site in the caller. */
struct error_frame {
    std::string source;
    std::string function;
    int line = 0;
    int column = 0;
};

#endif
