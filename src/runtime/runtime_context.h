#ifndef SKEIN_RUNTIME_CONTEXT_H
#define SKEIN_RUNTIME_CONTEXT_H

#include "skein_module.h"
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

    /* ABI v2 native task hooks.  Installed by skein::native_async::install().
     * `drain_native_tasks` is separate from `drain_background` so the FFI
     * callback drain is not replaced. */
    void (*drain_native_tasks)() = nullptr;
    struct value_t *(*call_task)(skein_task_work_fn work,
                                 skein_task_finish_fn finish,
                                 skein_task_destroy_fn destroy,
                                 void *data) = nullptr;
    bool (*task_cancelled)(struct value_t *task) = nullptr;

    /* ABI v4 posted callbacks.  `post_callback` is thread-safe and may be
     * called from native task workers; it queues values for the owner thread.
     * Installed by skein::native_async::install() alongside the task hooks. */
    skein_post_id (*post_callback)(skein_context *ctx,
                                   const struct value_t *callee,
                                   const struct value_t *const *args,
                                   size_t argc) = nullptr;
    bool (*cancel_post_callback)(skein_context *ctx, skein_post_id id) = nullptr;
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
