#include "task.h"

#include "ast.h"
#include "eval.h"
#include "eval_context.h"
#include "event_loop.h"
#include "fiber.h"
#include "repl.h"
#include "valuetype.h"

#include <uv.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum task_status {
    TASK_PENDING,
    TASK_DONE,
    TASK_FAILED,
    TASK_CANCELLED
};

struct task;
std::vector<task *> &task_registry() {
    static std::vector<task *> tasks;
    return tasks;
}

void on_task_timer_close(uv_handle_t *handle) {
    delete (uv_timer_t *)handle;
}

struct task {
    task_status status = TASK_PENDING;
    value_t *result = NULL;
    std::string error;
    value_t *error_value = NULL;
    uv_timer_t *timer = NULL;
    std::function<void()> cancel_operation;
    std::vector<std::function<void()>> waiters;
    std::vector<std::shared_ptr<task>> dependencies;

    task() {
        task_registry().push_back(this);
    }

    ~task() {
        if (status == TASK_PENDING)
            cancel();
        else
            close_timer();
        std::vector<task *> &tasks = task_registry();
        tasks.erase(std::remove(tasks.begin(), tasks.end(), this),
                    tasks.end());
        freeval(result);
        freeval(error_value);
    }

    bool done() const {
        return status != TASK_PENDING;
    }

    void close_timer() {
        if (!timer)
            return;
        uv_timer_t *handle = timer;
        timer = NULL;
        handle->data = NULL;
        uv_timer_stop(handle);
        if (event_loop_initialized())
            uv_close((uv_handle_t *)handle, on_task_timer_close);
        else
            delete handle;
    }

    void notify_waiters() {
        std::vector<std::function<void()>> pending = std::move(waiters);
        waiters.clear();
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i])
                pending[i]();
        }
    }

    void complete(value_t *value) {
        if (status != TASK_PENDING) {
            freeval(value);
            return;
        }
        status = TASK_DONE;
        result = value;
        close_timer();
        cancel_operation = nullptr;
        notify_waiters();
    }

    void fail_error(value_t *value) {
        if (status != TASK_PENDING) {
            freeval(value);
            return;
        }
        status = TASK_FAILED;
        error_value = normalize_error(value, "task");
        error = error_message_of(error_value);
        close_timer();
        cancel_operation = nullptr;
        notify_waiters();
    }

    void fail(const std::string &message) {
        if (status != TASK_PENDING)
            return;
        fail_error(newerrorval("runtime", message));
    }

    bool cancel() {
        if (status != TASK_PENDING)
            return false;
        status = TASK_CANCELLED;
        error = "task was cancelled";
        error_value = newerrorval("cancelled", error);
        close_timer();
        std::function<void()> cancel = std::move(cancel_operation);
        cancel_operation = nullptr;
        if (cancel)
            cancel();
        notify_waiters();
        return true;
    }
};

std::string task_describe(const void *data) {
    const task *value = (const task *)data;
    if (!value)
        return "<task>";
    switch (value->status) {
        case TASK_PENDING:
            return "<task pending>";
        case TASK_DONE:
            return "<task done>";
        case TASK_FAILED:
            return "<task failed>";
        case TASK_CANCELLED:
            return "<task cancelled>";
        default:
            return "<task>";
    }
}

const struct native_type task_type = {
    "task", task_describe
};

value_t *make_task_value(std::shared_ptr<task> value) {
    return newuserdataval(&task_type, value);
}

bool get_task_value(value_t *value, std::shared_ptr<task> &result) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a task, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &task_type) {
        yyerror("expected a task, got %s", typename_of(value));
        return false;
    }
    result = std::static_pointer_cast<task>(userdata->data);
    if (!result) {
        yyerror("invalid task");
        return false;
    }
    return true;
}

void on_task_timer(uv_timer_t *timer) {
    task *value = (task *)timer->data;
    if (!value) {
        uv_close((uv_handle_t *)timer, on_task_timer_close);
        return;
    }
    value->timer = NULL;
    timer->data = NULL;
    uv_timer_stop(timer);
    uv_close((uv_handle_t *)timer, on_task_timer_close);
    value->complete(NULL);
}

std::shared_ptr<task> new_sleep_task(long long timeout_ms) {
    if (!event_loop_init())
        return NULL;

    std::shared_ptr<task> value = std::make_shared<task>();
    value->timer = new uv_timer_t;
    int rc = uv_timer_init(event_loop_handle(), value->timer);
    if (rc < 0) {
        delete value->timer;
        value->timer = NULL;
        return NULL;
    }
    value->timer->data = value.get();
    rc = uv_timer_start(value->timer, on_task_timer,
                        (uint64_t)timeout_ms, 0);
    if (rc < 0) {
        value->close_timer();
        return NULL;
    }
    return value;
}

