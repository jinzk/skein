#ifndef SKEIN_NATIVE_ASYNC_H
#define SKEIN_NATIVE_ASYNC_H

#include "runtime_context.h"
#include "skein_module.h"

namespace skein {
namespace native_async {

/* Install the ABI v2 task hooks on a freshly created skein_context. */
void install(skein_context *ctx);

/* Schedule `work` on a worker thread.  `finish` runs on the owner thread and
 * its result completes the returned standard task value. */
struct value_t *call_task(skein_task_work_fn work, skein_task_finish_fn finish,
                          skein_task_destroy_fn destroy, void *data);

/* True when the standard task has been cancelled.  Safe from worker threads. */
bool task_cancelled(struct value_t *task);

/* ABI v4: copy `callee`/`args` and queue them for execution on the owner
 * thread.  Thread-safe, including from call_task workers.  Returns 0 on
 * failure.  `cancel_post_callback` is also thread-safe. */
skein_post_id post_callback(skein_context *ctx, const struct value_t *callee,
                            const struct value_t *const *args, size_t argc);
bool cancel_post_callback(skein_context *ctx, skein_post_id id);

/* Drain completed native jobs on the owner thread.  Called by fiber_pump(). */
void background_drain();

/* Join and release all outstanding jobs at thread shutdown. */
void shutdown();

} // namespace native_async
} // namespace skein

#endif
