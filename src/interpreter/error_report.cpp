#include "error_report.h"

#include "diagnostics.h"
#include "eval_context.h"

#include <cstring>

void runtime_error_report(const char *message) {
    const char *text = message ? message : "";
    const char *category = "runtime error";
    if (std::strncmp(text, "syntax error", 12) == 0) {
        category = "syntax error";
        text += 12;
        while (*text == ',' || *text == ' ')
            ++text;
        if (*text == '\0')
            text = "invalid syntax";
    }

    eval_context &context = active_eval_context();
    ++context.runtime_error_count;
    if (context.capture_errors &&
        std::strcmp(category, "runtime error") == 0) {
        if (!context.has_captured_error) {
            context.has_captured_error = true;
            context.captured_error = text;
        }
        return;
    }

    diag_report(g_error_line, g_error_column, category, text);
}
