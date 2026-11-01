#ifndef CALC_ASYNC_SUPPORT_H
#define CALC_ASYNC_SUPPORT_H

#include "task.h"
#include "valuetype.h"

#include <functional>
#include <string>

#include <uv.h>

namespace calc_async {

struct state {
    std::weak_ptr<task> task_ref;
    std::function<void()> cancel_operation;
    uv_timer_t *timer = NULL;
    std::string timeout_error;
    bool finished = false;
    bool callback_after_cancel = true;

    virtual ~state();

    void close_timer();
};

void async_complete(state *state, value_t *value);
void async_fail(state *state, const std::string &error);
void async_cancel(state *state);
void async_timeout(state *state);
void on_async_timeout(uv_timer_t *timer);
bool start_async_timeout(state *state, long long timeout_ms);
bool prepare_async(state *state, task_ptr &task, long long timeout_ms,
                   const std::string &timeout_error);
std::string async_timeout_error(const char *operation, long long timeout_ms);

} // namespace calc_async

#endif
