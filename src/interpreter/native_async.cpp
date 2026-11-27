#include "native_async.h"

#include "analysis_mode.h"
#include "binding_registry.h"
#include "eval.h"
#include "event_loop.h"
#include "gc.h"
#include "symbol.h"
#include "task.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace skein {
namespace native_async {
namespace {

struct job {
    skein_task_work_fn work = nullptr;
    skein_task_finish_fn finish = nullptr;
    skein_task_destroy_fn destroy = nullptr;
    void *data = nullptr;
    skein_context owner_context;
    std::shared_ptr<void> module_owner;
    task_ptr task;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<std::atomic<bool>> failed;
    std::atomic<bool> done{false};
    std::thread thread;
};

std::mutex &job_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<job>> &job_queue() {
    static std::vector<std::shared_ptr<job>> queue;
    return queue;
}

/* ABI v4 posted callbacks.  Values are copied on the posting thread, then
 * released on the owner thread when the callback runs or is cancelled. */
struct post_job {
    skein_post_id id = 0;
    skein_context owner_context;
    std::shared_ptr<void> module_owner;
    value_t *callee = nullptr;
    std::vector<value_t *> args;
    std::atomic<bool> cancelled{false};
};

std::mutex &post_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<post_job>> &post_queue() {
    static std::vector<std::shared_ptr<post_job>> queue;
    return queue;
}

std::atomic<skein_post_id> &next_post_id() {
    static std::atomic<skein_post_id> id{1};
    return id;
}

void release_post_values(post_job &job) {
    freeval(job.callee);
    job.callee = nullptr;
    for (value_t *arg : job.args)
        freeval(arg);
    job.args.clear();
}

/* Queued callbacks live in C++ state until the owner thread drains them, so
 * their closures must be marked by the collector independently of the active
 * script frame. */
void visit_post_roots(void *, gc_mark_fn mark, void *mark_ctx, gc_pin_fn pin,
                      void *pin_ctx) {
    std::lock_guard<std::mutex> lock(post_mutex());
    for (const std::shared_ptr<post_job> &job : post_queue()) {
        gc_mark_pinned_value(pin, pin_ctx, mark, mark_ctx, job->callee);
        for (value_t *arg : job->args)
            gc_mark_pinned_value(pin, pin_ctx, mark, mark_ctx, arg);
    }
}

} // namespace

void install(skein_context *ctx) {
    if (!ctx)
        return;
    static const bool roots_registered = [] {
        gc_add_global_root_visitor(&visit_post_roots);
        return true;
    }();
    (void)roots_registered;
    ctx->drain_native_tasks = background_drain;
    ctx->call_task = call_task;
    ctx->task_cancelled = task_cancelled;
    ctx->post_callback = post_callback;
    ctx->cancel_post_callback = cancel_post_callback;
}

struct value_t *call_task(skein_task_work_fn work,
                          skein_task_finish_fn finish,
                          skein_task_destroy_fn destroy, void *data) {
    if (g_check_only) {
        yyerror("native async tasks are disabled in check-only mode");
        return NULL;
    }
    if (!work || !finish) {
        yyerror("native call_task requires work and finish callbacks");
        return NULL;
    }
    const skein_context *current = skein_context_current();
    if (!current) {
        yyerror("native call_task requires a runtime context");
        return NULL;
    }
    if (!event_loop_init()) {
        yyerror("native call_task cannot initialize the event loop");
        return NULL;
    }

    auto pending = std::make_shared<job>();
    pending->work = work;
    pending->finish = finish;
    pending->destroy = destroy;
    pending->data = data;
    pending->owner_context = *current;
    pending->module_owner = current_module_owner;
    pending->cancelled = std::make_shared<std::atomic<bool>>(false);
    pending->failed = std::make_shared<std::atomic<bool>>(false);
    pending->task = task_create();
    task_set_cancel(pending->task, [cancelled = pending->cancelled] {
        cancelled->store(true);
    });

    auto state = pending;
    pending->thread = std::thread([state] {
        /* Keep the owning plugin loaded if the worker posts callbacks. */
        module_owner_guard owner_guard(state->module_owner);
        try {
            state->work(state->data);
        } catch (...) {
            state->failed->store(true);
        }
        state->done.store(true);
    });

    {
        std::lock_guard<std::mutex> lock(job_mutex());
        job_queue().push_back(std::move(pending));
    }
    return new_task_value(state->task);
}

bool task_cancelled(struct value_t *task) {
    task_ptr handle;
    if (!get_task_value(task, handle))
        return false;
    return task_get_state(handle) == TASK_STATE_CANCELLED;
}

skein_post_id post_callback(skein_context *ctx, const value_t *callee,
                            const value_t *const *args, size_t argc) {
    if (g_check_only) {
        yyerror("native post_callback is disabled in check-only mode");
        return 0;
    }
    if (!ctx || !ctx->call || !callee) {
        yyerror("native post_callback requires a context and a callback");
        return 0;
    }
    if (argc > 0 && !args) {
        yyerror("native post_callback requires an argument array");
        return 0;
    }

    auto job = std::make_shared<post_job>();
    job->id = next_post_id().fetch_add(1);
    if (job->id == 0)
        job->id = next_post_id().fetch_add(1);
    job->owner_context = *ctx;
    job->module_owner = current_module_owner;
    job->callee = dupval(const_cast<value_t *>(callee));
    if (!job->callee) {
        yyerror("native post_callback could not copy the callback");
        return 0;
    }
    job->args.reserve(argc);
    for (size_t i = 0; i < argc; ++i) {
        value_t *arg = dupval(const_cast<value_t *>(args[i]));
        if (!arg) {
            yyerror("native post_callback could not copy an argument");
            release_post_values(*job);
            return 0;
        }
        job->args.push_back(arg);
    }

    skein_post_id id = job->id;
    {
        std::lock_guard<std::mutex> lock(post_mutex());
        post_queue().push_back(std::move(job));
    }
    return id;
}

bool cancel_post_callback(skein_context *ctx, skein_post_id id) {
    (void)ctx;
    if (!id)
        return false;
    std::lock_guard<std::mutex> lock(post_mutex());
    for (const std::shared_ptr<post_job> &job : post_queue()) {
        if (job->id == id) {
            /* Values are released by the owner-thread drain. */
            job->cancelled.store(true);
            return true;
        }
    }
    return false;
}

namespace {

void drain_post_callbacks() {
    std::vector<std::shared_ptr<post_job>> ready;
    {
        std::lock_guard<std::mutex> lock(post_mutex());
        ready.swap(post_queue());
    }

    for (const std::shared_ptr<post_job> &job : ready) {
        module_owner_guard owner_guard(job->module_owner);
        if (!job->cancelled.load()) {
            skein_context *previous = skein_context_current();
            skein_context_set(&job->owner_context);
            std::vector<value_t *> args = std::move(job->args);
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            value_t *result =
                skein_call_function(job->callee, std::move(args));
            eval_capture_result captured = eval_capture_end(snapshot);
            if (captured.failed || !result) {
                std::string message =
                    captured.failed ? error_message_of(captured.error)
                                    : std::string("native callback failed");
                freeval(captured.error);
                freeval(result);
                yyerror("native post_callback: %s", message.c_str());
            } else {
                freeval(captured.error);
                freeval(result);
            }
            skein_context_set(previous);
        }
        release_post_values(*job);
    }
}

} // namespace

void background_drain() {
    static thread_local bool draining = false;
    if (draining)
        return;
    draining = true;
    struct drain_guard {
        bool &flag;
        ~drain_guard() { flag = false; }
    } guard{draining};

    /* Posted callbacks run before task completions in the same pump, so a
     * worker that posts and then returns sees its callback delivered first. */
    drain_post_callbacks();

    std::vector<std::shared_ptr<job>> ready;
    {
        std::lock_guard<std::mutex> lock(job_mutex());
        std::vector<std::shared_ptr<job>> &queue = job_queue();
        for (auto it = queue.begin(); it != queue.end();) {
            if ((*it)->done.load()) {
                ready.push_back(std::move(*it));
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const std::shared_ptr<job> &state : ready) {
        if (state->thread.joinable())
            state->thread.join();

        skein_context *previous = skein_context_current();
        skein_context_set(&state->owner_context);
        module_owner_guard owner_guard(state->module_owner);
        if (state->failed->load()) {
            task_fail(state->task, "native task failed");
        } else {
            struct value_t *result = reinterpret_cast<value_t *>(
                state->finish(&state->owner_context, state->data));
            if (result)
                task_complete(state->task, result);
            else
                task_fail(state->task, "native task failed");
        }
        if (state->destroy && state->data)
            state->destroy(state->data);
        skein_context_set(previous);
    }
}

void shutdown() {
    /* Discard rather than run callbacks still queued at interpreter exit. */
    std::vector<std::shared_ptr<post_job>> posted;
    {
        std::lock_guard<std::mutex> lock(post_mutex());
        posted.swap(post_queue());
    }
    for (const std::shared_ptr<post_job> &job : posted)
        release_post_values(*job);

    std::vector<std::shared_ptr<job>> pending;
    {
        std::lock_guard<std::mutex> lock(job_mutex());
        pending.swap(job_queue());
    }
    for (const std::shared_ptr<job> &state : pending) {
        state->cancelled->store(true);
        if (state->thread.joinable())
            state->thread.join();
        if (state->destroy && state->data)
            state->destroy(state->data);
    }
}

} // namespace native_async
} // namespace skein
