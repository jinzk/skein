#include "position.h"

#include <algorithm>

namespace skein {
namespace lsp {

namespace {

bool is_continuation(unsigned char c) { return (c & 0xc0) == 0x80; }

std::size_t line_start(const std::string &text, int line) {
    if (line <= 0)
        return 0;
    std::size_t pos = 0;
    int current = 0;
    while (pos < text.size() && current < line) {
        if (text[pos] == '\r' && pos + 1 < text.size() &&
            text[pos + 1] == '\n') {
            pos += 2;
        } else if (text[pos] == '\r' || text[pos] == '\n') {
            ++pos;
        } else {
            ++pos;
            continue;
        }
        ++current;
    }
    return pos;
}

std::size_t line_end(const std::string &text, int line) {
    std::size_t pos = line_start(text, line);
    while (pos < text.size() && text[pos] != '\r' && text[pos] != '\n')
        ++pos;
    return pos;
}

int utf16_units(unsigned int codepoint) { return codepoint > 0xffff ? 2 : 1; }

} // namespace

std::size_t decode_utf8(const std::string &text, std::size_t pos,
                        unsigned int &codepoint) {
    if (pos >= text.size()) {
        codepoint = 0;
        return 0;
    }
    const unsigned char first = (unsigned char)text[pos];
    if (first < 0x80) {
        codepoint = first;
        return 1;
    }

    std::size_t length = 0;
    unsigned int value = 0;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        value = first & 0x07;
    } else {
        codepoint = first;
        return 1;
    }
    if (pos + length > text.size()) {
        codepoint = first;
        return 1;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char c = (unsigned char)text[pos + i];
        if (!is_continuation(c)) {
            codepoint = first;
            return 1;
        }
        value = (value << 6) | (c & 0x3f);
    }
    if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
        (length == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        codepoint = first;
        return 1;
    }
    codepoint = value;
    return length;
}

std::size_t byte_offset_from_position(const std::string &text,
                                      const lsp_position &position) {
    std::size_t pos = line_start(text, position.line);
    const std::size_t end = line_end(text, position.line);
    int character = 0;
    while (pos < end && character < position.character) {
        unsigned int codepoint = 0;
        const std::size_t length = decode_utf8(text, pos, codepoint);
        pos += length;
        character += utf16_units(codepoint);
    }
    return pos;
}

lsp_position position_from_byte_offset(const std::string &text,
                                       std::size_t byte_offset) {
    byte_offset = std::min(byte_offset, text.size());
    lsp_position result;
    std::size_t pos = 0;
    while (pos < byte_offset) {
        if (text[pos] == '\r' && pos + 1 < text.size() &&
            text[pos + 1] == '\n') {
            if (pos + 2 > byte_offset)
                break;
            pos += 2;
        } else if (text[pos] == '\n' || text[pos] == '\r') {
            ++pos;
        } else {
            unsigned int codepoint = 0;
            pos += decode_utf8(text, pos, codepoint);
            result.character += utf16_units(codepoint);
            continue;
        }
        ++result.line;
        result.character = 0;
    }
    return result;
}

int line_utf16_length(const std::string &text, int line) {
    std::size_t pos = line_start(text, line);
    const std::size_t end = line_end(text, line);
    int units = 0;
    while (pos < end) {
        unsigned int codepoint = 0;
        pos += decode_utf8(text, pos, codepoint);
        units += utf16_units(codepoint);
    }
    return units;
}

} // namespace lsp
} // namespace skein
