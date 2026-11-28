#include "runtime_context.h"
#include "runtime_state.h"

#include <cstdarg>
#include <cstdio>

/*
 * Runtime-facing error entry point.  The interpreter installs a handler via
 * skein_context::error; without one we fall back to stderr so standalone
 * embedding cannot silently lose errors.
 */
void yyerror_v(const char *s, va_list ap) {
    if (g_interrupted)
        return;

    char buffer[2048];
    std::vsnprintf(buffer, sizeof(buffer), s, ap);

    struct skein_context *ctx = skein_context_current();
    if (ctx && ctx->error) {
        ctx->error(buffer);
        return;
    }
    std::fprintf(stderr, "%s\n", buffer);
}

void yyerror(const char *s, ...) {
    va_list ap;
    va_start(ap, s);
    yyerror_v(s, ap);
    va_end(ap);
}
