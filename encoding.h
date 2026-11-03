#ifndef SKEIN_ENCODING_H
#define SKEIN_ENCODING_H

#include "valuetype.h"

#include <string>
#include <vector>

/* Canonical name for an encoding, or an empty string when unknown. */
std::string canonical_encoding_name(const std::string &name);

/* Decode/encode text with an explicit errors mode. */
bool decode_text(const std::string &input, const std::string &encoding,
                 const std::string &errors, std::string &output,
                 std::string &error);
bool encode_text(const std::string &input, const std::string &encoding,
                 const std::string &errors, std::string &output,
                 std::string &error);

/* Decode strict text to UTF-8.  Used by the source loader. */
bool decode_to_utf8(const std::string &input, const std::string &encoding,
                    std::string &output, std::string &error);

struct value_t *call_encoding_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
