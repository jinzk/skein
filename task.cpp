#include "task.h"

#include "ast.h"
#include "event_loop.h"
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
    uv_timer_t *timer = NULL;
    std::function<void()> cancel_operation;

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

    void complete(value_t *value) {
        if (status != TASK_PENDING) {
            freeval(value);
            return;
        }
        status = TASK_DONE;
        result = value;
        close_timer();
        cancel_operation = nullptr;
    }

    void fail(const std::string &message) {
        if (status != TASK_PENDING)
            return;
        status = TASK_FAILED;
        error = message;
        close_timer();
        cancel_operation = nullptr;
    }

    bool cancel() {
        if (status != TASK_PENDING)
            return false;
        status = TASK_CANCELLED;
        error = "task was cancelled";
        close_timer();
        std::function<void()> cancel = std::move(cancel_operation);
        cancel_operation = nullptr;
        if (cancel)
            cancel();
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

bool get_task(value_t *value, std::shared_ptr<task> &result) {
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
    if (!get_task(args[0], value))
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
        }
    }

    if (value->status == TASK_CANCELLED) {
        yyerror("task was cancelled");
        return NULL;
    }
    if (value->status == TASK_FAILED) {
        yyerror("task failed: %s", value->error.c_str());
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
        if (!get_task(values->payload->items[i], value))
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
        }
    }

    struct list_value_t *result = newlistval();
    result->payload->items.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->status == TASK_CANCELLED) {
            freeval((value_t *)result);
            yyerror("task was cancelled");
            return NULL;
        }
        if (tasks[i]->status == TASK_FAILED) {
            freeval((value_t *)result);
            yyerror("task failed: %s", tasks[i]->error.c_str());
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
    if (!get_task(args[0], value))
        return NULL;
    return newboolval(value->cancel());
}

value_t *task_done(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.task.done expects 1 argument");
        return NULL;
    }
    std::shared_ptr<task> value;
    if (!get_task(args[0], value))
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
        default:
            yyerror("unknown task function");
            return NULL;
    }
}
