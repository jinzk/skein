#include "task.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "event_loop.h"
#include "fiber.h"
#include "repl.h"
#include "valuetype.h"

#include <uv.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
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
    static thread_local std::vector<task *> tasks;
    return tasks;
}

void on_task_timer_close(uv_handle_t *handle) {
    delete (uv_timer_t *)handle;
}

struct task : std::enable_shared_from_this<task> {
    task_status status = TASK_PENDING;
    value_t *result = NULL;
    std::string error;
    value_t *error_value = NULL;
    uv_timer_t *timer = NULL;
    std::function<void()> cancel_operation;
    std::vector<std::function<void()>> waiters;
    std::vector<std::shared_ptr<task>> dependencies;
    /* Structured concurrency: tasks launched from this task's fiber. */
    std::vector<std::shared_ptr<task>> children;
    std::weak_ptr<task> parent;
    bool detached = false;
    /* A wait/try_wait/catch consumed the outcome of this task. */
    bool observed = false;
    bool failure_reported = false;

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
        for (const task_ptr &child : children) {
            if (child && child->status == TASK_PENDING)
                child->cancel();
        }
        children.clear();
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

value_t *task_get_field(void *data, const char *name) {
    task *raw = static_cast<task *>(data);
    if (!raw || !name)
        return NULL;
    std::shared_ptr<task> task = raw->shared_from_this();
    if (std::strcmp(name, "detach") == 0) {
        return newnativeval([task](const std::vector<value_t *> &) {
            task_detach(task);
            return new_task_value(task);
        });
    }
    if (std::strcmp(name, "detached") == 0) {
        return newnativeval([task](const std::vector<value_t *> &) {
            return newboolval(task_detached(task));
        });
    }
    int functype = 0;
    if (std::strcmp(name, "wait") == 0)
        functype = B_task_wait;
    else if (std::strcmp(name, "try_wait") == 0)
        functype = B_task_try_wait;
    else if (std::strcmp(name, "cancel") == 0)
        functype = B_task_cancel;
    else if (std::strcmp(name, "done") == 0)
        functype = B_task_done;
    else if (std::strcmp(name, "catch") == 0)
        functype = B_task_catch;
    else
        return NULL;
    return newnativeval([task, functype](const std::vector<value_t *> &args) {
        std::vector<value_t *> all;
        all.reserve(args.size() + 1);
        value_t *task_value = new_task_value(task);
        all.push_back(task_value);
        for (value_t *arg : args)
            all.push_back(arg);
        value_t *result = call_task_builtin(functype, all);
        freeval(task_value);
        return result;
    });
}

const struct native_type task_type = {
    "task", task_describe, nullptr, task_get_field
};

value_t *make_task_value(std::shared_ptr<task> value) {
    return newuserdataval(&task_type, value);
}

bool is_task_value(value_t *value) {
    return value && value->type == T_userdata &&
           ((userdata_value_t *)value)->meta == &task_type;
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
    value->observed = true;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;

    if (!value->done()) {
        if (fiber_in_fiber()) {
            /*
             * A task waited on from a fiber must suspend that fiber instead
             * of driving the event loop inline; otherwise sibling tasks and
             * the structured parent scope cannot make progress.
             */
            if (timeout_ms <= 0)
                return fiber_await_task(value);
            auto ready = [value]() { return value->done(); };
            if (!fiber_wait_for(ready, timeout_ms) || !value->done()) {
                if (!value->done())
                    report_timeout("task.wait", timeout_ms);
                return NULL;
            }
        } else {
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
        value->observed = true;
        tasks.push_back(std::move(value));
    }

    if (!tasks.empty()) {
        auto all_done = [&tasks]() {
            for (const std::shared_ptr<task> &item : tasks) {
                if (!item->done())
                    return false;
            }
            return true;
        };
        if (fiber_in_fiber()) {
            if (timeout_ms <= 0) {
                for (const std::shared_ptr<task> &item : tasks) {
                    if (item->done())
                        continue;
                    value_t *result = fiber_await_task(item);
                    if (!result)
                        return NULL;
                    freeval(result);
                }
            } else if (!fiber_wait_for(all_done, timeout_ms) ||
                       !all_done()) {
                if (!all_done())
                    report_timeout("task.wait_all", timeout_ms);
                return NULL;
            }
        } else {
            if (!event_loop_init()) {
                yyerror("task.wait_all: cannot initialize event loop");
                return NULL;
            }
            event_timeout timeout = make_event_timeout(timeout_ms);
            while (!all_done()) {
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
    source->observed = true;

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

static value_t *task_timeout(const std::vector<value_t *> &args) {
    return task_capture_call(task_wait, args);
}

static bool task_sleep_ms(long long ms) {
    if (ms <= 0)
        return true;
    std::shared_ptr<task> timer = new_sleep_task(ms);
    if (!timer) {
        yyerror("task.sleep: cannot create timer");
        return false;
    }
    if (!event_loop_init()) {
        yyerror("task: cannot initialize event loop");
        return false;
    }
    while (!timer->done()) {
        if (g_interrupted)
            return false;
        uv_run(event_loop_handle(), UV_RUN_ONCE);
        fiber_pump();
    }
    return true;
}

static bool task_wait_done(const std::shared_ptr<task> &value) {
    if (!value)
        return false;
    if (value->done())
        return true;
    if (!event_loop_init()) {
        yyerror("task: cannot initialize event loop");
        return false;
    }
    while (!value->done()) {
        if (g_interrupted)
            return false;
        uv_run(event_loop_handle(), UV_RUN_ONCE);
        fiber_pump();
    }
    return true;
}

static struct value_t *task_dict_lookup(struct dict_value_t *dict,
                                        const char *key) {
    if (!dict || !key)
        return NULL;
    for (auto &entry : dict->payload->items) {
        if (entry.first && entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key) == 0)
            return entry.second;
    }
    return NULL;
}

static bool task_number_value(struct value_t *value, double &out,
                              const char *name) {
    if (!value || !is_numeric(value)) {
        yyerror("%s must be a number", name);
        return false;
    }
    out = as_double(value);
    return true;
}

static bool task_integer_value(struct value_t *value, long long &out,
                               const char *name) {
    if (!value || value->type != T_int) {
        yyerror("%s must be an integer", name);
        return false;
    }
    out = ((struct int_value_t *)value)->value;
    return true;
}

static bool task_callable_value(struct value_t *value, const char *name) {
    if (!value ||
        (value->type != T_func && value->type != T_builtin &&
         value->type != T_method)) {
        yyerror("%s must be callable", name);
        return false;
    }
    return true;
}

static value_t *task_retry(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.task.retry expects 1 or 2 arguments");
        return NULL;
    }
    if (!task_callable_value(args[0], "std.task.retry factory"))
        return NULL;

    long long attempts = 3;
    long long delay_ms = 0;
    long long max_delay_ms = 0;
    double backoff = 2.0;
    std::vector<std::string> retry_on;

    if (args.size() == 2) {
        if (args[1]->type != T_dict) {
            yyerror("std.task.retry options must be a dict");
            return NULL;
        }
        struct dict_value_t *options = (struct dict_value_t *)args[1];
        if (struct value_t *value = task_dict_lookup(options, "attempts")) {
            if (!task_integer_value(value, attempts,
                                    "std.task.retry attempts"))
                return NULL;
        }
        if (struct value_t *value = task_dict_lookup(options, "delay_ms")) {
            if (!task_integer_value(value, delay_ms,
                                    "std.task.retry delay_ms"))
                return NULL;
        }
        if (struct value_t *value = task_dict_lookup(options, "max_delay_ms")) {
            if (!task_integer_value(value, max_delay_ms,
                                    "std.task.retry max_delay_ms"))
                return NULL;
        }
        if (struct value_t *value = task_dict_lookup(options, "backoff")) {
            if (!task_number_value(value, backoff,
                                   "std.task.retry backoff"))
                return NULL;
        }
        if (struct value_t *value = task_dict_lookup(options, "retry_on")) {
            if (value->type == T_string) {
                retry_on.push_back(
                    ((struct string_value_t *)value)->value);
            } else if (value->type == T_list) {
                struct list_value_t *list = (struct list_value_t *)value;
                for (struct value_t *item : list->payload->items) {
                    if (item->type != T_string) {
                        yyerror("std.task.retry retry_on entries must be strings");
                        return NULL;
                    }
                    retry_on.push_back(
                        ((struct string_value_t *)item)->value);
                }
            } else {
                yyerror("std.task.retry retry_on must be a string or list");
                return NULL;
            }
        }
    }

    if (attempts < 1) {
        yyerror("std.task.retry attempts must be at least 1");
        return NULL;
    }
    if (delay_ms < 0 || max_delay_ms < 0) {
        yyerror("std.task.retry delays must be non-negative");
        return NULL;
    }
    if (backoff < 1.0) {
        yyerror("std.task.retry backoff must be at least 1.0");
        return NULL;
    }

    for (long long attempt = 0; attempt < attempts; ++attempt) {
        value_t *produced = call_value_args(args[0], {});
        if (!produced)
            return NULL;

        value_t *result = NULL;
        if (is_task_value(produced)) {
            std::shared_ptr<task> value;
            if (!get_task_value(produced, value)) {
                freeval(produced);
                return NULL;
            }
            freeval(produced);
            if (!task_wait_done(value))
                return NULL;
            result = task_as_result(value);
        } else if (produced->type == T_result) {
            result = produced;
        } else {
            result = newresultok(produced);
        }

        if (!result)
            return NULL;
        if (result->type != T_result) {
            freeval(result);
            return newresulterr_message(
                "retry", "factory returned an invalid result");
        }
        struct result_value_t *result_value =
            (struct result_value_t *)result;
        if (result_value->ok)
            return result;

        std::string kind = error_kind_of(result_value->error);
        bool retryable = retry_on.empty();
        for (const std::string &allowed : retry_on) {
            if (allowed == kind) {
                retryable = true;
                break;
            }
        }
        if (!retryable || attempt + 1 >= attempts)
            return result;

        freeval(result);
        if (!task_sleep_ms(delay_ms))
            return NULL;

        double next = (double)delay_ms * backoff;
        if (max_delay_ms > 0 && next > (double)max_delay_ms)
            next = (double)max_delay_ms;
        delay_ms = (long long)next;
    }

    return newresulterr_message("retry", "retry failed");
}

struct rate_limiter {
    double rate = 1.0;
    double burst = 1.0;
    double tokens = 1.0;
    std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
};

static std::string rate_limiter_describe(const void *data) {
    const rate_limiter *limiter = (const rate_limiter *)data;
    if (!limiter)
        return "<rate_limiter>";
    return "<rate_limiter rate=" + std::to_string(limiter->rate) +
           " burst=" + std::to_string(limiter->burst) + ">";
}

static const struct native_type rate_limiter_type = {
    "rate_limiter", rate_limiter_describe
};

static value_t *task_rate_limit(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.task.rate_limit expects 2 arguments");
        return NULL;
    }
    double rate = 0.0;
    double burst = 0.0;
    if (!task_number_value(args[0], rate, "std.task.rate_limit rate"))
        return NULL;
    if (!task_number_value(args[1], burst, "std.task.rate_limit burst"))
        return NULL;
    if (rate <= 0.0) {
        yyerror("std.task.rate_limit rate must be positive");
        return NULL;
    }
    if (burst < 1.0) {
        yyerror("std.task.rate_limit burst must be at least 1");
        return NULL;
    }

    std::shared_ptr<rate_limiter> limiter =
        std::make_shared<rate_limiter>();
    limiter->rate = rate;
    limiter->burst = burst;
    limiter->tokens = burst;
    limiter->last = std::chrono::steady_clock::now();
    return newuserdataval(&rate_limiter_type, limiter);
}

static void rate_limiter_refill(const std::shared_ptr<rate_limiter> &limiter) {
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - limiter->last).count();
    limiter->last = now;
    if (elapsed <= 0.0)
        return;
    limiter->tokens =
        std::min(limiter->burst, limiter->tokens + elapsed * limiter->rate);
}

static bool get_rate_limiter(value_t *value,
                             std::shared_ptr<rate_limiter> &out) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a rate_limiter, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &rate_limiter_type) {
        yyerror("expected a rate_limiter, got %s", typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<rate_limiter>(userdata->data);
    if (!out) {
        yyerror("invalid rate_limiter");
        return false;
    }
    return true;
}

static value_t *task_acquire(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.task.acquire expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<rate_limiter> limiter;
    if (!get_rate_limiter(args[0], limiter))
        return NULL;

    long long tokens = 1;
    if (args.size() == 2 &&
        !task_integer_value(args[1], tokens, "std.task.acquire tokens"))
        return NULL;
    if (tokens < 1) {
        yyerror("std.task.acquire tokens must be at least 1");
        return NULL;
    }

    for (;;) {
        rate_limiter_refill(limiter);
        if (limiter->tokens >= (double)tokens) {
            limiter->tokens -= (double)tokens;
            return newboolval(true);
        }
        double missing = (double)tokens - limiter->tokens;
        double wait_seconds = missing / limiter->rate;
        long long wait_ms = (long long)std::ceil(wait_seconds * 1000.0);
        if (wait_ms < 1)
            wait_ms = 1;
        if (!task_sleep_ms(wait_ms))
            return NULL;
    }
}

static value_t *task_bounded_parallel_map(
    const std::vector<value_t *> &args) {
    if (args.size() != 3) {
        yyerror("std.task.bounded_parallel_map expects 3 arguments");
        return NULL;
    }
    if (args[0]->type != T_list) {
        yyerror("std.task.bounded_parallel_map expects a list, got %s",
                typename_of(args[0]));
        return NULL;
    }
    long long limit = 0;
    if (!task_integer_value(args[1], limit,
                            "std.task.bounded_parallel_map limit"))
        return NULL;
    if (limit < 1) {
        yyerror("std.task.bounded_parallel_map limit must be at least 1");
        return NULL;
    }
    if (!task_callable_value(args[2],
                             "std.task.bounded_parallel_map callback"))
        return NULL;

    struct list_value_t *items = (struct list_value_t *)args[0];
    size_t count = items->payload->items.size();
    std::vector<value_t *> results(count, NULL);
    std::vector<task_ptr> active;
    std::vector<size_t> active_index;
    size_t next = 0;

    auto start_ready = [&]() -> bool {
        while (active.size() < (size_t)limit && next < count) {
            std::vector<value_t *> call_args;
            call_args.push_back(dupval(items->payload->items[next]));
            value_t *produced =
                call_value_args(args[2], std::move(call_args));
            if (!produced) {
                for (value_t *value : results)
                    freeval(value);
                return false;
            }
            if (is_task_value(produced)) {
                task_ptr value;
                if (!get_task_value(produced, value)) {
                    freeval(produced);
                    for (value_t *result : results)
                        freeval(result);
                    return false;
                }
                freeval(produced);
                /* The map owns the task and converts its outcome to result. */
                value->observed = true;
                active.push_back(std::move(value));
                active_index.push_back(next);
            } else if (produced->type == T_result) {
                results[next] = produced;
            } else {
                results[next] = newresultok(produced);
            }
            ++next;
        }
        return true;
    };

    if (!start_ready())
        return NULL;

    while (!active.empty()) {
        bool progressed = false;
        for (size_t i = 0; i < active.size();) {
            if (active[i]->done()) {
                results[active_index[i]] = task_as_result(active[i]);
                active.erase(active.begin() + i);
                active_index.erase(active_index.begin() + i);
                progressed = true;
            } else {
                ++i;
            }
        }
        if (!start_ready()) {
            for (value_t *value : results)
                freeval(value);
            return NULL;
        }
        if (active.empty())
            break;
        if (progressed)
            continue;
        if (g_interrupted) {
            for (value_t *value : results)
                freeval(value);
            return NULL;
        }
        if (!event_loop_init()) {
            yyerror("task: cannot initialize event loop");
            for (value_t *value : results)
                freeval(value);
            return NULL;
        }
        uv_run(event_loop_handle(), UV_RUN_ONCE);
        fiber_pump();
    }

    struct list_value_t *out = newlistval();
    out->payload->items.reserve(count);
    for (size_t i = 0; i < count; ++i)
        out->payload->items.push_back(
            results[i] ? results[i] : newresulterr_message(
                "task", "missing task result"));
    return (value_t *)out;
}

void task_shutdown() {
    std::vector<task *> &tasks = task_registry();
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->status == TASK_PENDING)
            tasks[i]->cancel();
    }
}

