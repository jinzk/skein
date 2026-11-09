#ifndef SKEIN_STDIO_STREAM_H
#define SKEIN_STDIO_STREAM_H

#include <cstdio>
#include <mutex>

#include "valuetype.h"

/*
 * Serializes process-wide stdout/stderr writes. Worker threads share the
 * underlying FILE objects, so a caller that emits a logical line must hold
 * this lock across all of its writes.
 */
std::mutex &stdio_output_mutex();

struct value_t *new_stdio_stream_value(std::FILE *file, const char *name,
                                       bool readable, bool writable);

#endif