bool get_optional_timeout(const std::vector<value_t *> &args, size_t index,
                          long long &timeout_ms) {
    timeout_ms = 0;
    if (args.size() <= index)
        return true;
    if (args[index]->type != T_int) {
        yyerror("timeout_ms must be an integer");
        return false;
    }
    long long value = ((struct int_value_t *)args[index])->value;
    if (value < 0) {
        yyerror("timeout_ms must be non-negative");
        return false;
    }
    timeout_ms = value;
    return true;
}

void report_timeout(const char *operation, long long timeout_ms) {
    yyerror("%s: timed out after %lld ms", operation, timeout_ms);
}

value_t *task_sleep(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.task.sleep expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_int ||
        ((struct int_value_t *)args[0])->value < 0) {
        yyerror("task.sleep duration must be a non-negative integer");
        return NULL;
    }
    long long duration = ((struct int_value_t *)args[0])->value;
    std::shared_ptr<task> value = new_sleep_task(duration);
    if (!value) {
        yyerror("task.sleep: cannot create timer");
        return NULL;
    }
    return make_task_value(value);
}

value_t *task_yield(const std::vector<value_t *> &args) {
    if (!args.empty()) {
        yyerror("std.task.yield expects no arguments");
        return NULL;
    }
    std::shared_ptr<task> value = new_sleep_task(0);
    if (!value) {
        yyerror("task.yield: cannot create timer");
        return NULL;
    }
    return make_task_value(value);
}

value_t *task_wait(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.task.wait expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<task> value;
    if (!get_task_value(args[0], value))
        return NULL;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;

    if (!value->done()) {
        if (!event_loop_init()) {
            yyerror("task.wait: cannot initialize event loop");
            return NULL;
        }
        event_timeout timeout = make_event_timeout(timeout_ms);
        while (!value->done()) {
            if (g_interrupted)
                return NULL;
            if (event_timeout_expired(timeout)) {
                report_timeout("task.wait", timeout_ms);
                return NULL;
            }
            uv_run(event_loop_handle(), UV_RUN_ONCE);
            fiber_pump();
        }
    }

    if (value->status == TASK_CANCELLED) {
        eval_raise(newerrorval("cancelled", "task was cancelled"));
        return NULL;
    }
    if (value->status == TASK_FAILED) {
        eval_raise(task_error_value(value));
        return NULL;
    }
    return value->result ? dupval(value->result) : newnull();
}

value_t *task_wait_all(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.task.wait_all expects 1 or 2 arguments");
        return NULL;
    }
    if (args[0]->type != T_list) {
        yyerror("std.task.wait_all expects a list of tasks");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;

    struct list_value_t *values = (struct list_value_t *)args[0];
    std::vector<std::shared_ptr<task>> tasks;
    tasks.reserve(values->payload->items.size());
    for (size_t i = 0; i < values->payload->items.size(); ++i) {
        std::shared_ptr<task> value;
        if (!get_task_value(values->payload->items[i], value))
            return NULL;
        tasks.push_back(std::move(value));
    }

    if (!tasks.empty()) {
        if (!event_loop_init()) {
            yyerror("task.wait_all: cannot initialize event loop");
            return NULL;
        }
        event_timeout timeout = make_event_timeout(timeout_ms);
        for (;;) {
            bool all_done = true;
            for (size_t i = 0; i < tasks.size(); ++i) {
                if (!tasks[i]->done()) {
                    all_done = false;
                    break;
                }
            }
            if (all_done)
                break;
            if (g_interrupted)
                return NULL;
            if (event_timeout_expired(timeout)) {
                report_timeout("task.wait_all", timeout_ms);
                return NULL;
            }
            uv_run(event_loop_handle(), UV_RUN_ONCE);
            fiber_pump();
        }
    }

    struct list_value_t *result = newlistval();
    result->payload->items.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->status == TASK_CANCELLED) {
            freeval((value_t *)result);
            eval_raise(newerrorval("cancelled", "task was cancelled"));
            return NULL;
        }
        if (tasks[i]->status == TASK_FAILED) {
            freeval((value_t *)result);
            eval_raise(task_error_value(tasks[i]));
            return NULL;
        }
        result->payload->items.push_back(
            tasks[i]->result ? dupval(tasks[i]->result) : newnull());
    }
    return (value_t *)result;
}

value_t *task_cancel(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.task.cancel expects 1 argument");
        return NULL;
    }
    std::shared_ptr<task> value;
    if (!get_task_value(args[0], value))
        return NULL;
    return newboolval(value->cancel());
}

value_t *task_done(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.task.done expects 1 argument");
        return NULL;
    }
    std::shared_ptr<task> value;
    if (!get_task_value(args[0], value))
        return NULL;
    return newboolval(value->done());
}

task_ptr task_create() {
    return std::make_shared<task>();
}

value_t *new_task_value(task_ptr value) {
    return make_task_value(std::move(value));
}

