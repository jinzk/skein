#ifndef SKEIN_TASK_H
#define SKEIN_TASK_H

#include "valuetype.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct task;
using task_ptr = std::shared_ptr<task>;

enum task_state_code {
    TASK_STATE_PENDING,
    TASK_STATE_DONE,
    TASK_STATE_FAILED,
    TASK_STATE_CANCELLED
};

task_ptr task_create();
task_ptr new_sleep_task(long long timeout_ms);
struct value_t *new_task_value(task_ptr task);
void task_set_cancel(const task_ptr &task, std::function<void()> cancel);
void task_complete(const task_ptr &task, struct value_t *result);
void task_fail(const task_ptr &task, const std::string &error);
/* Take ownership of `error`, which should be an error value. */
void task_fail_error(const task_ptr &task, struct value_t *error);
task_state_code task_get_state(const task_ptr &task);
struct value_t *task_copy_result(const task_ptr &task);
const std::string &task_error_message(const task_ptr &task);
/* Return a copy of the structured error, or synthesize one from the message. */
struct value_t *task_error_value(const task_ptr &task);
bool get_task_value(struct value_t *value, task_ptr &task);
bool is_task_value(struct value_t *value);

struct value_t *call_task_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Mark result/error values of every live task on this thread. */
void task_visit_values(void (*visit)(void *, struct value_t *), void *ctx);

/* Structured concurrency bookkeeping for `launch`. */
void task_add_child(const task_ptr &parent, const task_ptr &child);
void task_detach(const task_ptr &task);
bool task_detached(const task_ptr &task);
std::vector<task_ptr> task_children(const task_ptr &task);
void task_cancel_children(const task_ptr &task);
/* Report a detached task failure that no wait/catch/try_wait observed. */
void task_report_unobserved_failure(const task_ptr &task);

void task_shutdown();

#endif
