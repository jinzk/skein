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

bool truthy(const struct value_t *v);
bool value_equals(struct value_t *a, struct value_t *b);
int compare_numeric(const struct value_t *a, const struct value_t *b,
                    bool &unordered);

#endif
