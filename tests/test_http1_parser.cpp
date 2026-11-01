#include "http1_parser.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        ++failures;
        std::cerr << "test_http1_parser: line " << line << ": "
                  << expression << " failed\n";
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

bool feed(http1::parser &parser, const std::string &data, std::size_t chunk) {
    if (chunk == 0)
        chunk = data.size();
    for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
        std::size_t length = std::min(chunk, data.size() - offset);
        if (!parser.execute(data.data() + offset, length))
            return false;
    }
    return true;
}

void test_simple_request() {
    http1::parser parser(true);
    CHECK(parser.execute(
        "GET /hello?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test\r\n"
        "\r\n"));
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(message.is_request);
    CHECK(message.method == "GET");
    CHECK(message.url == "/hello?x=1");
    CHECK(message.http_major == 1);
    CHECK(message.http_minor == 1);
    CHECK(message.headers.size() == 2);
    CHECK(message.headers[0].name == "Host");
    CHECK(message.headers[0].value == "example.com");
    CHECK(message.headers[1].name == "User-Agent");
    CHECK(message.headers[1].value == "test");
    CHECK(message.keep_alive);
    CHECK(!message.upgrade);
    CHECK(!message.chunked);
    CHECK(message.body.empty());
}

void test_split_request() {
    const std::string input =
        "GET /split HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    http1::parser parser(true);
    CHECK(feed(parser, input, 1));
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].url == "/split");
    CHECK(parser.messages()[0].headers.size() == 1);
}

void test_pipelined_requests() {
    http1::parser parser(true);
    CHECK(parser.execute(
        "GET /a HTTP/1.1\r\nHost: a\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: b\r\n\r\n"));
    CHECK(parser.messages().size() == 2);
    CHECK(parser.messages()[0].url == "/a");
    CHECK(parser.messages()[1].url == "/b");
}

void test_content_length_body() {
    const std::string input =
        "POST /submit HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "hello world";
    http1::parser parser(true);
    CHECK(feed(parser, input, 7));
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(message.method == "POST");
    CHECK(message.body == "hello world");
    CHECK(!message.chunked);
}

void test_chunked_body() {
    const std::string input =
        "POST /chunk HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n";
    http1::parser parser(true);
    CHECK(feed(parser, input, 3));
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(message.body == "hello world");
    CHECK(message.chunked);
}

