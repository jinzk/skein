#ifndef SKEIN_ERROR_REPORT_H
#define SKEIN_ERROR_REPORT_H

#include "diag_collect.h"

#include <vector>

/* Interpreter-side handler installed into skein_context::error. */
void runtime_error_report(const char *message);

/* Secondary locations for the next error reported on this thread.  Used by
 * user macro expansion to point at a template definition site.  The pending
 * value is consumed (and cleared) by runtime_error_report(). */
void runtime_error_set_related(
    const std::vector<diagnostic_related> &related);
void runtime_error_clear_related();

#endif
