#ifndef SKEIN_HTTP2_H
#define SKEIN_HTTP2_H

#include "http.h"
#include "valuetype.h"

#include <cstddef>
#include <memory>
#include <vector>

struct h2_connection_config {
    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    bool stream_request_body = false;
    struct value_t *on_request_body = nullptr;
    long long max_concurrent_streams = 16;
};

/*
 * Run one HTTP/2 server connection to completion.  Takes ownership of
 * `stream`.  `handler` is borrowed for the duration of the call.
 */
void http2_serve_connection(
    struct value_t *stream, struct value_t *handler,
    const h2_connection_config &config,
    const std::shared_ptr<http_shared_state> &shared);

struct value_t *call_http2_builtin(
    int functype, const std::vector<struct value_t *> &args);

void http2_shutdown();

#endif
