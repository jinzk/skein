#ifndef CALC_FIBER_H
#define CALC_FIBER_H

#include "task.h"
#include "valuetype.h"

#include <functional>

bool fiber_available();
task_ptr fiber_start_async(std::function<value_t *()> body);
value_t *fiber_await_task(const task_ptr &task);
void fiber_pump();
void fiber_shutdown();

#endif
