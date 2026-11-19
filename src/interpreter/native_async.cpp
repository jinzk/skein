#include "native_async.h"

#include "analysis_mode.h"
#include "binding_registry.h"
#include "event_loop.h"
#include "symbol.h"
#include "task.h"

#include <atomic>
#include <memory>
#include <mutex>
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

} // namespace

void install(skein_context *ctx) {
    if (!ctx)
        return;
    ctx->drain_native_tasks = background_drain;
    ctx->call_task = call_task;
    ctx->task_cancelled = task_cancelled;
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

void background_drain() {
    static thread_local bool draining = false;
    if (draining)
        return;
    draining = true;
    struct drain_guard {
        bool &flag;
        ~drain_guard() { flag = false; }
    } guard{draining};

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
