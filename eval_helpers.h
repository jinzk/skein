#ifndef _EVAL_HELPERS_H_
#define _EVAL_HELPERS_H_

#include "valuetype.h"

#include <cstddef>
#include <string>
#include <vector>

bool is_numeric(const struct value_t *v);
struct value_t *new_int_checked(__int128 n);
double as_double(const struct value_t *v);

size_t utf8_sequence_length(unsigned char c);
size_t utf8_valid_sequence_length(const char *s, size_t available);
size_t utf8_length(const char *s);
bool utf8_char_at(const char *s, size_t index, std::string &out);

bool truthy(const struct value_t *v);
bool value_equals(struct value_t *a, struct value_t *b);
int compare_numeric(const struct value_t *a, const struct value_t *b,
                    bool &unordered);
struct value_t *call_value_args(struct value_t *callee,
                                std::vector<struct value_t *> args);

#endif
