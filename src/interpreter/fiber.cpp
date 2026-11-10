#include "fiber.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval.h"
#include "eval_context.h"
#include "event_loop.h"
#include "repl.h"
#include "runtime_context.h"
#include "scopestack.h"
#include "task.h"
#include "valuetype.h"

#include <boost/context/fiber.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kDefaultFiberStackBytes = 1024 * 1024;
constexpr size_t kFiberStackMarginBytes = 16 * 1024;
constexpr size_t kMaxPooledFiberStacks = 32;

size_t fiber_stack_bytes() {
    const char *configured = std::getenv("SKEIN_FIBER_STACK_KB");
    if (!configured || !*configured)
        return kDefaultFiberStackBytes;
    char *end = NULL;
    unsigned long kb = std::strtoul(configured, &end, 10);
    if (end == configured || *end != '\0' || kb < 64 || kb > 1024 * 1024)
        return kDefaultFiberStackBytes;
    return static_cast<size_t>(kb) * 1024;
}

bool fiber_stack_pool_enabled() {
    const char *configured = std::getenv("SKEIN_FIBER_STACK_POOL");
    if (!configured || !*configured)
        return true;
    return !(std::strcmp(configured, "0") == 0 ||
             std::strcmp(configured, "false") == 0 ||
             std::strcmp(configured, "off") == 0 ||
             std::strcmp(configured, "no") == 0);
}

struct fiber_stack_bounds {
    char *low = nullptr;
    char *high = nullptr;
    size_t usable = 0;
};

thread_local fiber_stack_bounds last_stack_bounds;

struct fiber_stack_pool {
    std::vector<boost::context::stack_context> free;
    size_t allocated = 0;
    size_t reused = 0;
    bool enabled = fiber_stack_pool_enabled();

    ~fiber_stack_pool() { clear(); }

    void clear() {
        boost::context::protected_fixedsize_stack allocator;
        for (boost::context::stack_context &context : free)
            allocator.deallocate(context);
        free.clear();
    }
};

thread_local fiber_stack_pool fiber_stacks;

fiber_stack_bounds bounds_of(const boost::context::stack_context &context) {
    fiber_stack_bounds bounds;
    const size_t guard =
        boost::context::stack_traits::page_size();
    if (!context.sp || context.size <= guard)
        return bounds;
    bounds.high = static_cast<char *>(context.sp);
    bounds.low = bounds.high - context.size + guard;
    bounds.usable = static_cast<size_t>(bounds.high - bounds.low);
    return bounds;
}

/*
 * Guard-paged stacks from Boost.Context with a per-thread free list.  Fiber
 * creation is dominated by mmap/VirtualAlloc; reusing stacks keeps short-lived
 * async tasks and generators cheap.
 */
class pooled_fiber_stack {
public:
    typedef boost::context::stack_traits traits_type;

    explicit pooled_fiber_stack(std::size_t size) : size_(size) {}

    boost::context::stack_context allocate() {
        if (fiber_stacks.enabled && !fiber_stacks.free.empty()) {
            boost::context::stack_context context =
                fiber_stacks.free.back();
            fiber_stacks.free.pop_back();
            ++fiber_stacks.reused;
            last_stack_bounds = bounds_of(context);
            return context;
        }
        boost::context::protected_fixedsize_stack allocator(size_);
        boost::context::stack_context context = allocator.allocate();
        ++fiber_stacks.allocated;
        last_stack_bounds = bounds_of(context);
        return context;
    }

    void deallocate(boost::context::stack_context &context) noexcept {
        if (fiber_stacks.enabled &&
            fiber_stacks.free.size() < kMaxPooledFiberStacks) {
            try {
                fiber_stacks.free.push_back(context);
                context.sp = nullptr;
                return;
            } catch (...) {
                /* Fall through and release the stack directly. */
            }
        }
        boost::context::protected_fixedsize_stack allocator(size_);
        allocator.deallocate(context);
    }

private:
    std::size_t size_;
};

