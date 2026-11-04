#ifndef SKEIN_RUNTIME_CONTEXT_H
#define SKEIN_RUNTIME_CONTEXT_H

#include "valuetype.h"

#include <utility>
#include <vector>

/*
 * Internal runtime boundary.  The interpreter/native frontend installs the
 * callbacks; builtins only see this small interface, never eval.h.
 */
struct skein_context {
    struct value_t *(*call)(struct value_t *callee,
                            std::vector<struct value_t *> args);
    struct value_t *(*call_builtin)(
        int functype, const std::vector<struct value_t *> &args);
    void (*error)(const char *message);
    /* Optional background work (for example queued FFI callbacks) drained
     * while the interpreter pumps the event loop. */
    void (*drain_background)() = nullptr;
};

struct skein_context *skein_context_current();
void skein_context_set(struct skein_context *ctx);

inline struct value_t *skein_call_function(
    struct value_t *callee, std::vector<struct value_t *> args) {
    struct skein_context *ctx = skein_context_current();
    if (!ctx || !ctx->call)
        return NULL;
    return ctx->call(callee, std::move(args));
}

#endif
