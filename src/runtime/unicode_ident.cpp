#include "unicode_ident.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace unicode_ident {
namespace {

struct unicode_range {
    uint32_t first;
    uint32_t last;
};

#include "unicode_ident_data.inc"

bool in_ranges(const unicode_range *ranges, size_t count, uint32_t cp) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first)
            hi = mid;
        else if (cp > ranges[mid].last)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

bool ascii_start(uint32_t cp) {
    return cp == '_' || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z');
}

bool ascii_continue(uint32_t cp) {
    return ascii_start(cp) || (cp >= '0' && cp <= '9');
}

}  // namespace

decode_result decode(const char *text, size_t size) {
    decode_result result;
    if (!text || size == 0)
        return result;

    const unsigned char *s = reinterpret_cast<const unsigned char *>(text);
    unsigned char b0 = s[0];

    if (b0 < 0x80) {
        result.codepoint = b0;
        result.length = 1;
        result.valid = true;
        return result;
    }

    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (size < 2 || (s[1] & 0xC0) != 0x80)
            return result;
        result.codepoint = ((uint32_t)(b0 & 0x1F) << 6) |
                           (uint32_t)(s[1] & 0x3F);
        result.length = 2;
        result.valid = true;
        return result;
    }

    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (size < 3 || (s[1] & 0xC0) != 0x80 ||
            (s[2] & 0xC0) != 0x80)
            return result;
        if (b0 == 0xE0 && s[1] < 0xA0)
            return result;  // overlong
        if (b0 == 0xED && s[1] >= 0xA0)
            return result;  // surrogate
        result.codepoint = ((uint32_t)(b0 & 0x0F) << 12) |
                           ((uint32_t)(s[1] & 0x3F) << 6) |
                           (uint32_t)(s[2] & 0x3F);
        result.length = 3;
        result.valid = true;
        return result;
    }

    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (size < 4 || (s[1] & 0xC0) != 0x80 ||
            (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80)
            return result;
        if (b0 == 0xF0 && s[1] < 0x90)
            return result;  // overlong
        if (b0 == 0xF4 && s[1] >= 0x90)
            return result;  // beyond U+10FFFF
        result.codepoint = ((uint32_t)(b0 & 0x07) << 18) |
                           ((uint32_t)(s[1] & 0x3F) << 12) |
                           ((uint32_t)(s[2] & 0x3F) << 6) |
                           (uint32_t)(s[3] & 0x3F);
        result.length = 4;
        result.valid = true;
        return result;
    }

    return result;
}

bool is_start(uint32_t codepoint) {
    if (ascii_start(codepoint))
        return true;
    return in_ranges(kXidStartRanges,
                     sizeof(kXidStartRanges) / sizeof(kXidStartRanges[0]),
                     codepoint);
}

bool is_continue(uint32_t codepoint) {
    if (ascii_continue(codepoint))
        return true;
    return in_ranges(kXidContinueRanges,
                     sizeof(kXidContinueRanges) / sizeof(kXidContinueRanges[0]),
                     codepoint);
}

scan_result scan_identifier(const char *text, size_t size) {
    scan_result result;
    if (!text || size == 0) {
        result.bad_length = 1;
        return result;
    }

    size_t pos = 0;
    bool first = true;
    while (pos < size) {
        decode_result decoded = decode(text + pos, size - pos);
        if (!decoded.valid) {
            result.valid_bytes = pos;
            result.bad_offset = pos;
            result.bad_length = 1;
            return result;
        }
        bool ok = first ? is_start(decoded.codepoint)
                        : is_continue(decoded.codepoint);
        if (!ok) {
            result.valid_bytes = pos;
            result.bad_offset = pos;
            result.bad_length = decoded.length;
            result.bad_codepoint = decoded.codepoint;
            return result;
        }
        pos += decoded.length;
        first = false;
    }

    result.valid = !first;
    result.valid_bytes = size;
    return result;
}

bool is_identifier(const std::string &text) {
    return scan_identifier(text.data(), text.size()).valid;
}

size_t display_width(uint32_t codepoint) {
    if (codepoint == '\t')
        return 1;
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0))
        return 0;
    if (in_ranges(kZeroWidthRanges,
                  sizeof(kZeroWidthRanges) / sizeof(kZeroWidthRanges[0]),
                  codepoint))
        return 0;
    if (in_ranges(kWideRanges,
                  sizeof(kWideRanges) / sizeof(kWideRanges[0]),
                  codepoint))
        return 2;
    return 1;
}

size_t display_width(const char *text, size_t size) {
    size_t width = 0;
    size_t pos = 0;
    while (pos < size) {
        decode_result decoded = decode(text + pos, size - pos);
        if (!decoded.valid) {
            ++width;
            ++pos;
            continue;
        }
        width += display_width(decoded.codepoint);
        pos += decoded.length;
    }
    return width;
}

const char *unicode_version() {
    return kUnicodeVersion;
}

}  // namespace unicode_ident
