#ifndef SKEIN_LSP_POSITION_H
#define SKEIN_LSP_POSITION_H

#include <cstddef>
#include <string>

namespace skein {
namespace lsp {

struct lsp_position {
    int line = 0;
    int character = 0;
};

/* Convert an LSP Position (0-based line, UTF-16 code unit offset) into a byte
 * offset in UTF-8 text.  CRLF is treated as a single line break. */
std::size_t byte_offset_from_position(const std::string &text,
                                      const lsp_position &position);

/* Convert a byte offset in UTF-8 text into an LSP Position. */
lsp_position position_from_byte_offset(const std::string &text,
                                       std::size_t byte_offset);

/* UTF-16 length of one line, excluding its line terminator. */
int line_utf16_length(const std::string &text, int line);

/* Decode one UTF-8 scalar.  Returns the byte length (1-4); invalid bytes are
 * consumed one at a time. */
std::size_t decode_utf8(const std::string &text, std::size_t pos,
                        unsigned int &codepoint);

} // namespace lsp
} // namespace skein

#endif