void task_visit_values(void (*visit)(void *, struct value_t *), void *ctx) {
    if (!visit)
        return;
    for (task *value : task_registry()) {
        if (!value)
            continue;
        if (value->result)
            visit(ctx, value->result);
        if (value->error_value)
            visit(ctx, value->error_value);
    }
}

void task_add_child(const task_ptr &parent, const task_ptr &child) {
    if (!child)
        return;
    if (!parent) {
        child->detached = true;
        return;
    }
    if (parent->status == TASK_CANCELLED) {
        child->detached = true;
        child->cancel();
        return;
    }
    child->parent = parent;
    parent->children.push_back(child);
}

void task_detach(const task_ptr &task) {
    if (!task || task->detached)
        return;
    task->detached = true;
    task_ptr parent = task->parent.lock();
    if (parent) {
        std::vector<task_ptr> &children = parent->children;
        children.erase(
            std::remove(children.begin(), children.end(), task),
            children.end());
    }
    task->parent.reset();
    task_report_unobserved_failure(task);
}

bool task_detached(const task_ptr &task) {
    return !task || task->detached;
}

void task_report_unobserved_failure(const task_ptr &task) {
    if (!task || !task->detached || task->observed ||
        task->failure_reported || task->status != TASK_FAILED)
        return;
    task->failure_reported = true;
    /*
     * Server/worker teardown can fail many detached tasks with the same
     * message.  Report each distinct failure once so diagnostics stay useful
     * without flooding stderr (which can block a piped parent process).
     */
    static thread_local std::set<std::string> reported;
    const std::string &error = task_error_message(task);
    if (!reported.insert(error).second)
        return;
    std::string message = "detached task failed: ";
    message += error;
    diag_report_warning(0, 0, "detached task", message.c_str());
}

std::vector<task_ptr> task_children(const task_ptr &task) {
    if (!task)
        return {};
    return task->children;
}

void task_cancel_children(const task_ptr &task) {
    if (!task)
        return;
    for (const task_ptr &child : task->children) {
        if (child && child->status == TASK_PENDING)
            child->cancel();
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
        case B_task_retry:
            return task_retry(args);
        case B_task_timeout:
            return task_timeout(args);
        case B_task_rate_limit:
            return task_rate_limit(args);
        case B_task_acquire:
            return task_acquire(args);
        case B_task_bounded_parallel_map:
            return task_bounded_parallel_map(args);
        default:
            yyerror("unknown task function");
            return NULL;
    }
}
