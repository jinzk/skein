#include "async_support.h"

#include "event_loop.h"

namespace calc_async {

namespace {

void on_async_timer_close(uv_handle_t *handle) {
    delete (uv_timer_t *)handle;
}

} // namespace

state::~state() {
    close_timer();
}

void state::close_timer() {
    if (!timer)
        return;
    uv_timer_t *handle = timer;
    timer = NULL;
    handle->data = NULL;
    uv_timer_stop(handle);
    if (event_loop_initialized())
        uv_close((uv_handle_t *)handle, on_async_timer_close);
    else
        delete handle;
}

void async_complete(state *state, value_t *value) {
    if (!state) {
        freeval(value);
        return;
    }
    if (state->finished) {
        freeval(value);
        delete state;
        return;
    }
    state->finished = true;
    state->close_timer();
    task_ptr task = state->task_ref.lock();
    if (task)
        task_complete(task, value);
    else
        freeval(value);
    delete state;
}

void async_fail(state *state, const std::string &error) {
    if (!state)
        return;
    if (state->finished) {
        delete state;
        return;
    }
    state->finished = true;
    state->close_timer();
    task_ptr task = state->task_ref.lock();
    if (task)
        task_fail(task, error);
    delete state;
}

void async_cancel(state *state) {
    if (!state || state->finished)
        return;
    state->finished = true;
    state->close_timer();
    std::function<void()> cancel = std::move(state->cancel_operation);
    state->cancel_operation = nullptr;
    if (cancel)
        cancel();
    if (!state->callback_after_cancel)
        delete state;
}

void async_timeout(state *state) {
    if (!state || state->finished)
        return;
    state->finished = true;
    state->close_timer();
    task_ptr task = state->task_ref.lock();
    if (task)
        task_fail(task, state->timeout_error);
    std::function<void()> cancel = std::move(state->cancel_operation);
    state->cancel_operation = nullptr;
    if (cancel)
        cancel();
    if (!state->callback_after_cancel)
        delete state;
}

void on_async_timeout(uv_timer_t *timer) {
    state *value = (state *)timer->data;
    timer->data = NULL;
    if (!value) {
        uv_close((uv_handle_t *)timer, on_async_timer_close);
        return;
    }
    value->timer = NULL;
    uv_timer_stop(timer);
    uv_close((uv_handle_t *)timer, on_async_timer_close);
    async_timeout(value);
}

bool start_async_timeout(state *state, long long timeout_ms) {
    if (timeout_ms <= 0)
        return true;
    state->timer = new uv_timer_t;
    int rc = uv_timer_init(event_loop_handle(), state->timer);
    if (rc < 0) {
        delete state->timer;
        state->timer = NULL;
        return false;
    }
    state->timer->data = state;
    rc = uv_timer_start(state->timer, on_async_timeout,
                        (uint64_t)timeout_ms, 0);
    if (rc < 0) {
        state->close_timer();
        return false;
    }
    return true;
}

bool prepare_async(state *state, task_ptr &task, long long timeout_ms,
                   const std::string &timeout_error) {
    if (!event_loop_init())
        return false;
    task = task_create();
    state->task_ref = task;
    state->timeout_error = timeout_error;
    if (!start_async_timeout(state, timeout_ms)) {
        task_fail(task, "cannot create timeout timer");
        return false;
    }
    task_set_cancel(task, [state] {
        async_cancel(state);
    });
    return true;
}

std::string async_timeout_error(const char *operation, long long timeout_ms) {
    return std::string(operation) + ": timed out after " +
           std::to_string(timeout_ms) + " ms";
}

} // namespace calc_async
