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

/*
 * Track whether the last stdout write left the terminal in the middle of a
 * line.  The REPL uses this to start the next prompt on a fresh line without
 * changing `print` output semantics.  The `_locked` variant requires the
 * caller to hold stdio_output_mutex().
 */
void note_stdout_bytes(const char *data, std::size_t size);
void note_stdout_bytes_locked(const char *data, std::size_t size);
void ensure_stdout_newline();

struct value_t *new_stdio_stream_value(std::FILE *file, const char *name,
                                       bool readable, bool writable);

#endif
