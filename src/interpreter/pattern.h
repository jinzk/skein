#ifndef SKEIN_PATTERN_H
#define SKEIN_PATTERN_H

#include <string>
#include <vector>

struct ast;
struct value_t;

struct pattern_binding {
    std::string name;
    struct value_t *value;
};

void free_pattern_bindings(std::vector<pattern_binding> &bindings);
bool bind_pattern_value(std::vector<pattern_binding> &bindings,
                        const char *name, struct value_t *value);
bool match_pattern(struct ast *pattern, struct value_t *value,
                   std::vector<pattern_binding> &bindings);

/* Returns false after reporting a missing enum variant (or another
 * exhaustiveness error).  Non-enum subjects are always considered fine. */
bool match_exhaustive_check(struct ast *arms, struct value_t *subject);

#endif
