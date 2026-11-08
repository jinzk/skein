#ifndef SKEIN_THREAD_CHANNEL_H
#define SKEIN_THREAD_CHANNEL_H

#include "valuetype.h"

#include <vector>

struct value_t *call_thread_channel_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
