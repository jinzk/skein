#ifndef SKEIN_HTTP_H
#define SKEIN_HTTP_H

#include "valuetype.h"

#include <atomic>
#include <memory>
#include <string>
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

/*
 * Request context shared by HTTP/1.1 and HTTP/2.  `header_values` maps a
 * lower-case header name to either a string (single value) or a list of
 * strings (repeated header).  Callers keep ownership of `header_values`.
 */
struct value_t *new_header_values_dict();
void header_values_add(struct value_t *values, const std::string &name,
                       const std::string &value);
std::string http_request_id(struct value_t *header_values);
void add_request_context(struct value_t *request, const std::string &target,
                         struct value_t *header_values);
struct value_t *http_json_error_response(long long status,
                                         const std::string &code,
                                         const std::string &message,
                                         const std::string &request_id);

struct value_t *call_http_builtin(
    int functype, const std::vector<struct value_t *> &args);

void http_shutdown();

#endif
