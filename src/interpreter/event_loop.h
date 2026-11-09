#ifndef SKEIN_EVENT_LOOP_H
#define SKEIN_EVENT_LOOP_H

#include <uv.h>

#include <cstdint>
#include <functional>

bool event_loop_init();
bool event_loop_initialized();
uv_loop_t *event_loop_handle();

/* Thread-safe wakeup for fibers blocked on non-I/O state (channels, etc.).
 * The handle belongs to the loop of the thread that created it; callers on
 * other threads may pass it to event_loop_wake(). */
void *event_loop_wake_handle();
void event_loop_wake(void *handle);

enum event_wait_result {
    EVENT_WAIT_OK,
    EVENT_WAIT_TIMEOUT,
    EVENT_WAIT_INTERRUPTED
};

struct event_timeout {
    bool enabled = false;
    uint64_t deadline_ms = 0;
};

event_timeout make_event_timeout(long long timeout_ms);
bool event_timeout_expired(const event_timeout &timeout);

event_wait_result event_wait(bool &done,
                             const std::function<void()> &cancel,
                             const event_timeout &timeout);

void event_loop_shutdown();

#endif
