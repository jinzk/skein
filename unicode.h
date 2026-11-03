#ifndef SKEIN_UNICODE_H
#define SKEIN_UNICODE_H

#include "valuetype.h"

#include <string>
#include <vector>

/* NFC-normalize `input`.  Used by the lexer for identifiers. */
bool normalize_nfc(const std::string &input, std::string &output,
                   std::string &error);

struct value_t *call_unicode_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
