#ifndef SKEIN_HTTP_H
#define SKEIN_HTTP_H

#include "valuetype.h"

#include <atomic>
#include <memory>
#include <vector>

/* Shared counters for servers that may run across worker threads. */
struct http_shared_state {
    std::atomic<long long> claimed{0};
    std::atomic<long long> served{0};
    std::atomic<bool> stopping{false};
    std::atomic<long long> active_connections{0};
};

/* A server request body that the handler reads chunk by chunk. */
struct request_body {
    virtual ~request_body() {}
    /* Return the next chunk. `done` is set when the body is exhausted. */
    virtual bool next(struct value_t *&value, bool &done) = 0;
};

struct value_t *new_request_body_value(std::shared_ptr<request_body> body);
bool get_request_body_value(struct value_t *value,
                            std::shared_ptr<request_body> &body);
struct value_t *http_read_body(const std::vector<struct value_t *> &args);

struct value_t *call_http_builtin(
    int functype, const std::vector<struct value_t *> &args);

void http_shutdown();

#endif
