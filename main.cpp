#include <cstdio>
#include <cstdlib>

#include "scopestack.h"
#include "parser.hpp"

int indent_count = 0;

int main() {
    std::printf("> ");
    std::fflush(stdout);
    initscope();
    int status = yyparse();
    donescope();
    return status;
}
