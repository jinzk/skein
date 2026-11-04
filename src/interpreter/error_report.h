#ifndef SKEIN_ERROR_REPORT_H
#define SKEIN_ERROR_REPORT_H

/* Interpreter-side handler installed into skein_context::error. */
void runtime_error_report(const char *message);

#endif
