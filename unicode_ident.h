#ifndef SKEIN_UNICODE_IDENT_H
#define SKEIN_UNICODE_IDENT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace unicode_ident {

struct decode_result {
    uint32_t codepoint = 0;
    size_t length = 1;
    bool valid = false;
};

struct scan_result {
    bool valid = false;
    size_t valid_bytes = 0;
    size_t bad_offset = 0;
    size_t bad_length = 1;
    uint32_t bad_codepoint = 0;
};

/* Strict UTF-8 decoding.  Invalid bytes report length 1. */
decode_result decode(const char *text, size_t size);

/* Unicode XID_Start / XID_Continue, with '_' and ASCII handled by callers. */
bool is_start(uint32_t codepoint);
bool is_continue(uint32_t codepoint);

/* Scan a complete identifier candidate.  On failure, valid_bytes is the
 * length of the valid prefix before the first bad code point. */
scan_result scan_identifier(const char *text, size_t size);

/* True when text is a non-empty identifier. */
bool is_identifier(const std::string &text);

/* Terminal display width used by diagnostics (0, 1, or 2 columns). */
size_t display_width(uint32_t codepoint);
size_t display_width(const char *text, size_t size);

/* Unicode database version used to generate the tables. */
const char *unicode_version();

}  // namespace unicode_ident

#endif
