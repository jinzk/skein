#include "fiber.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval_context.h"
#include "event_loop.h"
#include "repl.h"
#include "scopestack.h"
#include "task.h"
#include "valuetype.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32

#include <ucontext.h>

namespace {

enum fiber_state {
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_WAITING,
    FIBER_DONE
};

struct fiber {
    ucontext_t context = {};
    std::vector<char> stack;
    eval_context eval;
    diag_state diagnostics;
    std::function<value_t *()> body;
    task_ptr task;
    task_ptr await_task;
    ucontext_t *resume_context = NULL;
    fiber_state state = FIBER_READY;
    bool canceled = false;
    bool queued = false;

    explicit fiber(std::function<value_t *()> fn)
        : body(std::move(fn)) {
        stack.resize(2 * 1024 * 1024);
        getcontext(&context);
        context.uc_stack.ss_sp = stack.data();
        context.uc_stack.ss_size = stack.size();
        context.uc_link = NULL;
        makecontext(&context, (void (*)(void))fiber_main, 0);
    }

    static void fiber_main();
};

fiber *current_fiber = NULL;
fiber *starting_fiber = NULL;
eval_context *root_context = NULL;
bool shutting_down = false;
std::deque<fiber *> ready_fibers;
std::vector<fiber *> all_fibers;

void remove_fiber(fiber *value) {
    all_fibers.erase(std::remove(all_fibers.begin(), all_fibers.end(), value),
                     all_fibers.end());
    delete value;
}

void make_ready(fiber *value) {
    if (!value || value->state == FIBER_DONE || value->queued)
        return;
    value->state = FIBER_READY;
    value->queued = true;
    ready_fibers.push_back(value);
}

void switch_into(fiber *value, ucontext_t *target, fiber *previous) {
    if (!previous && !root_context)
        root_context = &active_eval_context();

    diag_state previous_diagnostics;
    diag_capture(previous_diagnostics);
    diag_restore(value->diagnostics);
    set_active_eval_context(&value->eval);
    current_fiber = value;
    value->resume_context = target;
    value->state = FIBER_RUNNING;
    swapcontext(target, &value->context);

    diag_capture(value->diagnostics);
    current_fiber = previous;
    if (previous) {
        set_active_eval_context(&previous->eval);
        diag_restore(previous->diagnostics);
    } else {
        set_active_eval_context(root_context);
        diag_restore(previous_diagnostics);
    }
}

void suspend_current() {
    fiber *value = current_fiber;
    if (!value || !value->resume_context)
        return;
    value->state = FIBER_WAITING;
    swapcontext(&value->context, value->resume_context);
    value->state = FIBER_RUNNING;
}

value_t *await_result(const task_ptr &task) {
    if (!task) {
        yyerror("await expects a task");
        return NULL;
    }
    switch (task_get_state(task)) {
        case TASK_STATE_DONE:
            return task_copy_result(task);
        case TASK_STATE_FAILED:
            yyerror("task failed: %s",
                    task_error_message(task).c_str());
            return NULL;
        case TASK_STATE_CANCELLED:
            yyerror("task was cancelled");
            return NULL;
        case TASK_STATE_PENDING:
        default:
            return NULL;
    }
}

} // namespace

void fiber::fiber_main() {
    fiber *value = starting_fiber;
    if (!value)
        std::abort();

    value_t *result = value->body ? value->body() : NULL;
    value->body = nullptr;

    if (value->canceled) {
        freeval(result);
    } else if (!result) {
        task_fail(value->task, "async function failed");
    } else {
        task_complete(value->task, result);
    }

    if (root_context)
        adopt_scopes(value->eval.scopes, root_context->scopes);
    value->state = FIBER_DONE;
    setcontext(value->resume_context);
}

bool fiber_available() {
    return true;
}

task_ptr fiber_start_async(std::function<value_t *()> body) {
    task_ptr task = task_create();
    fiber *value = new fiber(std::move(body));
    value->task = task;
    diag_capture(value->diagnostics);
    value->eval.scopes.global_scope =
        active_eval_context().scopes.global_scope;

    task_set_cancel(task, [value] {
        if (value->state == FIBER_DONE)
            return;
        value->canceled = true;
        if (value->state == FIBER_WAITING)
            make_ready(value);
    });

    all_fibers.push_back(value);

    fiber *previous = current_fiber;
    ucontext_t caller_context;
    starting_fiber = value;
    switch_into(value, &caller_context, previous);
    starting_fiber = NULL;

    if (value->state == FIBER_DONE)
        remove_fiber(value);
    return task;
}

value_t *fiber_await_task(const task_ptr &task) {
    if (!task) {
        yyerror("await expects a task");
        return NULL;
    }

    if (current_fiber && current_fiber->canceled) {
        if (!shutting_down)
            yyerror("task was cancelled");
        return NULL;
    }

    if (task_get_state(task) == TASK_STATE_PENDING) {
        if (current_fiber) {
            fiber *value = current_fiber;
            value->await_task = task;
            suspend_current();
            value->await_task = nullptr;
            if (value->canceled) {
                if (!shutting_down)
                    yyerror("task was cancelled");
                return NULL;
            }
        } else {
            if (!event_loop_init()) {
                yyerror("await: cannot initialize event loop");
                return NULL;
            }
            while (task_get_state(task) == TASK_STATE_PENDING) {
                if (g_interrupted)
                    return NULL;
                uv_run(event_loop_handle(), UV_RUN_ONCE);
                fiber_pump();
            }
        }
    }

    return await_result(task);
}

void fiber_pump() {
    for (;;) {
        for (size_t i = 0; i < all_fibers.size(); ++i) {
            fiber *value = all_fibers[i];
            if (value->state != FIBER_WAITING)
                continue;
            if (value->canceled || !value->await_task ||
                task_get_state(value->await_task) != TASK_STATE_PENDING) {
                make_ready(value);
            }
        }

        if (ready_fibers.empty())
            break;

        fiber *value = ready_fibers.front();
        ready_fibers.pop_front();
        value->queued = false;
        if (value->state == FIBER_DONE) {
            remove_fiber(value);
            continue;
        }

        fiber *previous = current_fiber;
        ucontext_t pump_context;
        switch_into(value, &pump_context, previous);
        if (value->state == FIBER_DONE)
            remove_fiber(value);
    }
}

void fiber_shutdown() {
    shutting_down = true;
    for (size_t i = 0; i < all_fibers.size(); ++i) {
        fiber *value = all_fibers[i];
        if (value->state == FIBER_DONE)
            continue;
        value->canceled = true;
        make_ready(value);
    }
    fiber_pump();
    while (!all_fibers.empty()) {
        fiber *value = all_fibers.back();
        all_fibers.pop_back();
        value->canceled = true;
        delete value;
    }
    ready_fibers.clear();
    shutting_down = false;
}

#else

bool fiber_available() {
    return false;
}

task_ptr fiber_start_async(std::function<value_t *()> body) {
    task_ptr task = task_create();
    value_t *result = body ? body() : NULL;
    if (result)
        task_complete(task, result);
    else
        task_fail(task, "async function failed");
    return task;
}

value_t *fiber_await_task(const task_ptr &task) {
    (void)task;
    yyerror("await is not supported on this platform");
    return NULL;
}

void fiber_pump() {}

void fiber_shutdown() {}

#endif
