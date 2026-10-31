#ifndef CALC_TASK_H
#define CALC_TASK_H

#include "valuetype.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct task;
using task_ptr = std::shared_ptr<task>;

task_ptr task_create();
struct value_t *new_task_value(task_ptr task);
void task_set_cancel(const task_ptr &task, std::function<void()> cancel);
void task_complete(const task_ptr &task, struct value_t *result);
void task_fail(const task_ptr &task, const std::string &error);

struct value_t *call_task_builtin(
    int functype, const std::vector<struct value_t *> &args);

void task_shutdown();

#endif
