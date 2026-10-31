#include "diagnostics.h"
#include "repl.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

int g_token_line = 1;
int g_token_column = 1;
int g_error_line = 0;
int g_error_column = 0;
int g_flow_line = 0;
int g_flow_column = 0;

static int current_line = 1;
static int current_column = 1;
static int error_count = 0;
static int lexical_error_line = 0;
static std::string source_name;
static std::vector<std::string> source_lines;

struct call_frame {
    std::string name;
    int line;
    int column;
};

static std::vector<call_frame> call_stack;
static const size_t MAX_REPORTED_FRAMES = 8;

void diag_scan_token(const char *text, int length) {
    g_token_line = current_line;
    g_token_column = current_column;
    for (int i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            ++current_line;
            current_column = 1;
        } else {
            ++current_column;
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
    for (int i = 1; i < column; ++i) {
        char c = (size_t)(i - 1) < text.size() ? text[(size_t)(i - 1)] : ' ';
        std::fputc(c == '\t' ? '\t' : ' ', stderr);
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
        const call_frame &frame = call_stack[i - 1];
        if (frame.line > 0)
            std::fprintf(stderr, "  in %s() called from line %d\n",
                         frame.name.c_str(), frame.line);
        else
            std::fprintf(stderr, "  in %s()\n", frame.name.c_str());
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

void diag_push_call(const std::string &name, int line, int column) {
    call_stack.push_back(call_frame{name, line, column});
}

void diag_pop_call() {
    if (!call_stack.empty())
        call_stack.pop_back();
}

int diag_error_count() {
    return error_count;
}

void diag_set_source_name(const std::string &name) {
    source_name = name;
}

const std::string &diag_current_source_name() {
    return source_name;
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
    source_lines = checkpoint.lines;
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