void task_set_cancel(const task_ptr &value,
                     std::function<void()> cancel) {
    if (value && value->status == TASK_PENDING)
        value->cancel_operation = std::move(cancel);
}

void task_complete(const task_ptr &value, value_t *result) {
    if (!value) {
        freeval(result);
        return;
    }
    value->complete(result);
}

void task_fail(const task_ptr &value, const std::string &error) {
    if (value)
        value->fail(error);
}

void task_fail_error(const task_ptr &value, value_t *error) {
    if (value)
        value->fail_error(error);
    else
        freeval(error);
}

task_state_code task_get_state(const task_ptr &value) {
    if (!value)
        return TASK_STATE_FAILED;
    switch (value->status) {
        case TASK_PENDING: return TASK_STATE_PENDING;
        case TASK_DONE: return TASK_STATE_DONE;
        case TASK_FAILED: return TASK_STATE_FAILED;
        case TASK_CANCELLED: return TASK_STATE_CANCELLED;
        default: return TASK_STATE_FAILED;
    }
}

value_t *task_copy_result(const task_ptr &value) {
    if (!value || value->status != TASK_DONE)
        return NULL;
    return value->result ? dupval(value->result) : newnull();
}

const std::string &task_error_message(const task_ptr &value) {
    static const std::string empty;
    return value ? value->error : empty;
}

value_t *task_error_value(const task_ptr &value) {
    if (!value)
        return newerrorval("task", "task failed");
    if (value->error_value)
        return dupval(value->error_value);
    if (value->status == TASK_CANCELLED)
        return newerrorval("cancelled", "task was cancelled");
    return newerrorval("runtime",
                       value->error.empty() ? "task failed" : value->error);
}

static void task_add_waiter(const task_ptr &task,
                            std::function<void()> waiter) {
    if (!task)
        return;
    if (task->status != TASK_PENDING) {
        waiter();
        return;
    }
    task->waiters.push_back(std::move(waiter));
}

static value_t *task_as_result(const task_ptr &task) {
    switch (task_get_state(task)) {
        case TASK_STATE_DONE:
            return newresultok(task_copy_result(task));
        case TASK_STATE_FAILED:
            return newresulterr(task_error_value(task));
        case TASK_STATE_CANCELLED:
            return newresulterr(
                newerrorval("cancelled", "task was cancelled"));
        case TASK_STATE_PENDING:
        default:
            return newresulterr(newerrorval("pending", "task is pending"));
    }
}

static value_t *task_catch(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.task.catch expects 1 argument");
        return NULL;
    }
    task_ptr source;
    if (!get_task_value(args[0], source))
        return NULL;

    task_ptr result = task_create();
    result->dependencies.push_back(source);

    if (source->status != TASK_PENDING) {
        task_complete(result, task_as_result(source));
        return new_task_value(std::move(result));
    }

    std::weak_ptr<task> weak_result = result;
    std::weak_ptr<task> weak_source = source;
    task_add_waiter(source, [weak_result, weak_source] {
        task_ptr result = weak_result.lock();
        task_ptr source = weak_source.lock();
        if (result && source)
            task_complete(result, task_as_result(source));
    });
    return new_task_value(std::move(result));
}

static value_t *task_capture_call(
    value_t *(*call)(const std::vector<value_t *> &),
    const std::vector<value_t *> &args) {
    eval_capture_snapshot snapshot;
    eval_capture_begin(snapshot);
    value_t *result = call(args);
    eval_capture_result captured = eval_capture_end(snapshot);

    if (g_interrupted) {
        freeval(result);
        freeval(captured.error);
        return NULL;
    }
    if (captured.failed) {
        freeval(result);
        return newresulterr(captured.error);
    }
    if (active_eval_context().control == EVAL_CONTROL_CLOSE) {
        freeval(result);
        freeval(captured.error);
        return NULL;
    }
    if (!result)
        return newresulterr_message("runtime", "runtime error");
    return newresultok(result);
}

static value_t *task_try_wait(const std::vector<value_t *> &args) {
    return task_capture_call(task_wait, args);
}

static value_t *task_try_wait_all(const std::vector<value_t *> &args) {
    return task_capture_call(task_wait_all, args);
}

void task_shutdown() {
    std::vector<task *> &tasks = task_registry();
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->status == TASK_PENDING)
            tasks[i]->cancel();
    }
}

struct value_t *call_task_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_task_sleep:
            return task_sleep(args);
        case B_task_yield:
            return task_yield(args);
        case B_task_wait:
            return task_wait(args);
        case B_task_wait_all:
            return task_wait_all(args);
        case B_task_cancel:
            return task_cancel(args);
        case B_task_done:
            return task_done(args);
        case B_task_catch:
            return task_catch(args);
        case B_task_try_wait:
            return task_try_wait(args);
        case B_task_try_wait_all:
            return task_try_wait_all(args);
        default:
            yyerror("unknown task function");
            return NULL;
    }
}
