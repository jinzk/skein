#ifndef SKEIN_UNICODE_H
#define SKEIN_UNICODE_H

#include "valuetype.h"

#include <string>
#include <vector>

/* NFC-normalize `input`.  Used by the lexer for identifiers. */
bool normalize_nfc(const std::string &input, std::string &output,
                   std::string &error);

/* Encode one Unicode scalar value as UTF-8. */
bool utf8_encode(unsigned int codepoint, std::string &out);

/* Decode one UTF-8 scalar value.  Returns false for invalid input. */
bool utf8_decode_one(const char *data, size_t size, size_t &consumed,
                     unsigned int &codepoint);

struct value_t *call_unicode_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
