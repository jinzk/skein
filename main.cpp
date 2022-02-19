#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "scopestack.h"
#include "parser.hpp"

int indent_count = 0;

int main() {
    printf("> ");
    initscope();
	return yyparse();
}

