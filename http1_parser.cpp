#include "http1_parser.h"

#include "llhttp.h"

#include <cstring>
#include <utility>

namespace http1 {

namespace {

struct parser_state;

int on_message_begin(llhttp_t *parser);
int on_url(llhttp_t *parser, const char *at, size_t length);
int on_status(llhttp_t *parser, const char *at, size_t length);
int on_header_field(llhttp_t *parser, const char *at, size_t length);
int on_header_field_complete(llhttp_t *parser);
int on_header_value(llhttp_t *parser, const char *at, size_t length);
int on_header_value_complete(llhttp_t *parser);
int on_headers_complete(llhttp_t *parser);
int on_body(llhttp_t *parser, const char *at, size_t length);
int on_message_complete(llhttp_t *parser);
int on_chunk_header(llhttp_t *parser);
int on_reset(llhttp_t *parser);

struct parser_state {
    llhttp_t handle{};
    llhttp_settings_t settings{};
    parser_options options;
    bool request = false;

    std::vector<message> messages;
    message current;

    std::string header_field;
    std::string header_value;
    bool header_field_done = false;
    bool headers_complete = false;
    std::size_t header_bytes = 0;
    std::size_t header_count = 0;
    std::size_t body_bytes = 0;
    std::function<bool(const char *, std::size_t)> body_sink;
    bool paused = false;
    std::size_t paused_offset = 0;

    std::size_t total_bytes = 0;
    std::size_t base_offset = 0;
    int error_code = 0;
    std::string error;
    std::size_t error_offset = 0;
    bool failed = false;

    parser_state(bool request_, const parser_options &options_)
        : options(options_), request(request_) {
        std::memset(&settings, 0, sizeof(settings));
        settings.on_message_begin = on_message_begin;
        settings.on_url = on_url;
        settings.on_status = on_status;
        settings.on_header_field = on_header_field;
        settings.on_header_field_complete = on_header_field_complete;
        settings.on_header_value = on_header_value;
        settings.on_header_value_complete = on_header_value_complete;
        settings.on_headers_complete = on_headers_complete;
        settings.on_body = on_body;
        settings.on_message_complete = on_message_complete;
        settings.on_chunk_header = on_chunk_header;
        settings.on_reset = on_reset;

        llhttp_init(&handle, request ? HTTP_REQUEST : HTTP_RESPONSE,
                    &settings);
        handle.data = this;
        current.is_request = request;
        header_field.reserve(64);
        header_value.reserve(128);
    }

    static parser_state *from(llhttp_t *parser) {
        return static_cast<parser_state *>(parser->data);
    }

    int fail(const char *message) {
        if (!failed) {
            failed = true;
            error_code = HPE_USER;
            error = message;
        }
        return -1;
    }

    bool add_header_bytes(std::size_t length) {
        if (header_bytes > options.max_header_bytes ||
            length > options.max_header_bytes - header_bytes) {
            fail("header bytes exceed limit");
            return false;
        }
        header_bytes += length;
        return true;
    }

