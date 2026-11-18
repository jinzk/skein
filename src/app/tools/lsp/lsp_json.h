#ifndef SKEIN_LSP_JSON_H
#define SKEIN_LSP_JSON_H

#include <string>

#include "valuetype.h"

namespace skein {
namespace lsp {
namespace json {

/* Borrowed lookup into a parsed JSON object.  The returned value is owned by
 * `object` and must not be freed by the caller. */
const struct value_t *member(const struct value_t *object, const char *key);

std::string string_value(const struct value_t *value,
                         const std::string &fallback = "");
long long int_value(const struct value_t *value, long long fallback = 0);
bool bool_value(const struct value_t *value, bool fallback = false);

/* Constructors for outbound JSON trees.  `object()`, `array()`, `string()`,
 * `integer()`, `boolean()` and `null()` return owned values.  `set()` and
 * `push()` take ownership of the child value. */
struct value_t *object();
struct value_t *array();
struct value_t *string(const std::string &text);
struct value_t *integer(long long value);
struct value_t *boolean(bool value);
struct value_t *null();
void set(struct value_t *object, const char *key, struct value_t *value);
void push(struct value_t *array, struct value_t *value);

std::string stringify(const struct value_t *value, std::string &error);

} // namespace json
} // namespace lsp
} // namespace skein

#endif