void test_simple_response() {
    http1::parser parser(false);
    CHECK(parser.execute(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello"));
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(!message.is_request);
    CHECK(message.status_code == 200);
    CHECK(message.status_text == "OK");
    CHECK(message.body == "hello");
    CHECK(message.keep_alive);
}

void test_chunked_response() {
    http1::parser parser(false);
    CHECK(parser.execute(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nwiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n"));
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].body == "wikipedia");
    CHECK(parser.messages()[0].chunked);
}

void test_close_delimited_response() {
    http1::parser parser(false);
    CHECK(parser.execute(
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello"));
    CHECK(parser.finish());
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(message.body == "hello");
    CHECK(!message.keep_alive);
}

void test_empty_header_value() {
    http1::parser parser(true);
    CHECK(parser.execute(
        "GET / HTTP/1.1\r\n"
        "X-Empty:\r\n"
        "Host: x\r\n"
        "\r\n"));
    CHECK(parser.messages().size() == 1);
    const http1::message &message = parser.messages()[0];
    CHECK(message.headers.size() == 2);
    CHECK(message.headers[0].name == "X-Empty");
    CHECK(message.headers[0].value.empty());
    CHECK(message.headers[1].name == "Host");
}

void test_http_10() {
    http1::parser parser(true);
    CHECK(parser.execute("GET / HTTP/1.0\r\nHost: x\r\n\r\n"));
    CHECK(parser.messages().size() == 1);
    CHECK(!parser.messages()[0].keep_alive);
}

void test_upgrade() {
    http1::parser parser(true);
    CHECK(parser.execute(
        "GET /chat HTTP/1.1\r\n"
        "Host: x\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "\r\n"));
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].upgrade);
}

void test_invalid_request() {
    http1::parser parser(true);
    CHECK(!parser.execute("GET / HTTP/1.1\r\nBad Header\r\n\r\n"));
    CHECK(!parser.error().empty());
    CHECK(parser.error_code() != 0);
}

void test_header_limit() {
    http1::parser_options options;
    options.max_header_bytes = 8;
    http1::parser parser(true, options);
    CHECK(!parser.execute(
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"));
    CHECK(parser.error().find("header") != std::string::npos);
}

void test_header_count_limit() {
    http1::parser_options options;
    options.max_headers = 1;
    http1::parser parser(true, options);
    CHECK(!parser.execute(
        "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: y\r\n\r\n"));
    CHECK(parser.error().find("headers") != std::string::npos);
}

void test_body_limit() {
    http1::parser_options options;
    options.max_body_bytes = 4;
    http1::parser parser(true, options);
    CHECK(!parser.execute(
        "POST / HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello"));
    CHECK(parser.error().find("body") != std::string::npos);
}

void test_streaming_body_pause() {
    http1::parser parser(true);
    std::string body;
    int calls = 0;
    parser.set_body_sink([&](const char *at, std::size_t length) {
        body.append(at, length);
        ++calls;
        return calls != 1;
    });

    const std::string input =
        "POST /pause HTTP/1.1\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "0123456789";
    CHECK(!parser.execute(input));
    CHECK(parser.paused());
    CHECK(parser.error().empty());
    std::size_t offset = parser.paused_offset();
    CHECK(offset > 0);
    CHECK(offset <= input.size());
    CHECK(parser.resume());
    CHECK(parser.execute(input.data() + offset, input.size() - offset));
    CHECK(body == "0123456789");
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].body.empty());
}

void test_streaming_chunked_body() {
    http1::parser parser(true);
    std::string body;
    int calls = 0;
    parser.set_body_sink([&](const char *at, std::size_t length) {
        body.append(at, length);
        ++calls;
        return calls != 1;
    });

    const std::string input =
        "POST /chunk HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n";
    CHECK(!parser.execute(input));
    CHECK(parser.paused());
    std::size_t offset = parser.paused_offset();
    CHECK(parser.resume());
    CHECK(parser.execute(input.data() + offset, input.size() - offset));
    CHECK(body == "hello world");
    CHECK(parser.messages().size() == 1);
}

void test_take_messages() {
    http1::parser parser(true);
    CHECK(parser.execute("GET /a HTTP/1.1\r\n\r\n"));
    std::vector<http1::message> messages = parser.take_messages();
    CHECK(messages.size() == 1);
    CHECK(parser.messages().empty());
    CHECK(parser.execute("GET /b HTTP/1.1\r\n\r\n"));
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].url == "/b");
}

void test_reset() {
    http1::parser parser(true);
    CHECK(parser.execute("GET /a HTTP/1.1\r\n\r\n"));
    parser.reset(false);
    CHECK(!parser.is_request());
    CHECK(parser.execute("HTTP/1.1 204 No Content\r\n\r\n"));
    CHECK(parser.messages().size() == 1);
    CHECK(parser.messages()[0].status_code == 204);
}

} // namespace

int main() {
    test_simple_request();
    test_split_request();
    test_pipelined_requests();
    test_content_length_body();
    test_chunked_body();
    test_simple_response();
    test_chunked_response();
    test_close_delimited_response();
    test_empty_header_value();
    test_http_10();
    test_upgrade();
    test_invalid_request();
    test_header_limit();
    test_header_count_limit();
    test_body_limit();
    test_streaming_body_pause();
    test_streaming_chunked_body();
    test_take_messages();
    test_reset();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "http1 parser tests passed\n";
    return 0;
}
