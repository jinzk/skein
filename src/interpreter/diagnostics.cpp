#include "diagnostics.h"
#include "runtime_state.h"
#include "unicode_ident.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

thread_local int g_token_line = 1;
thread_local int g_token_column = 1;
thread_local int g_error_line = 0;
thread_local int g_error_column = 0;
thread_local int g_flow_line = 0;
thread_local int g_flow_column = 0;

static thread_local int current_line = 1;
static thread_local int current_column = 1;
static thread_local int error_count = 0;
static thread_local int lexical_error_line = 0;
static thread_local std::string source_name;
static thread_local std::string package_name;
static thread_local std::vector<std::string> source_lines;

static thread_local std::vector<diag_call_frame> call_stack;
static const size_t MAX_REPORTED_FRAMES = 8;

void diag_scan_token(const char *text, int length) {
    g_token_line = current_line;
    g_token_column = current_column;
    int i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            ++current_line;
            current_column = 1;
            ++i;
        } else if (c < 0x80) {
            ++current_column;
            ++i;
        } else {
            unicode_ident::decode_result decoded =
                unicode_ident::decode(text + i, (size_t)(length - i));
            if (!decoded.valid) {
                ++current_column;
                ++i;
            } else {
                current_column +=
                    (int)unicode_ident::display_width(decoded.codepoint);
                i += (int)decoded.length;
            }
        }
    }
}

void diag_scan_rewind(const char *text, int length) {
    int i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            if (current_line > 1)
                --current_line;
            current_column = 1;
            ++i;
        } else if (c < 0x80) {
            if (current_column > 1)
                --current_column;
            ++i;
        } else {
            unicode_ident::decode_result decoded =
                unicode_ident::decode(text + i, (size_t)(length - i));
            if (!decoded.valid) {
                if (current_column > 1)
                    --current_column;
                ++i;
            } else {
                size_t width = unicode_ident::display_width(decoded.codepoint);
                current_column -= (int)width;
                if (current_column < 1)
                    current_column = 1;
                i += (int)decoded.length;
            }
        }
    }
}

void diag_record_line(int line, const std::string &text) {
    if (line <= 0)
        return;
    if ((size_t)line > source_lines.size())
        source_lines.resize((size_t)line);
    std::string display = text;
    while (!display.empty() &&
           (display.back() == '\n' || display.back() == '\r'))
        display.pop_back();
    source_lines[(size_t)line - 1] = display;
}

void diag_set_location(int line, int column) {
    g_error_line = line;
    g_error_column = column;
}

void diag_set_flow_location(int line, int column) {
    g_flow_line = line;
    g_flow_column = column;
}

int diag_current_line() {
    return current_line;
}

int diag_current_column() {
    return current_column;
}

void diag_start_input_line() {
    current_column = 1;
}

static void print_snippet(int line, int column) {
    if (line <= 0 || (size_t)line > source_lines.size())
        return;
    const std::string &text = source_lines[(size_t)line - 1];
    if (text.empty())
        return;
    std::fprintf(stderr, "  %s\n", text.c_str());
    if (column <= 0)
        return;
    std::fputs("  ", stderr);
    size_t pos = 0;
    int col = 1;
    while (pos < text.size() && col < column) {
        unsigned char c = (unsigned char)text[pos];
        if (c == '\n')
            break;
        if (c == '\t') {
            std::fputc('\t', stderr);
            ++col;
            ++pos;
        } else if (c < 0x80) {
            std::fputc(' ', stderr);
            ++col;
            ++pos;
        } else {
            unicode_ident::decode_result decoded =
                unicode_ident::decode(text.data() + pos, text.size() - pos);
            if (!decoded.valid) {
                std::fputc(' ', stderr);
                ++col;
                ++pos;
            } else {
                size_t width =
                    unicode_ident::display_width(decoded.codepoint);
                for (size_t i = 0; i < width; ++i)
                    std::fputc(' ', stderr);
                col += (int)width;
                pos += decoded.length;
            }
        }
    }
    while (col < column) {
        std::fputc(' ', stderr);
        ++col;
    }
    std::fputs("^\n", stderr);
}

static void print_call_stack() {
    if (call_stack.empty())
        return;
    size_t shown = call_stack.size();
    size_t first = 0;
    if (shown > MAX_REPORTED_FRAMES) {
        first = shown - MAX_REPORTED_FRAMES;
        std::fprintf(stderr, "  ... %zu more call(s)\n", first);
    }
    for (size_t i = shown; i > first; --i) {
        const diag_call_frame &frame = call_stack[i - 1];
        if (frame.line > 0)
            std::fprintf(stderr, "  in %s() called from line %d\n",
                         frame.name, frame.line);
        else
            std::fprintf(stderr, "  in %s()\n", frame.name);
    }
}

void diag_mark_script_failure() {
    if (!g_interactive && !g_keep_going && g_package_loading == 0)
        g_script_stop = true;
}

