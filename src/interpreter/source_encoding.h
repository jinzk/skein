#ifndef SKEIN_SOURCE_ENCODING_H
#define SKEIN_SOURCE_ENCODING_H

#include <cstdio>
#include <string>

/* Encoding selected with --encoding; empty means auto-detect BOM or UTF-8. */
void source_encoding_set(const std::string &encoding);
const std::string &source_encoding_get();

/* Decode raw source bytes to UTF-8, honoring BOM and the forced encoding. */
bool decode_source_bytes(const std::string &raw,
                         const std::string &forced_encoding,
                         std::string &text, std::string &error);

/* Read a source file and decode it to UTF-8. */
bool load_source_file(const std::string &path,
                      const std::string &forced_encoding,
                      std::string &text, std::string &error);

/* Create a temporary FILE* containing decoded UTF-8 source. */
FILE *source_text_file(const std::string &text, std::string &error);

#endif
