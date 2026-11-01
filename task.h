#ifndef CALC_TASK_H
#define CALC_TASK_H

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

struct value_t *call_task_builtin(
    int functype, const std::vector<struct value_t *> &args);

void task_shutdown();

#endif
