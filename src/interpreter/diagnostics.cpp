#include "diagnostics.h"
#include "diag_collect.h"
#include "runtime_state.h"
#include "unicode_ident.h"
#include "valuetype.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
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
static thread_local std::string module_name;
static thread_local std::vector<std::string> source_lines;

static thread_local std::vector<diag_call_frame> call_stack;
static const size_t MAX_REPORTED_FRAMES = 8;

/* Source paths are interned so call frames only store a pointer.  std::deque
 * keeps addresses stable as more files are loaded. */
static thread_local std::deque<std::string> interned_sources;

const std::string *diag_intern_source(const std::string &source) {
    for (const std::string &existing : interned_sources) {
        if (existing == source)
            return &existing;
    }
    interned_sources.push_back(source);
    return &interned_sources.back();
}

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

static void diagnostic_offsets(int line, int column, std::size_t &byte_offset,
                               std::size_t &utf16_offset) {
    byte_offset = 0;
    utf16_offset = 0;
    if (line <= 0 || (std::size_t)line > source_lines.size())
        return;
    const std::string &text = source_lines[(std::size_t)line - 1];
    std::size_t pos = 0;
    int col = 1;
    while (pos < text.size() && col < column) {
        unsigned char c = (unsigned char)text[pos];
        if (c == '\n')
            break;
        if (c < 0x80) {
            ++pos;
            ++col;
            ++utf16_offset;
        } else {
            unicode_ident::decode_result decoded =
                unicode_ident::decode(text.data() + pos, text.size() - pos);
            if (!decoded.valid) {
                ++pos;
                ++col;
                ++utf16_offset;
            } else {
                std::size_t width =
                    unicode_ident::display_width(decoded.codepoint);
                pos += decoded.length;
                col += (int)width;
                utf16_offset += decoded.codepoint > 0xffff ? 2 : 1;
            }
        }
    }
    byte_offset = pos;
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

static void print_trace(const std::vector<error_frame> &frames) {
    if (frames.empty())
        return;
    size_t shown = frames.size();
    if (shown > MAX_REPORTED_FRAMES) {
        std::fprintf(stderr, "  ... %zu more call(s)\n",
                     shown - MAX_REPORTED_FRAMES);
        shown = MAX_REPORTED_FRAMES;
    }
    for (size_t i = 0; i < shown; ++i) {
        const error_frame &frame = frames[i];
        if (frame.line > 0)
            std::fprintf(stderr, "  in %s() called from line %d\n",
                         frame.function.c_str(), frame.line);
        else
            std::fprintf(stderr, "  in %s()\n", frame.function.c_str());
    }
}

void diag_mark_script_failure() {
    if (!g_interactive && !g_keep_going && g_package_loading == 0)
        g_script_stop = true;
}

static void diag_report_at(const std::string &source, int line, int column,
                           const char *category, const char *message,
                           const std::vector<error_frame> &trace) {
    if (g_interrupted)
        return;

    ++error_count;
    diag_mark_script_failure();

    if (line <= 0)
        line = g_token_line;
    if (column <= 0)
        column = g_token_column;

    if (diag_collect_active()) {
        diagnostic_record record;
        record.source = source;
        record.line = line;
        record.column = column;
        diagnostic_offsets(line, column, record.byte_offset,
                           record.utf16_offset);
        record.severity = DiagnosticSeverity::Error;
        record.code = diag_code_for_category(category);
        record.category = category ? category : "";
        record.message = message ? message : "";
        diag_collect_push(record);
        if (diag_collect_suppress_text())
            return;
    }

    std::fflush(stdout);
    if (!source.empty()) {
        if (column > 0)
            std::fprintf(stderr, "%s:%d:%d: %s: %s\n", source.c_str(),
                         line, column, category, message);
        else
            std::fprintf(stderr, "%s:%d: %s: %s\n", source.c_str(),
                         line, category, message);
    } else if (column > 0) {
        std::fprintf(stderr, "%d:%d: %s: %s\n", line, column, category,
                     message);
    } else {
        std::fprintf(stderr, "%d: %s: %s\n", line, category, message);
    }

    /* The snippet belongs to the file that is currently loaded; skip it when
       the error was raised in a different source. */
    if (source == source_name)
        print_snippet(line, column);
    print_trace(trace);
    std::fflush(stderr);
}

void diag_report(int line, int column, const char *category,
                 const char *message) {
    std::vector<error_frame> trace;
    diag_snapshot_trace(trace);
    diag_report_at(source_name, line, column, category, message, trace);
}

void diag_report_error(const struct error_value_t &error, const char *category,
                       const char *message) {
    const bool has_location = error.line > 0;
    const std::string &source =
        error.source.empty() ? source_name : error.source;
    diag_report_at(source, has_location ? error.line : g_error_line,
                   has_location ? error.column : g_error_column, category,
                   message, error.trace);
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

    if (diag_collect_active()) {
        diagnostic_record record;
        record.source = source_name;
        record.line = line;
        record.column = column;
        diagnostic_offsets(line, column, record.byte_offset,
                           record.utf16_offset);
        record.severity = DiagnosticSeverity::Warning;
        record.code = diag_code_for_category(category);
        record.category = category && *category ? category : "warning";
        record.message = message ? message : "";
        diag_collect_push(record);
        if (diag_collect_suppress_text())
            return;
    }

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

void diag_push_call(const char *name, const std::string *source, int line,
                    int column) {
    diag_call_frame frame;
    frame.name = name ? name : "<anonymous>";
    frame.source = source;
    frame.line = line;
    frame.column = column;
    call_stack.push_back(frame);
}

void diag_pop_call() {
    if (!call_stack.empty())
        call_stack.pop_back();
}

int diag_call_depth() { return (int)call_stack.size(); }

bool diag_current_call_site(int &line, int &column) {
    if (call_stack.empty())
        return false;
    const diag_call_frame &frame = call_stack.back();
    line = frame.line;
    column = frame.column;
    return true;
}

void diag_snapshot_trace(std::vector<error_frame> &frames, std::size_t limit) {
    frames.clear();
    if (call_stack.empty())
        return;
    std::size_t shown = call_stack.size();
    if (limit > 0 && shown > limit)
        shown = limit;
    frames.reserve(shown);
    for (std::size_t i = call_stack.size(); i > call_stack.size() - shown;
         --i) {
        const diag_call_frame &frame = call_stack[i - 1];
        error_frame entry;
        entry.source = frame.source ? *frame.source : std::string();
        entry.function = frame.name ? frame.name : "<anonymous>";
        entry.line = frame.line;
        entry.column = frame.column;
        frames.push_back(std::move(entry));
    }
}

int diag_error_count() {
    return error_count;
}

void diag_reset_errors() { error_count = 0; }

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

void diag_set_module_name(const std::string &name) {
    module_name = name;
}

const std::string &diag_current_module_name() {
    return module_name;
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
    checkpoint.module_name = module_name;
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
    module_name = checkpoint.module_name;
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
    state.module_name = module_name;
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
    module_name = state.module_name;
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
