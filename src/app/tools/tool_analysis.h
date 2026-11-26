#ifndef SKEIN_TOOL_ANALYSIS_H
#define SKEIN_TOOL_ANALYSIS_H

#include <string>
#include <vector>

#include "diag_collect.h"
#include "typecheck.h"

struct analysis_options {
    std::string source_name;
    std::string encoding;
    std::vector<std::string> package_dirs;
    skein::typecheck::Mode typecheck_mode = skein::typecheck::Mode::Warn;
    bool keep_going = true;
    bool suppress_text = false;
    /* Syntax-only callers can disable imported macro resolution so they never
     * touch package files. */
    bool resolve_macros = true;
};

struct analysis_symbol {
    std::string name;
    std::string type;
    bool callable = false;
};

struct analysis_result {
    int error_count = 0;
    std::vector<diagnostic_record> diagnostics;
    /* Symbols visible after the check-only parse.  Used by LSP completion and
     * hover; values are copied before the analysis scope is torn down. */
    std::vector<analysis_symbol> symbols;
};

/* Analyze decoded UTF-8 source text without executing runtime-effect
 * statements.  The caller is responsible for reading/decoding the file. */
analysis_result analyze_source_text(const std::string &source,
                                    const analysis_options &options);

#endif