uintptr_t current_stack_pointer() {
#if defined(__GNUC__) || defined(__clang__)
    return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
#else
    return 0;
#endif
}

enum fiber_state {
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_WAITING,
    FIBER_YIELDED,
    FIBER_DONE
};

enum fiber_kind {
    FIBER_TASK,
    FIBER_GENERATOR
};

struct fiber {
    boost::context::fiber context;
    boost::context::fiber caller;
    eval_context eval;
    diag_state diagnostics;
    std::function<value_t *()> body;
    task_ptr task;
    task_ptr await_task;
    std::function<bool()> wait_ready;
    fiber_state state = FIBER_READY;
    fiber_kind kind = FIBER_TASK;
    value_t *yielded_value = NULL;
    value_t *final_value = NULL;
    value_t *error_value = NULL;
    value_t *resume_value = NULL;
    bool failed = false;
    bool canceled = false;
    bool closing = false;
    bool queued = false;
    char *stack_low = nullptr;
    size_t stack_usable = 0;
    std::string stack_error;

    fiber(std::function<value_t *()> fn, fiber_kind fiber_type)
        : body(std::move(fn)), kind(fiber_type) {
        context = boost::context::fiber(
            std::allocator_arg,
            pooled_fiber_stack(fiber_stack_bytes()),
            [this](boost::context::fiber &&sink) {
                caller = std::move(sink);
                fiber_main();
                return std::move(caller);
            });
        stack_low = last_stack_bounds.low;
        stack_usable = last_stack_bounds.usable;
    }

    ~fiber() {
        context = boost::context::fiber();
        caller = boost::context::fiber();
        freeval(resume_value);
        resume_value = NULL;
        freeval(error_value);
        error_value = NULL;
    }

    bool check_stack(const char *where);
    value_t *raise_stack_error();

    void fiber_main();
};

thread_local fiber *current_fiber = NULL;
thread_local eval_context *root_context = NULL;
thread_local bool shutting_down = false;
thread_local bool generator_warnings_enabled = true;
thread_local std::deque<fiber *> ready_fibers;
thread_local std::vector<fiber *> all_fibers;

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

bool fiber_wait_satisfied(fiber *value) {
    if (!value)
        return true;
    if (value->canceled)
        return true;
    bool waiting = false;
    if (value->await_task) {
        waiting = true;
        if (task_get_state(value->await_task) != TASK_STATE_PENDING)
            return true;
    }
    if (value->wait_ready) {
        waiting = true;
        if (value->wait_ready())
            return true;
    }
    return !waiting;
}

void switch_into(fiber *value, fiber *previous) {
    if (!previous && !root_context)
        root_context = &active_eval_context();

    diag_state previous_diagnostics;
    diag_capture(previous_diagnostics);
    diag_restore(value->diagnostics);
    set_active_eval_context(&value->eval);
    current_fiber = value;
    value->state = FIBER_RUNNING;
    value->context = std::move(value->context).resume();

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
    if (!value)
        return;
    value->state = FIBER_WAITING;
    value->caller = std::move(value->caller).resume();
    value->state = FIBER_RUNNING;
}

value_t *take_fiber_error(fiber *value, const char *fallback) {
    value_t *error = NULL;
    if (value->eval.control == EVAL_CONTROL_FAIL) {
        error = value->eval.control_value;
        value->eval.control_value = NULL;
        value->eval.control = EVAL_CONTROL_NONE;
    }
    if (!error) {
        if (value->eval.has_captured_error)
            error = newerrorval("runtime", value->eval.captured_error);
        else
            error = newerrorval("runtime", fallback);
    }
    return normalize_error(error, "runtime");
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
            eval_raise(task_error_value(task));
            return NULL;
        case TASK_STATE_CANCELLED:
            eval_raise(newerrorval("cancelled", "task was cancelled"));
            return NULL;
        case TASK_STATE_PENDING:
        default:
            return NULL;
    }
}

} // namespace

