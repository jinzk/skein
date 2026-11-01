#ifndef CALC_FIBER_H
#define CALC_FIBER_H

#include "task.h"
#include "valuetype.h"

#include <functional>

struct generator;
using generator_ptr = std::shared_ptr<generator>;

bool fiber_available();
task_ptr fiber_start_async(std::function<value_t *()> body);
value_t *fiber_await_task(const task_ptr &task);
value_t *fiber_yield_value(value_t *value);

generator_ptr generator_create(std::function<value_t *()> body);
value_t *new_generator_value(generator_ptr generator);
bool get_generator_value(value_t *value, generator_ptr &generator);
bool generator_next_value(const generator_ptr &generator,
                          value_t *&value, bool &done);
bool generator_done(const generator_ptr &generator);

void fiber_pump();
void fiber_shutdown();

#endif
