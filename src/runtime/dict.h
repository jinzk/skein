#ifndef SKEIN_DICT_H
#define SKEIN_DICT_H

#include "valuetype.h"

#include <cstdint>

bool dict_key_allowed(const struct value_t *key);
const char *dict_key_error();
std::uint64_t dict_key_hash(const struct value_t *key);
int dict_find_index(const struct dict_value_t *dict,
                    const struct value_t *key);
void dict_append_item(struct dict_value_t *dict, struct value_t *key,
                      struct value_t *value);
void dict_invalidate_index(struct dict_value_t *dict);

#endif
