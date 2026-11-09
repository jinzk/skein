#ifndef SKEIN_FIBER_H
#define SKEIN_FIBER_H

#include "task.h"
#include "valuetype.h"

#include <functional>

struct generator;
using generator_ptr = std::shared_ptr<generator>;

bool fiber_available();
bool fiber_in_fiber();
task_ptr fiber_start_async(std::function<value_t *()> body);
value_t *fiber_await_task(const task_ptr &task);
bool fiber_wait_for(std::function<bool()> ready, long long timeout_ms);
value_t *fiber_yield_value(value_t *value);

generator_ptr generator_create(std::function<value_t *()> body);
value_t *new_generator_value(generator_ptr generator);
bool get_generator_value(value_t *value, generator_ptr &generator);
bool generator_next_value(const generator_ptr &generator,
                          value_t *send_value,
                          value_t *&value, bool &done);
bool generator_close(const generator_ptr &generator);
bool generator_done(const generator_ptr &generator);
bool fiber_is_generator(value_t *value);

void fiber_pump();
void fiber_disable_generator_warnings();
void fiber_shutdown();

#endif
