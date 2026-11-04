#include "runtime_context.h"

namespace {
thread_local skein_context *current_context = nullptr;
}

skein_context *skein_context_current() {
    return current_context;
}

void skein_context_set(skein_context *ctx) {
    current_context = ctx;
}
