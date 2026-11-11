#ifndef SKEIN_THREAD_CHANNEL_H
#define SKEIN_THREAD_CHANNEL_H

#include "valuetype.h"

#include <memory>
#include <string>
#include <vector>

struct value_t *call_thread_channel_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Encode a function value and its dependency graph as an entry blob.
 * Shared by std.entry.encode and std.process.run_function/spawn_function. */
bool thread_entry_encode(struct value_t *function, std::string &bytes,
                         std::string &error);

/* Run a `std.entry.encode` blob synchronously with the given Message.
 * Used by process IPC (`--run-entry`) and std.process.run_entry. */
bool thread_run_entry_blocking(const std::string &bytes,
                                 struct value_t *message,
                                 struct value_t *&result,
                                 std::string &error);

/* Parse a process request (entry blob + serialized Message payload), run it,
 * and serialize the result envelope as data values. */
bool thread_run_entry_request(const std::string &request,
                                std::string &result, std::string &error);

/* Borrowed payload for process IPC; returns a duplicated value. */
struct value_t *thread_message_payload(struct value_t *message);

/*
 * Common per-thread runtime setup/teardown for worker threads that execute
 * Skein code (thread pool workers and reuse-port HTTP workers).
 */
bool thread_worker_begin(bool worker_vm, std::string &error);
void thread_worker_end();

/*
 * C++ link to the thread pool for native subsystems such as the HTTP server.
 * The script-facing std.thread.pool wrappers are built on the same state.
 */
struct thread_pool_link;
struct thread_future_link;

enum class thread_future_status {
    ready,
    pending,
    closed,
    canceled,
};

std::shared_ptr<thread_pool_link> thread_pool_link_create(
    std::size_t workers, const std::string &script, const std::string &entry,
    long long queue_capacity, long long restart_limit, bool worker_vm,
    std::string &error);

bool thread_pool_link_submit(
    const std::shared_ptr<thread_pool_link> &pool, struct value_t *payload,
    long long timeout_ms, std::shared_ptr<thread_future_link> &future,
    std::string &error);

thread_future_status thread_future_link_wait(
    const std::shared_ptr<thread_future_link> &future, long long timeout_ms,
    struct value_t *&payload, std::string &error);

void thread_future_link_cancel(
    const std::shared_ptr<thread_future_link> &future);

void thread_pool_link_close(const std::shared_ptr<thread_pool_link> &pool);
void thread_pool_link_join(const std::shared_ptr<thread_pool_link> &pool);
struct value_t *thread_pool_link_stats(
    const std::shared_ptr<thread_pool_link> &pool);

#endif
