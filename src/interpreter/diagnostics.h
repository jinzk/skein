#ifndef SKEIN_DIAGNOSTICS_H
#define SKEIN_DIAGNOSTICS_H

#include <string>
#include <vector>

/* Location of the most recently scanned token. */
extern thread_local int g_token_line;
extern thread_local int g_token_column;

/* Current runtime error location, set while evaluating an AST node. */
extern thread_local int g_error_line;
extern thread_local int g_error_column;

/* Location of the most recent break/continue value. */
extern thread_local int g_flow_line;
extern thread_local int g_flow_column;

void diag_set_flow_location(int line, int column);

/* Called by the scanner before each rule action. */
void diag_scan_token(const char *text, int length);

/* Undo column advancement for text that the scanner returns to the input
 * with yyless(). */
void diag_scan_rewind(const char *text, int length);

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

/* Print a warning without counting it as an error or stopping the script. */
void diag_warning(const char *message);
/* Location-aware warning used by the static type checker. */
void diag_report_warning(int line, int column, const char *category,
                         const char *message);
void diag_set_source_name(const std::string &name);
const std::string &diag_current_source_name();
void diag_set_package_name(const std::string &name);
const std::string &diag_current_package_name();
bool diag_lexical_error_on_line(int line);
void diag_mark_script_failure();

/* Lexical error with printf-style formatting. */
void diag_report_lexical(int line, int column, const char *fmt, ...);

/* Runtime call stack. */
void diag_push_call(const char *name, int line, int column);
void diag_pop_call();
int diag_call_depth();

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
    std::string package_name;
    std::vector<std::string> lines;
};

void diag_save_source(diag_source_checkpoint &checkpoint);
void diag_restore_source(const diag_source_checkpoint &checkpoint);
void diag_start_source();

struct diag_call_frame {
    const char *name;
    int line;
    int column;
};

struct diag_state {
    int current_line = 1;
    int current_column = 1;
    int token_line = 1;
    int token_column = 1;
    int error_line = 0;
    int error_column = 0;
    int flow_line = 0;
    int flow_column = 0;
    int error_count = 0;
    int lexical_line = 0;
    std::string source_name;
    std::string package_name;
    std::vector<std::string> lines;
    std::vector<diag_call_frame> call_stack;
};

void diag_capture(diag_state &state);
void diag_restore(const diag_state &state);

#endif