    int push_header() {
        if (header_count >= options.max_headers) {
            return fail("too many headers");
        }
        current.headers.push_back(
            header{std::move(header_field), std::move(header_value)});
        header_field.clear();
        header_value.clear();
        header_field_done = false;
        ++header_count;
        return 0;
    }
};

int on_message_begin(llhttp_t *parser) {
    parser_state *state = parser_state::from(parser);
    state->current = message{};
    state->current.is_request = state->request;
    state->header_field.clear();
    state->header_value.clear();
    state->header_field_done = false;
    state->headers_complete = false;
    state->header_bytes = 0;
    state->header_count = 0;
    state->body_bytes = 0;
    return 0;
}

int on_url(llhttp_t *parser, const char *at, size_t length) {
    parser_state *state = parser_state::from(parser);
    state->current.url.append(at, length);
    return 0;
}

int on_status(llhttp_t *parser, const char *at, size_t length) {
    parser_state *state = parser_state::from(parser);
    state->current.status_text.append(at, length);
    return 0;
}

int on_header_field(llhttp_t *parser, const char *at, size_t length) {
    parser_state *state = parser_state::from(parser);
    if (state->header_field_done) {
        /* The previous header had no value callback. */
        if (state->push_header() != 0)
            return -1;
    }
    if (!state->add_header_bytes(length))
        return -1;
    state->header_field.append(at, length);
    return 0;
}

int on_header_field_complete(llhttp_t *parser) {
    parser_state::from(parser)->header_field_done = true;
    return 0;
}

int on_header_value(llhttp_t *parser, const char *at, size_t length) {
    parser_state *state = parser_state::from(parser);
    if (!state->add_header_bytes(length))
        return -1;
    state->header_value.append(at, length);
    return 0;
}

int on_header_value_complete(llhttp_t *parser) {
    parser_state *state = parser_state::from(parser);
    return state->push_header();
}

int on_headers_complete(llhttp_t *parser) {
    parser_state *state = parser_state::from(parser);

    /* Empty header values do not always produce a value callback. */
    if (state->header_field_done || !state->header_field.empty() ||
        !state->header_value.empty()) {
        if (state->push_header() != 0)
            return -1;
    }

    state->current.http_major =
        static_cast<int>(llhttp_get_http_major(parser));
    state->current.http_minor =
        static_cast<int>(llhttp_get_http_minor(parser));

    if (state->request) {
        const char *method = llhttp_method_name(
            static_cast<llhttp_method_t>(llhttp_get_method(parser)));
        state->current.method = method ? method : "";
    } else {
        state->current.status_code = llhttp_get_status_code(parser);
    }

    state->current.keep_alive = llhttp_should_keep_alive(parser) != 0;
    state->current.upgrade = llhttp_get_upgrade(parser) != 0;
    state->current.chunked = (parser->flags & F_CHUNKED) != 0;
    state->headers_complete = true;
    return 0;
}

int on_body(llhttp_t *parser, const char *at, size_t length) {
    parser_state *state = parser_state::from(parser);
    if (state->body_bytes > state->options.max_body_bytes ||
        length > state->options.max_body_bytes - state->body_bytes) {
        return state->fail("body exceeds limit");
    }
    state->body_bytes += length;
    if (state->body_sink) {
        if (!state->body_sink(at, length))
            return HPE_PAUSED;
        return 0;
    }
    state->current.body.append(at, length);
    return 0;
}

int on_message_complete(llhttp_t *parser) {
    parser_state *state = parser_state::from(parser);
    state->messages.push_back(std::move(state->current));
    state->current = message{};
    state->current.is_request = state->request;
    state->headers_complete = false;
    return 0;
}

int on_chunk_header(llhttp_t *parser) {
    parser_state::from(parser)->current.chunked = true;
    return 0;
}

int on_reset(llhttp_t *parser) {
    parser_state *state = parser_state::from(parser);
    state->current = message{};
    state->current.is_request = state->request;
    state->header_field.clear();
    state->header_value.clear();
    state->header_field_done = false;
    state->headers_complete = false;
    state->header_bytes = 0;
    state->header_count = 0;
    state->body_bytes = 0;
    return 0;
}

/* Upgrade responses pause after the HTTP message without calling
   on_message_complete. Preserve the parsed message before switching
   protocols. */
void finalize_upgrade_message(parser_state *state) {
    if (state->current.url.empty() && state->current.status_text.empty() &&
        state->current.headers.empty()) {
        return;
    }
    state->current.upgrade = true;
    state->messages.push_back(std::move(state->current));
    state->current = message{};
    state->current.is_request = state->request;
    state->headers_complete = false;
}

} // namespace

struct parser::impl {
    parser_state state;