bool fiber::check_stack(const char *where) {
    if (!stack_low || stack_usable == 0)
        return true;
    const uintptr_t low = reinterpret_cast<uintptr_t>(stack_low);
    const uintptr_t high = low + stack_usable;
    const uintptr_t sp = current_stack_pointer();
    if (sp < low || sp > high)
        return true;
    const size_t remaining = static_cast<size_t>(sp - low);
    if (remaining >= kFiberStackMarginBytes)
        return true;
    char message[256];
    std::snprintf(
        message, sizeof(message),
        "fiber stack overflow at %s: %zu of %zu bytes left "
        "(raise SKEIN_FIBER_STACK_KB)", where, remaining, stack_usable);
    stack_error = message;
    return false;
}

value_t *fiber::raise_stack_error() {
    eval_raise(newerrorval("fiber_stack", stack_error.c_str()));
    return NULL;
}

void fiber::fiber_main() {
    if (!check_stack("fiber start")) {
        body = nullptr;
        value_t *error = newerrorval("fiber_stack", stack_error.c_str());
        if (kind == FIBER_TASK) {
            task_fail_error(task, error);
        } else {
            failed = true;
            error_value = error;
        }
        if (root_context)
            adopt_scopes(eval.scopes, root_context->scopes);
        state = FIBER_DONE;
        return;
    }
    value_t *result = body ? body() : NULL;
    body = nullptr;

    if (kind == FIBER_GENERATOR) {
        bool closed = eval.control == EVAL_CONTROL_CLOSE ||
                      (closing && eval.control == EVAL_CONTROL_NONE);
        if (closed) {
            freeval(result);
            if (eval.control != EVAL_CONTROL_NONE) {
                eval.control = EVAL_CONTROL_NONE;
                freeval(eval.control_value);
                eval.control_value = NULL;
            }
        } else if (canceled) {
            freeval(result);
        } else if (!result) {
            failed = true;
            error_value = take_fiber_error(this, "generator failed");
        } else {
            final_value = result;
        }
    } else {
        if (canceled) {
            freeval(result);
        } else if (!result) {
            task_fail_error(task,
                            take_fiber_error(this, "async function failed"));
        } else {
            task_complete(task, result);
        }
    }

    if (root_context)
        adopt_scopes(eval.scopes, root_context->scopes);
    state = FIBER_DONE;
}

bool fiber_available() {
    return true;
}

bool fiber_in_fiber() {
    return current_fiber != nullptr;
}

task_ptr fiber_start_async(std::function<value_t *()> body) {
    task_ptr task = task_create();
    fiber *value = new fiber(std::move(body), FIBER_TASK);
    value->task = task;
    diag_capture(value->diagnostics);
    value->eval.scopes.global_scope =
        active_eval_context().scopes.global_scope;
    value->eval.capture_errors = true;
    /* Match the script root context so defer's scope check is uniform. */
    value->eval.defer_stack.emplace_back();

    task_set_cancel(task, [value] {
        if (value->state == FIBER_DONE)
            return;
        value->canceled = true;
        if (value->state == FIBER_WAITING)
            make_ready(value);
    });

    all_fibers.push_back(value);

    fiber *previous = current_fiber;
    switch_into(value, previous);

    if (value->state == FIBER_DONE)
        remove_fiber(value);
    return task;
}

struct generator {
    fiber *value = NULL;
    bool started = false;
    bool done = false;

    ~generator() {
        if (value) {
            if (generator_warnings_enabled && !shutting_down &&
                started && !done && value->state == FIBER_YIELDED) {
                bool has_defers = value->eval.vm_defer_count > 0;
                for (size_t i = 0;
                     !has_defers && i < value->eval.defer_stack.size(); ++i) {
                    if (!value->eval.defer_stack[i].empty()) {
                        has_defers = true;
                        break;
                    }
                }
                if (has_defers) {
                    diag_warning(
                        "suspended generator dropped without close; "
                        "deferred cleanup skipped "
                        "(use std.generator.close or defer close)");
                }
            }
            if (root_context)
                adopt_scopes(value->eval.scopes, root_context->scopes);
            delete value;
        }
    }
};

