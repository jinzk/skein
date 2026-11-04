#ifndef SKEIN_JSON_H
#define SKEIN_JSON_H

#include "valuetype.h"

#include <string>
#include <vector>

struct value_t *call_json_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Parse a JSON document without reporting through yyerror.  Returns null and
 * fills `error` on failure. */
struct value_t *json_parse_text(const std::string &text, std::string &error);

#endif
