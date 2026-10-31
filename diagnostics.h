#ifndef CALC_DIAGNOSTICS_H
#define CALC_DIAGNOSTICS_H

#include <string>
#include <vector>

/* Location of the most recently scanned token. */
extern int g_token_line;
extern int g_token_column;

/* Current runtime error location, set while evaluating an AST node. */
extern int g_error_line;
extern int g_error_column;

/* Location of the most recent break/continue value. */
extern int g_flow_line;
extern int g_flow_column;

void diag_set_flow_location(int line, int column);

/* Called by the scanner before each rule action. */
void diag_scan_token(const char *text, int length);

/* Remember a physical source line for snippets. */
void diag_record_line(int line, const std::string &text);

/* Runtime location handling. */
void diag_set_location(int line, int column);
int diag_current_line();
int diag_current_column();
void diag_start_input_line();

/* Report an error and print the source snippet and call stack. */
void diag_report(int line, int column, const char *category,
                 const char *message);
void diag_set_source_name(const std::string &name);
const std::string &diag_current_source_name();
bool diag_lexical_error_on_line(int line);
void diag_mark_script_failure();

/* Lexical error with printf-style formatting. */
void diag_report_lexical(int line, int column, const char *fmt, ...);

/* Runtime call stack. */
void diag_push_call(const std::string &name, int line, int column);
void diag_pop_call();

/* Total number of reported errors. */
int diag_error_count();

/* Save/restore diagnostics state while parsing an imported source file. */
struct diag_source_checkpoint {
    int current_line;
    int current_column;
    int token_line;
    int token_column;
    int error_line;
    int error_column;
    int flow_line;
    int flow_column;
    int lexical_line;
    std::string source_name;
    std::vector<std::string> lines;
};

void diag_save_source(diag_source_checkpoint &checkpoint);
void diag_restore_source(const diag_source_checkpoint &checkpoint);
void diag_start_source();

#endif