namespace {

std::string generator_describe(const void *data) {
    const generator *value = (const generator *)data;
    if (!value)
        return "<generator>";
    return value->done ? "<generator done>" : "<generator>";
}

const struct native_type generator_type = {
    "generator", generator_describe
};

} // namespace

generator_ptr generator_create(std::function<value_t *()> body) {
    if (!fiber_available())
        return NULL;
    fiber *value = new fiber(std::move(body), FIBER_GENERATOR);
    value->eval.scopes.global_scope =
        active_eval_context().scopes.global_scope;
    value->eval.capture_errors = true;
    value->eval.defer_stack.emplace_back();
    diag_capture(value->diagnostics);
    generator_ptr result = std::make_shared<generator>();
    result->value = value;
    return result;
}

value_t *new_generator_value(generator_ptr value) {
    return newuserdataval(&generator_type, std::move(value));
}

bool get_generator_value(value_t *value, generator_ptr &generator) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a generator, got %s", typename_of(value));
        return false;
    }
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (userdata->meta != &generator_type) {
        yyerror("expected a generator, got %s", typename_of(value));
        return false;
    }
    generator = std::static_pointer_cast<::generator>(userdata->data);
    if (!generator) {
        yyerror("invalid generator");
        return false;
    }
    return true;
}

bool generator_next_value(const generator_ptr &generator,
                          value_t *send_value,
                          value_t *&value, bool &done) {
    value = NULL;
    done = false;
    if (!generator || generator->done || !generator->value) {
        freeval(send_value);
        done = true;
        value = newnull();
        return true;
    }

    if (!generator->started) {
        freeval(send_value);
        send_value = NULL;
        generator->started = true;
    }

    fiber *current = generator->value;
    current->resume_value = send_value;
    fiber *previous = current_fiber;
    switch_into(current, previous);

    /*
     * An async generator may suspend on an awaited task before yielding or
     * finishing.  Generator fibers are not in the task pump's ready list, so
     * resume this one directly once its awaited task completes.
     */
    while (current->state == FIBER_WAITING) {
        if (!event_loop_init())
            break;
        uv_run(event_loop_handle(), UV_RUN_ONCE);
        fiber_pump();
        if (fiber_wait_satisfied(current)) {
            fiber *caller = current_fiber;
            switch_into(current, caller);
        }
    }

    if (current->state == FIBER_YIELDED) {
        value = current->yielded_value;
        current->yielded_value = NULL;
        if (!value)
            value = newnull();
        return true;
    }

    done = true;
    generator->done = true;
    bool failed = current->failed;
    value = current->final_value;
    current->final_value = NULL;
    if (current->resume_value) {
        freeval(current->resume_value);
        current->resume_value = NULL;
    }
    value_t *error_value = current->error_value;
    current->error_value = NULL;
    delete current;
    generator->value = NULL;
    if (failed) {
        freeval(value);
        if (error_value) {
            eval_raise(error_value);
        } else {
            yyerror("generator failed");
        }
        return false;
    }
    if (!value)
        value = newnull();
    return true;
}

bool generator_close(const generator_ptr &generator) {
    if (!generator || generator->done || !generator->value)
        return false;

    fiber *current = generator->value;
    if (current == current_fiber) {
        yyerror("cannot close a running generator");
        return false;
    }

    if (!generator->started) {
        /* The body never ran, so there are no defers to execute. */
        delete current;
        generator->value = NULL;
        generator->done = true;
        return true;
    }

    /* Let an async generator finish its current await before closing. */
    current->closing = true;
    freeval(current->resume_value);
    current->resume_value = NULL;
    while (current->state == FIBER_WAITING) {
        if (!event_loop_init())
            break;
        uv_run(event_loop_handle(), UV_RUN_ONCE);
        fiber_pump();
        if (fiber_wait_satisfied(current)) {
            fiber *caller = current_fiber;
            switch_into(current, caller);
        }
    }

    fiber *previous = current_fiber;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (current->state != FIBER_YIELDED)
            break;
        current->closing = true;
        switch_into(current, previous);
    }

    if (current->state != FIBER_DONE) {
        yyerror("generator did not close");
        return false;
    }

    bool failed = current->failed;
    value_t *close_error = current->error_value;
    current->error_value = NULL;
    freeval(current->yielded_value);
    current->yielded_value = NULL;
    freeval(current->final_value);
    current->final_value = NULL;
    delete current;
    generator->value = NULL;
    generator->done = true;

    if (failed) {
        if (!close_error)
            close_error = newerrorval("runtime", "generator close failed");
        eval_raise(close_error);
    } else {
        freeval(close_error);
    }
    return true;
}