void diag_report(int line, int column, const char *category,
                 const char *message) {
    if (g_interrupted)
        return;

    ++error_count;
    diag_mark_script_failure();

    if (line <= 0)
        line = g_token_line;
    if (column <= 0)
        column = g_token_column;

    std::fflush(stdout);
    if (!source_name.empty()) {
        if (column > 0)
            std::fprintf(stderr, "%s:%d:%d: %s: %s\n", source_name.c_str(),
                         line, column, category, message);
        else
            std::fprintf(stderr, "%s:%d: %s: %s\n", source_name.c_str(),
                         line, category, message);
    } else if (column > 0) {
        std::fprintf(stderr, "%d:%d: %s: %s\n", line, column, category,
                     message);
    } else {
        std::fprintf(stderr, "%d: %s: %s\n", line, category, message);
    }

    print_snippet(line, column);
    print_call_stack();
    std::fflush(stderr);
}

void diag_warning(const char *message) {
    if (g_interrupted)
        return;

    std::fflush(stdout);
    if (!source_name.empty())
        std::fprintf(stderr, "%s: warning: %s\n", source_name.c_str(),
                     message);
    else
        std::fprintf(stderr, "warning: %s\n", message);
    std::fflush(stderr);
}

void diag_report_warning(int line, int column, const char *category,
                         const char *message) {
    if (g_interrupted)
        return;

    if (line <= 0)
        line = g_token_line;
    if (column <= 0)
        column = g_token_column;

    std::fflush(stdout);
    const char *kind = category && *category ? category : "warning";
    if (!source_name.empty()) {
        if (column > 0)
            std::fprintf(stderr, "%s:%d:%d: %s: %s\n", source_name.c_str(),
                         line, column, kind, message);
        else
            std::fprintf(stderr, "%s:%d: %s: %s\n", source_name.c_str(),
                         line, kind, message);
    } else if (column > 0) {
        std::fprintf(stderr, "%d:%d: %s: %s\n", line, column, kind,
                     message);
    } else {
        std::fprintf(stderr, "%d: %s: %s\n", line, kind, message);
    }
    std::fflush(stderr);
}

void diag_report_lexical(int line, int column, const char *fmt, ...) {
    if (line <= 0)
        line = g_token_line;
    if (lexical_error_line == line)
        return;
    lexical_error_line = line;
    g_statement_invalid = true;

    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    diag_report(line, column, "lexical error", buffer);
}

bool diag_lexical_error_on_line(int line) {
    return line > 0 && lexical_error_line == line;
}

void diag_push_call(const char *name, int line, int column) {
    call_stack.push_back(diag_call_frame{name, line, column});
}

void diag_pop_call() {
    if (!call_stack.empty())
        call_stack.pop_back();
}

int diag_call_depth() { return (int)call_stack.size(); }

int diag_error_count() {
    return error_count;
}

void diag_set_source_name(const std::string &name) {
    source_name = name;
}

const std::string &diag_current_source_name() {
    return source_name;
}

void diag_set_package_name(const std::string &name) {
    package_name = name;
}

const std::string &diag_current_package_name() {
    return package_name;
}

void diag_save_source(diag_source_checkpoint &checkpoint) {
    checkpoint.current_line = current_line;
    checkpoint.current_column = current_column;
    checkpoint.token_line = g_token_line;
    checkpoint.token_column = g_token_column;
    checkpoint.error_line = g_error_line;
    checkpoint.error_column = g_error_column;
    checkpoint.flow_line = g_flow_line;
    checkpoint.flow_column = g_flow_column;
    checkpoint.lexical_line = lexical_error_line;
    checkpoint.source_name = source_name;
    checkpoint.package_name = package_name;
    checkpoint.lines = source_lines;
}

void diag_restore_source(const diag_source_checkpoint &checkpoint) {
    current_line = checkpoint.current_line;
    current_column = checkpoint.current_column;
    g_token_line = checkpoint.token_line;
    g_token_column = checkpoint.token_column;
    g_error_line = checkpoint.error_line;
    g_error_column = checkpoint.error_column;
    g_flow_line = checkpoint.flow_line;
    g_flow_column = checkpoint.flow_column;
    lexical_error_line = checkpoint.lexical_line;
    source_name = checkpoint.source_name;
    package_name = checkpoint.package_name;
    source_lines = checkpoint.lines;
}

void diag_capture(diag_state &state) {
    state.current_line = current_line;
    state.current_column = current_column;
    state.token_line = g_token_line;
    state.token_column = g_token_column;
    state.error_line = g_error_line;
    state.error_column = g_error_column;
    state.flow_line = g_flow_line;
    state.flow_column = g_flow_column;
    state.error_count = error_count;
    state.lexical_line = lexical_error_line;
    state.source_name = source_name;
    state.package_name = package_name;
    state.lines = source_lines;
    state.call_stack = call_stack;
}

void diag_restore(const diag_state &state) {
    current_line = state.current_line;
    current_column = state.current_column;
    g_token_line = state.token_line;
    g_token_column = state.token_column;
    g_error_line = state.error_line;
    g_error_column = state.error_column;
    g_flow_line = state.flow_line;
    g_flow_column = state.flow_column;
    error_count = state.error_count;
    lexical_error_line = state.lexical_line;
    source_name = state.source_name;
    package_name = state.package_name;
    source_lines = state.lines;
    call_stack = state.call_stack;
}

void diag_start_source() {
    current_line = 1;
    current_column = 1;
    g_token_line = 1;
    g_token_column = 1;
    g_error_line = 0;
    g_error_column = 0;
    g_flow_line = 0;
    g_flow_column = 0;
    lexical_error_line = 0;
    source_name.clear();
    source_lines.clear();
}
