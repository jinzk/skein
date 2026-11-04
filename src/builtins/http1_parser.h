#ifndef SKEIN_HTTP1_PARSER_H
#define SKEIN_HTTP1_PARSER_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace http1 {

struct header {
    std::string name;
    std::string value;
};

struct message {
    bool is_request = false;
    std::string method;
    std::string url;
    std::string status_text;
    int status_code = 0;
    int http_major = 0;
    int http_minor = 0;
    std::vector<header> headers;
    std::string body;
    bool keep_alive = false;
    bool upgrade = false;
    bool chunked = false;
};

struct parser_options {
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
};

/* Incremental HTTP/1.x parser. It only parses bytes; it does no I/O. */
class parser {
public:
    explicit parser(bool request, parser_options options = {});
    ~parser();

    parser(parser &&) noexcept;
    parser &operator=(parser &&) noexcept;

    parser(const parser &) = delete;
    parser &operator=(const parser &) = delete;

    /* Feed a chunk. Completed messages are appended to messages(). */
    bool execute(const char *data, std::size_t length);
    bool execute(const std::string &data);

    /* Signal EOF. This can complete a close-delimited response. */
    bool finish();

    /* When a body sink is installed, body bytes are delivered to the sink
       instead of being appended to message.body. Returning false pauses
       parsing; call resume() and feed the input again from paused_offset(). */
    void set_body_sink(std::function<bool(const char *, std::size_t)> sink);
    bool paused() const;
    std::size_t paused_offset() const;
    bool resume();

    const std::vector<message> &messages() const;
    std::vector<message> take_messages();

    /* True once the current message's headers have been parsed. */
    bool headers_complete() const;
    /* The message currently being parsed (headers and any body so far). */
    const message &current_message() const;

    int error_code() const;
    const std::string &error() const;
    std::size_t error_offset() const;

    bool is_request() const;
    void reset(bool request);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace http1

#endif
