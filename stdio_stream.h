#ifndef SKEIN_STDIO_STREAM_H
#define SKEIN_STDIO_STREAM_H

#include <cstdio>

#include "valuetype.h"

struct value_t *new_stdio_stream_value(std::FILE *file, const char *name,
                                       bool readable, bool writable);

#endif
