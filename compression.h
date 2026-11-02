#ifndef CALC_COMPRESSION_H
#define CALC_COMPRESSION_H

#include <string>

/* Encodings this build can decode, for Accept-Encoding. */
std::string compression_accept_encoding();

/* Decode a Content-Encoding value (possibly a comma-separated list). */
bool decompress_content_encoding(const std::string &encoding,
                                 const std::string &input,
                                 std::string &output, std::string &error);

#endif
