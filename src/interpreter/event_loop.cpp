#include "event_loop.h"

#include "fiber.h"
#include "repl.h"

namespace {

struct event_loop_runtime {
    uv_loop_t loop;
    uv_timer_t ticker;
    uv_async_t wake;
    bool has_wake = false;
    bool initialized = false;
};

event_loop_runtime &runtime() {
    static thread_local event_loop_runtime state;
    return state;
}

void on_timer_close(uv_handle_t *) {}

void on_loop_wake(uv_async_t *) {
    /* Wake uv_run(); the caller's fiber_pump() rechecks fiber predicates. */
}

} // namespace

bool event_loop_init() {
    event_loop_runtime &state = runtime();
    if (state.initialized)
        return true;
    if (uv_loop_init(&state.loop) != 0)
        return false;
    if (uv_timer_init(&state.loop, &state.ticker) != 0) {
        uv_loop_close(&state.loop);
        return false;
    }
    if (uv_timer_start(&state.ticker, [](uv_timer_t *) {}, 10, 10) != 0) {
        uv_close((uv_handle_t *)&state.ticker, on_timer_close);
        uv_run(&state.loop, UV_RUN_DEFAULT);
        uv_loop_close(&state.loop);
        return false;
    }
    state.initialized = true;
    return true;
}

bool event_loop_initialized() {
    return runtime().initialized;
}

uv_loop_t *event_loop_handle() {
    return runtime().initialized ? &runtime().loop : NULL;
}

void *event_loop_wake_handle() {
    event_loop_runtime &state = runtime();
    if (!state.initialized)
        return NULL;
    if (!state.has_wake) {
        if (uv_async_init(&state.loop, &state.wake, on_loop_wake) != 0)
            return NULL;
        uv_unref((uv_handle_t *)&state.wake);
        state.has_wake = true;
    }
    return &state.wake;
}

void event_loop_wake(void *handle) {
    if (handle)
        uv_async_send((uv_async_t *)handle);
}

event_timeout make_event_timeout(long long timeout_ms) {
    event_timeout timeout;
    if (timeout_ms > 0) {
        uint64_t now = (uint64_t)(uv_hrtime() / 1000000ULL);
        uint64_t delay = (uint64_t)timeout_ms;
        timeout.enabled = true;
        timeout.deadline_ms =
            now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    }
    return timeout;
}

bool event_timeout_expired(const event_timeout &timeout) {
    return timeout.enabled &&
           (uint64_t)(uv_hrtime() / 1000000ULL) >= timeout.deadline_ms;
}

event_wait_result event_wait(bool &done,
                             const std::function<void()> &cancel,
                             const event_timeout &timeout) {
    event_loop_runtime &state = runtime();
    bool interrupted = false;
    bool timed_out = false;
    bool canceled = false;
    while (!done) {
        if (!interrupted && g_interrupted)
            interrupted = true;
        if (!timed_out && event_timeout_expired(timeout))
            timed_out = true;
        if ((interrupted || timed_out) && !canceled) {
            canceled = true;
            cancel();
        }
        uv_run(&state.loop, UV_RUN_ONCE);
        fiber_pump();
    }
    if (interrupted)
        return EVENT_WAIT_INTERRUPTED;
    if (timed_out)
        return EVENT_WAIT_TIMEOUT;
    return EVENT_WAIT_OK;
}

void event_loop_shutdown() {
    event_loop_runtime &state = runtime();
    if (!state.initialized)
        return;
    uv_timer_stop(&state.ticker);
    uv_close((uv_handle_t *)&state.ticker, on_timer_close);
    if (state.has_wake) {
        uv_close((uv_handle_t *)&state.wake, on_timer_close);
        state.has_wake = false;
    }
    uv_run(&state.loop, UV_RUN_DEFAULT);
    uv_loop_close(&state.loop);
    state.initialized = false;
}
