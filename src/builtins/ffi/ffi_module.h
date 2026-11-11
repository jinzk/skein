#ifndef SKEIN_FFI_H
#define SKEIN_FFI_H

#include "valuetype.h"

#include <vector>

struct value_t *call_ffi_builtin(
    int functype, const std::vector<struct value_t *> &args);

namespace ffi {
/* Run callbacks queued by foreign threads; called from the event loop. */
void background_drain();
/* Fail callbacks owned by the exiting thread so foreign callers do not block. */
void background_discard_owned();
}

#endif
