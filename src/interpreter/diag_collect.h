#ifndef SKEIN_DIAG_COLLECT_H
#define SKEIN_DIAG_COLLECT_H

#include <string>
#include <vector>

enum class DiagnosticSeverity { Error, Warning, Information, Hint };

struct diagnostic_record {
    std::string source;
    int line = 0;
    int column = 0;
    std::size_t byte_offset = 0;
    std::size_t utf16_offset = 0;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string category;
    std::string message;
};

/* Start collecting structured diagnostics.  When suppress_text is true,
 * diag_report()/diag_report_warning() do not print their usual text format. */
void diag_collect_begin(bool suppress_text);
std::vector<diagnostic_record> diag_collect_end();
bool diag_collect_active();
bool diag_collect_suppress_text();
void diag_collect_push(const diagnostic_record &record);

const char *diag_code_for_category(const char *category);

#endif