bool generator_done(const generator_ptr &generator) {
    return !generator || generator->done;
}

bool fiber_is_generator(value_t *value) {
    return value && value->type == T_userdata &&
           ((userdata_value_t *)value)->meta == &generator_type;
}

value_t *fiber_yield_value(value_t *value) {
    fiber *current = current_fiber;
    if (!current || current->kind != FIBER_GENERATOR) {
        yyerror("yield outside generator");
        freeval(value);
        return NULL;
    }
    if (current->closing) {
        freeval(value);
        eval_request_close();
        return NULL;
    }
    if (!current->check_stack("yield")) {
        freeval(value);
        return current->raise_stack_error();
    }
    current->yielded_value = value;
    current->state = FIBER_YIELDED;
    current->caller = std::move(current->caller).resume();
    current->state = FIBER_RUNNING;

    value_t *sent = current->resume_value;
    current->resume_value = NULL;
    if (current->closing) {
        freeval(sent);
        eval_request_close();
        return NULL;
    }
    return sent ? sent : newnull();
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
            if (!value->check_stack("await")) {
                value->raise_stack_error();
                return NULL;
            }
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

bool fiber_wait_for(std::function<bool()> ready, long long timeout_ms) {
    fiber *value = current_fiber;
    if (!value) {
        if (!ready || ready())
            return true;
        return false;
    }
    if (value->canceled)
        return false;
    if (!value->check_stack("wait")) {
        value->raise_stack_error();
        return false;
    }

    task_ptr timer;
    if (timeout_ms > 0)
        timer = new_sleep_task(timeout_ms);

    value->wait_ready = std::move(ready);
    value->await_task = timer;
    suspend_current();
    value->wait_ready = nullptr;
    value->await_task = nullptr;
    bool canceled = value->canceled;
    return !canceled;
}

void fiber_pump() {
    struct skein_context *runtime = skein_context_current();
    if (runtime && runtime->drain_background)
        runtime->drain_background();
    for (;;) {
        for (size_t i = 0; i < all_fibers.size(); ++i) {
            fiber *value = all_fibers[i];
            if (value->state != FIBER_WAITING)
                continue;
            if (fiber_wait_satisfied(value))
                make_ready(value);
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
        switch_into(value, previous);
        if (value->state == FIBER_DONE)
            remove_fiber(value);
    }
}

void fiber_disable_generator_warnings() {
    generator_warnings_enabled = false;
}

void fiber_shutdown() {
    shutting_down = true;
    generator_warnings_enabled = false;
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
    if (const char *stats = std::getenv("SKEIN_FIBER_STACK_STATS")) {
        if (*stats && std::strcmp(stats, "0") != 0) {
            std::fprintf(stderr,
                         "fiber stack pool: allocated=%zu reused=%zu "
                         "pooled=%zu\n",
                         fiber_stacks.allocated, fiber_stacks.reused,
                         fiber_stacks.free.size());
        }
    }
    fiber_stacks.clear();
    shutting_down = false;
}

void fiber_stack_pool_stats(size_t &allocated, size_t &reused,
                            size_t &pooled) {
    allocated = fiber_stacks.allocated;
    reused = fiber_stacks.reused;
    pooled = fiber_stacks.free.size();
}