    impl(bool request, const parser_options &options)
        : state(request, options) {}
};

parser::parser(bool request, parser_options options)
    : impl_(new impl(request, options)) {}

parser::~parser() = default;

parser::parser(parser &&) noexcept = default;

parser &parser::operator=(parser &&) noexcept = default;

bool parser::execute(const char *data, std::size_t length) {
    parser_state &state = impl_->state;
    if (state.failed)
        return false;

    state.base_offset = state.total_bytes;
    llhttp_errno_t error = llhttp_execute(&state.handle, data, length);
    if (error == HPE_PAUSED_UPGRADE) {
        finalize_upgrade_message(&state);
        state.total_bytes += length;
        return true;
    }
    if (error == HPE_PAUSED) {
        const char *position = llhttp_get_error_pos(&state.handle);
        state.paused = true;
        state.paused_offset = 0;
        if (position && position >= data && position <= data + length)
            state.paused_offset = (std::size_t)(position - data);
        return false;
    }
    if (error == HPE_OK) {
        state.paused = false;
        state.paused_offset = 0;
        state.total_bytes += length;
        return true;
    }
    if (state.failed)
        return false;

    state.error_code = static_cast<int>(error);
    const char *name = llhttp_errno_name(error);
    const char *reason = llhttp_get_error_reason(&state.handle);
    state.error = name ? name : "HTTP parse error";
    if (reason && *reason) {
        state.error += ": ";
        state.error += reason;
    }

    const char *position = llhttp_get_error_pos(&state.handle);
    if (position && position >= data && position <= data + length) {
        state.error_offset =
            state.base_offset + static_cast<std::size_t>(position - data);
    }
    return false;
}

bool parser::execute(const std::string &data) {
    return execute(data.data(), data.size());
}

bool parser::finish() {
    parser_state &state = impl_->state;
    if (state.failed)
        return false;

    state.base_offset = state.total_bytes;
    llhttp_errno_t error = llhttp_finish(&state.handle);
    if (error == HPE_OK) {
        state.paused = false;
        state.paused_offset = 0;
        return true;
    }
    if (error == HPE_PAUSED) {
        state.paused = true;
        state.paused_offset = 0;
        return false;
    }
    if (state.failed)
        return false;

    state.error_code = static_cast<int>(error);
    const char *name = llhttp_errno_name(error);
    const char *reason = llhttp_get_error_reason(&state.handle);
    state.error = name ? name : "HTTP parse error";
    if (reason && *reason) {
        state.error += ": ";
        state.error += reason;
    }
    return false;
}

void parser::set_body_sink(
    std::function<bool(const char *, std::size_t)> sink) {
    impl_->state.body_sink = std::move(sink);
}

bool parser::paused() const {
    return impl_->state.paused;
}

std::size_t parser::paused_offset() const {
    return impl_->state.paused_offset;
}

bool parser::resume() {
    parser_state &state = impl_->state;
    if (!state.paused)
        return false;
    llhttp_resume(&state.handle);
    state.paused = false;
    state.paused_offset = 0;
    return true;
}

const std::vector<message> &parser::messages() const {
    return impl_->state.messages;
}

std::vector<message> parser::take_messages() {
    std::vector<message> messages = std::move(impl_->state.messages);
    impl_->state.messages.clear();
    return messages;
}

bool parser::headers_complete() const {
    return impl_->state.headers_complete;
}

const message &parser::current_message() const {
    return impl_->state.current;
}

int parser::error_code() const {
    return impl_->state.error_code;
}

const std::string &parser::error() const {
    return impl_->state.error;
}

std::size_t parser::error_offset() const {
    return impl_->state.error_offset;
}

bool parser::is_request() const {
    return impl_->state.request;
}

void parser::reset(bool request) {
    parser_state &state = impl_->state;
    state.request = request;
    llhttp_init(&state.handle, request ? HTTP_REQUEST : HTTP_RESPONSE,
                &state.settings);
    state.handle.data = &state;
    state.messages.clear();
    state.current = message{};
    state.current.is_request = request;
    state.header_field.clear();
    state.header_value.clear();
    state.header_bytes = 0;
    state.header_count = 0;
    state.body_bytes = 0;
    state.headers_complete = false;
    state.paused = false;
    state.paused_offset = 0;
    state.total_bytes = 0;
    state.base_offset = 0;
    state.error_code = 0;
    state.error.clear();
    state.error_offset = 0;
    state.failed = false;
}

} // namespace http1
