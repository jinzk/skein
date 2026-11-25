#ifndef SKEIN_VALUE_OPS_H
#define SKEIN_VALUE_OPS_H

#include "valuetype.h"

#include <cstddef>
#include <string>

bool is_numeric(const struct value_t *v);
double as_double(const struct value_t *v);

size_t utf8_sequence_length(unsigned char c);
size_t utf8_valid_sequence_length(const char *s, size_t available);
size_t utf8_length(const char *s);
bool utf8_char_at(const char *s, size_t index, std::string &out);

/* Cached codepoint operations for runtime string values.  Non-ASCII strings
 * above the sparse-index threshold build a sampled byte-offset table lazily;
 * ASCII strings and short strings are always handled directly. */
size_t string_length(const struct string_value_t *text);
bool string_codepoint_at(const struct string_value_t *text, size_t index,
                         unsigned int &codepoint);
void string_index_invalidate(struct string_value_t *text);

bool truthy(const struct value_t *v);
bool value_equals(struct value_t *a, struct value_t *b);
int compare_numeric(const struct value_t *a, const struct value_t *b,
                    bool &unordered);

#endif
