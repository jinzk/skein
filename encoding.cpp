#include "encoding.h"

#include "ast.h"
#include "valuetype.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace {

constexpr size_t kTwoByteTableSize = 126 * 190;
constexpr size_t kReplacementCodepoint = 0xFFFDu;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

struct Gb18030Range {
    uint32_t first_index;
    uint32_t last_index;
    uint32_t first_codepoint;
};

#include "encoding_gb18030_data.inc"
#include "encoding_gbk_validity.inc"

enum class Encoding {
    Utf8,
    Ascii,
    Latin1,
    Windows1252,
    Utf16Le,
    Utf16Be,
    Utf16,
    Gbk,
    Gb18030,
};

enum class ErrorMode {
    Strict,
    Replace,
    Ignore,
};

enum class Utf8Status {
    Ok,
    Invalid,
    Truncated,
};

static std::string normalize_encoding_name(const std::string &name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (c == '_')
            c = '-';
        normalized.push_back((char)c);
    }
    return normalized;
}

static bool lookup_encoding(const std::string &name, Encoding &encoding) {
    const std::string normalized = normalize_encoding_name(name);
    if (normalized == "utf-8" || normalized == "utf8") {
        encoding = Encoding::Utf8;
        return true;
    }
    if (normalized == "ascii" || normalized == "us-ascii") {
        encoding = Encoding::Ascii;
        return true;
    }
    if (normalized == "latin-1" || normalized == "latin1" ||
        normalized == "iso-8859-1" || normalized == "iso8859-1") {
        encoding = Encoding::Latin1;
        return true;
    }
    if (normalized == "windows-1252" || normalized == "cp1252") {
        encoding = Encoding::Windows1252;
        return true;
    }
    if (normalized == "utf-16le" || normalized == "utf16le" ||
        normalized == "utf-16-le") {
        encoding = Encoding::Utf16Le;
        return true;
    }
    if (normalized == "utf-16be" || normalized == "utf16be" ||
        normalized == "utf-16-be") {
        encoding = Encoding::Utf16Be;
        return true;
    }
    if (normalized == "utf-16" || normalized == "utf16") {
        encoding = Encoding::Utf16;
        return true;
    }
    if (normalized == "gbk" || normalized == "cp936" ||
        normalized == "ms936" || normalized == "windows-936") {
        encoding = Encoding::Gbk;
        return true;
    }
    if (normalized == "gb18030" || normalized == "gb-18030") {
        encoding = Encoding::Gb18030;
        return true;
    }
    return false;
}

static bool parse_error_mode(const std::string &name, ErrorMode &mode) {
    if (name == "strict") {
        mode = ErrorMode::Strict;
        return true;
    }
    if (name == "replace") {
        mode = ErrorMode::Replace;
        return true;
    }
    if (name == "ignore") {
        mode = ErrorMode::Ignore;
        return true;
    }
    return false;
}

static void append_utf8(uint32_t codepoint, std::string &out) {
    if (codepoint <= 0x7F) {
        out.push_back((char)codepoint);
    } else if (codepoint <= 0x7FF) {
        out.push_back((char)(0xC0 | (codepoint >> 6)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back((char)(0xE0 | (codepoint >> 12)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (codepoint >> 18)));
        out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    }
}

static std::string codepoint_name(uint32_t codepoint) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "U+%04X", (unsigned)codepoint);
    return buffer;
}

static Utf8Status utf8_next(const std::string &data, size_t pos,
                            uint32_t &codepoint, size_t &length) {
    const unsigned char first = (unsigned char)data[pos];
    if (first < 0x80) {
        codepoint = first;
        length = 1;
        return Utf8Status::Ok;
    }

    size_t sequence = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (first >= 0xC2 && first <= 0xDF) {
        sequence = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        sequence = 3;
        if (first == 0xE0)
            low = 0xA0;
        else if (first == 0xED)
            high = 0x9F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        sequence = 4;
        if (first == 0xF0)
            low = 0x90;
        else if (first == 0xF4)
            high = 0x8F;
    } else {
        length = 1;
        return Utf8Status::Invalid;
    }

    if (pos + sequence > data.size()) {
        length = 1;
        return Utf8Status::Truncated;
    }

    const unsigned char second = (unsigned char)data[pos + 1];
    if (second < low || second > high) {
        length = 1;
        return Utf8Status::Invalid;
    }
    for (size_t i = 2; i < sequence; ++i) {
        if (((unsigned char)data[pos + i] & 0xC0) != 0x80) {
            length = 1;
            return Utf8Status::Invalid;
        }
    }

    if (sequence == 2) {
        codepoint = ((uint32_t)(first & 0x1F) << 6) |
                    (uint32_t)(second & 0x3F);
    } else if (sequence == 3) {
        codepoint = ((uint32_t)(first & 0x0F) << 12) |
                    ((uint32_t)(second & 0x3F) << 6) |
                    (uint32_t)((unsigned char)data[pos + 2] & 0x3F);
    } else {
        codepoint = ((uint32_t)(first & 0x07) << 18) |
                    ((uint32_t)(second & 0x3F) << 12) |
                    ((uint32_t)((unsigned char)data[pos + 2] & 0x3F) << 6) |
                    (uint32_t)((unsigned char)data[pos + 3] & 0x3F);
    }
    length = sequence;
    return Utf8Status::Ok;
}

static bool handle_decode_error(const char *encoding, size_t offset,
                                ErrorMode mode, std::string &out,
                                std::string &error) {
    if (mode == ErrorMode::Strict) {
        error = std::string("invalid ") + encoding +
                " sequence at byte offset " + std::to_string(offset);
        return false;
    }
    if (mode == ErrorMode::Replace)
        append_utf8(kReplacementCodepoint, out);
    return true;
}

static bool handle_unrepresentable(uint32_t codepoint, const char *encoding,
                                   ErrorMode mode, std::string &out,
                                   std::string &error) {
    if (mode == ErrorMode::Strict) {
        error = "character " + codepoint_name(codepoint) +
                " is not representable in " + encoding;
        return false;
    }
    if (mode == ErrorMode::Replace)
        out.push_back('?');
    return true;
}

static bool append_decoded_codepoint(uint32_t codepoint, std::string &out,
                                     std::string &error) {
    if (codepoint == 0) {
        error = "decoded text contains NUL (U+0000)";
        return false;
    }
    append_utf8(codepoint, out);
    return true;
}

static std::array<uint16_t, kTwoByteTableSize> decode_gb18030_table() {
    std::array<uint16_t, kTwoByteTableSize> table{};
    size_t table_index = 0;
    size_t delta_index = 0;
    int32_t codepoint = 0;
    const size_t run_count =
        sizeof(GB18030_RUN_LENGTHS) / sizeof(GB18030_RUN_LENGTHS[0]);
    const size_t delta_count =
        sizeof(GB18030_RUN_DELTAS) / sizeof(GB18030_RUN_DELTAS[0]);

    for (size_t run = 0; run < run_count; ++run) {
        uint32_t encoded = 0;
        unsigned shift = 0;
        for (;;) {
            if (delta_index >= delta_count)
                return {};
            const uint8_t byte = GB18030_RUN_DELTAS[delta_index++];
            encoded |= (uint32_t)(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
                break;
            shift += 7;
            if (shift > 28)
                return {};
        }

        int32_t delta = (int32_t)(encoded >> 1);
        if (encoded & 1)
            delta = -delta - 1;
        codepoint += delta;

        const size_t length = GB18030_RUN_LENGTHS[run];
        for (size_t i = 0; i < length; ++i) {
            if (table_index >= kTwoByteTableSize)
                return {};
            const uint32_t value = (uint32_t)codepoint + (uint32_t)i;
            if (value > 0xFFFF)
                return {};
            table[table_index++] = (uint16_t)value;
        }
    }

    if (table_index != kTwoByteTableSize)
        return {};
    return table;
}

static const std::array<uint16_t, kTwoByteTableSize> &gb18030_table() {
    static const std::array<uint16_t, kTwoByteTableSize> table =
        decode_gb18030_table();
    return table;
}

static bool gbk_valid(size_t index) {
    if (index >= kTwoByteTableSize)
        return false;
    return (GBK_VALID_BITS[index >> 3] & (1u << (index & 7))) != 0;
}

static size_t gb_index(unsigned char lead, unsigned char trail) {
    const size_t lead_index = (size_t)(lead - 0x81);
    const size_t trail_index =
        (size_t)(trail < 0x7F ? trail - 0x40 : trail - 0x41);
    return lead_index * 190 + trail_index;
}

static bool valid_gb_trail(unsigned char trail) {
    return (trail >= 0x40 && trail <= 0x7E) ||
           (trail >= 0x80 && trail <= 0xFE);
}

static bool gb18030_lookup_index(uint32_t index, uint32_t &codepoint) {
    size_t low = 0;
    size_t high = sizeof(GB18030_RANGES) / sizeof(GB18030_RANGES[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const Gb18030Range &range = GB18030_RANGES[middle];
        if (index < range.first_index) {
            high = middle;
        } else if (index > range.last_index) {
            low = middle + 1;
        } else {
            codepoint = range.first_codepoint + (index - range.first_index);
            return true;
        }
    }
    return false;
}

static bool gb18030_lookup_codepoint(uint32_t codepoint, uint32_t &index) {
    size_t low = 0;
    size_t high = sizeof(GB18030_RANGES) / sizeof(GB18030_RANGES[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const Gb18030Range &range = GB18030_RANGES[middle];
        if (codepoint < range.first_codepoint) {
            high = middle;
            continue;
        }
        const uint32_t last_codepoint =
            range.first_codepoint + (range.last_index - range.first_index);
        if (codepoint <= last_codepoint) {
            index = range.first_index + (codepoint - range.first_codepoint);
            return true;
        }
        low = middle + 1;
    }
    return false;
}

static const std::array<uint32_t, 65536> &gb18030_reverse() {
    static const std::array<uint32_t, 65536> reverse = [] {
        std::array<uint32_t, 65536> result;
        result.fill(kInvalidIndex);
        const auto &table = gb18030_table();
        for (uint32_t index = 0; index < kTwoByteTableSize; ++index) {
            const uint32_t codepoint = table[index];
            if (codepoint < result.size() &&
                result[codepoint] == kInvalidIndex) {
                result[codepoint] = index;
            }
        }
        return result;
    }();
    return reverse;
}

static void append_u16(uint16_t value, bool big_endian, std::string &out) {
    if (big_endian) {
        out.push_back((char)(value >> 8));
        out.push_back((char)(value & 0xFF));
    } else {
        out.push_back((char)(value & 0xFF));
        out.push_back((char)(value >> 8));
    }
}

static uint16_t read_u16(const std::string &data, size_t offset,
                         bool big_endian) {
    const unsigned char first = (unsigned char)data[offset];
    const unsigned char second = (unsigned char)data[offset + 1];
    if (big_endian)
        return (uint16_t)(((uint16_t)first << 8) | second);
    return (uint16_t)(((uint16_t)second << 8) | first);
}

static void append_gb18030_four_byte(uint32_t index, std::string &out) {
    const uint32_t lead = 0x81 + index / (10 * 126 * 10);
    index %= 10 * 126 * 10;
    const uint32_t second = 0x30 + index / (126 * 10);
    index %= 126 * 10;
    const uint32_t third = 0x81 + index / 10;
    const uint32_t fourth = 0x30 + index % 10;
    out.push_back((char)lead);
    out.push_back((char)second);
    out.push_back((char)third);
    out.push_back((char)fourth);
}

static bool decode_utf8(const std::string &input, ErrorMode mode,
                        std::string &out, std::string &error) {
    size_t pos = 0;
    while (pos < input.size()) {
        uint32_t codepoint = 0;
        size_t length = 1;
        const Utf8Status status = utf8_next(input, pos, codepoint, length);
        if (status == Utf8Status::Ok) {
            if (!append_decoded_codepoint(codepoint, out, error))
                return false;
            pos += length;
            continue;
        }
        if (!handle_decode_error("utf-8", pos, mode, out, error))
            return false;
        pos += length;
    }
    return true;
}

static bool decode_ascii(const std::string &input, ErrorMode mode,
                         std::string &out, std::string &error) {
    for (size_t pos = 0; pos < input.size(); ++pos) {
        const unsigned char byte = (unsigned char)input[pos];
        if (byte < 0x80) {
            if (!append_decoded_codepoint(byte, out, error))
                return false;
        } else {
            if (!handle_decode_error("ascii", pos, mode, out, error))
                return false;
        }
    }
    return true;
}

static bool decode_latin1(const std::string &input, ErrorMode mode,
                          std::string &out, std::string &error) {
    (void)mode;
    for (size_t pos = 0; pos < input.size(); ++pos) {
        if (!append_decoded_codepoint((unsigned char)input[pos], out, error))
            return false;
    }
    return true;
}

static const uint16_t kWindows1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

static bool decode_windows1252(const std::string &input, ErrorMode mode,
                               std::string &out, std::string &error) {
    for (size_t pos = 0; pos < input.size(); ++pos) {
        const unsigned char byte = (unsigned char)input[pos];
        uint32_t codepoint = byte;
        if (byte >= 0x80 && byte <= 0x9F) {
            codepoint = kWindows1252High[byte - 0x80];
            if (codepoint == 0) {
                if (!handle_decode_error("windows-1252", pos, mode, out,
                                         error))
                    return false;
                continue;
            }
        }
        if (!append_decoded_codepoint(codepoint, out, error))
            return false;
    }
    return true;
}

static bool decode_utf16(const std::string &input, bool big_endian,
                         bool require_bom, ErrorMode mode, std::string &out,
                         std::string &error) {
    size_t pos = 0;
    if (require_bom) {
        if (input.size() < 2) {
            error = "utf-16 requires a BOM";
            return false;
        }
        const unsigned char first = (unsigned char)input[0];
        const unsigned char second = (unsigned char)input[1];
        if (first == 0xFF && second == 0xFE) {
            big_endian = false;
            pos = 2;
        } else if (first == 0xFE && second == 0xFF) {
            big_endian = true;
            pos = 2;
        } else {
            error = "utf-16 requires a BOM";
            return false;
        }
    }

    while (pos + 1 < input.size()) {
        const uint16_t unit = read_u16(input, pos, big_endian);
        uint32_t codepoint = 0;
        size_t consumed = 2;

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (pos + 3 >= input.size()) {
                if (!handle_decode_error("utf-16", pos, mode, out, error))
                    return false;
                pos += 2;
                continue;
            }
            const uint16_t low = read_u16(input, pos + 2, big_endian);
            if (low < 0xDC00 || low > 0xDFFF) {
                if (!handle_decode_error("utf-16", pos, mode, out, error))
                    return false;
                pos += 2;
                continue;
            }
            codepoint = 0x10000u +
                        (((uint32_t)unit - 0xD800u) << 10) +
                        ((uint32_t)low - 0xDC00u);
            consumed = 4;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (!handle_decode_error("utf-16", pos, mode, out, error))
                return false;
            pos += 2;
            continue;
        } else {
            codepoint = unit;
        }

        if (!append_decoded_codepoint(codepoint, out, error))
            return false;
        pos += consumed;
    }

    if (pos < input.size()) {
        if (!handle_decode_error("utf-16", pos, mode, out, error))
            return false;
        pos = input.size();
    }
    return true;
}

static bool decode_gbk(const std::string &input, ErrorMode mode,
                       std::string &out, std::string &error) {
    const auto &table = gb18030_table();
    size_t pos = 0;
    while (pos < input.size()) {
        const unsigned char lead = (unsigned char)input[pos];
        if (lead < 0x80) {
            if (!append_decoded_codepoint(lead, out, error))
                return false;
            ++pos;
            continue;
        }
        if (lead == 0x80) {
            if (!append_decoded_codepoint(0x20AC, out, error))
                return false;
            ++pos;
            continue;
        }
        if (lead >= 0x81 && lead <= 0xFE && pos + 1 < input.size()) {
            const unsigned char trail = (unsigned char)input[pos + 1];
            if (valid_gb_trail(trail)) {
                const size_t index = gb_index(lead, trail);
                if (gbk_valid(index)) {
                    if (!append_decoded_codepoint(table[index], out, error))
                        return false;
                    pos += 2;
                    continue;
                }
            }
        }
        if (!handle_decode_error("gbk", pos, mode, out, error))
            return false;
        ++pos;
    }
    return true;
}

static bool decode_gb18030(const std::string &input, ErrorMode mode,
                           std::string &out, std::string &error) {
    const auto &table = gb18030_table();
    size_t pos = 0;
    while (pos < input.size()) {
        const unsigned char lead = (unsigned char)input[pos];
        if (lead < 0x80) {
            if (!append_decoded_codepoint(lead, out, error))
                return false;
            ++pos;
            continue;
        }
        if (lead == 0x80) {
            if (!handle_decode_error("gb18030", pos, mode, out, error))
                return false;
            ++pos;
            continue;
        }
        if (lead >= 0x81 && lead <= 0xFE) {
            if (pos + 1 < input.size()) {
                const unsigned char second = (unsigned char)input[pos + 1];
                if (second >= 0x30 && second <= 0x39) {
                    if (pos + 3 < input.size()) {
                        const unsigned char third =
                            (unsigned char)input[pos + 2];
                        const unsigned char fourth =
                            (unsigned char)input[pos + 3];
                        if (third >= 0x81 && third <= 0xFE &&
                            fourth >= 0x30 && fourth <= 0x39) {
                            const uint32_t index =
                                (((uint32_t)(lead - 0x81) * 10 +
                                  (uint32_t)(second - 0x30)) *
                                     126 +
                                 (uint32_t)(third - 0x81)) *
                                    10 +
                                (uint32_t)(fourth - 0x30);
                            uint32_t codepoint = 0;
                            if (gb18030_lookup_index(index, codepoint)) {
                                if (!append_decoded_codepoint(
                                        codepoint, out, error))
                                    return false;
                                pos += 4;
                                continue;
                            }
                            if (!handle_decode_error("gb18030", pos, mode,
                                                     out, error))
                                return false;
                            pos += 4;
                            continue;
                        }
                    }
                    if (!handle_decode_error("gb18030", pos, mode, out,
                                             error))
                        return false;
                    ++pos;
                    continue;
                }
                if (valid_gb_trail(second)) {
                    const size_t index = gb_index(lead, second);
                    if (!append_decoded_codepoint(table[index], out, error))
                        return false;
                    pos += 2;
                    continue;
                }
            }
        }
        if (!handle_decode_error("gb18030", pos, mode, out, error))
            return false;
        ++pos;
    }
    return true;
}

template <typename Emit>
static bool walk_utf8(const std::string &input, ErrorMode mode,
                      std::string &output, std::string &error, Emit emit) {
    size_t pos = 0;
    while (pos < input.size()) {
        uint32_t codepoint = 0;
        size_t length = 1;
        const Utf8Status status = utf8_next(input, pos, codepoint, length);
        if (status != Utf8Status::Ok) {
            if (mode == ErrorMode::Strict) {
                error = "invalid utf-8 at byte offset " + std::to_string(pos);
                return false;
            }
            if (mode == ErrorMode::Replace)
                output.push_back('?');
            pos += length;
            continue;
        }
        if (codepoint == 0) {
            error = "string contains NUL";
            return false;
        }
        if (!emit(codepoint))
            return false;
        pos += length;
    }
    return true;
}

static bool encode_utf8(const std::string &input, ErrorMode mode,
                        std::string &output, std::string &error) {
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         append_utf8(codepoint, output);
                         return true;
                     });
}

static bool encode_ascii(const std::string &input, ErrorMode mode,
                         std::string &output, std::string &error) {
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint <= 0x7F) {
                             output.push_back((char)codepoint);
                             return true;
                         }
                         return handle_unrepresentable(codepoint, "ascii",
                                                       mode, output, error);
                     });
}

static bool encode_latin1(const std::string &input, ErrorMode mode,
                          std::string &output, std::string &error) {
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint <= 0xFF) {
                             output.push_back((char)codepoint);
                             return true;
                         }
                         return handle_unrepresentable(codepoint, "latin-1",
                                                       mode, output, error);
                     });
}

static bool encode_windows1252(const std::string &input, ErrorMode mode,
                               std::string &output, std::string &error) {
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint <= 0x7F ||
                             (codepoint >= 0xA0 && codepoint <= 0xFF)) {
                             output.push_back((char)codepoint);
                             return true;
                         }
                         for (size_t i = 0;
                              i < sizeof(kWindows1252High) /
                                      sizeof(kWindows1252High[0]);
                              ++i) {
                             if (kWindows1252High[i] == codepoint) {
                                 output.push_back((char)(0x80 + i));
                                 return true;
                             }
                         }
                         return handle_unrepresentable(
                             codepoint, "windows-1252", mode, output, error);
                     });
}

static bool encode_utf16(const std::string &input, bool big_endian,
                         bool with_bom, ErrorMode mode, std::string &output,
                         std::string &error) {
    if (with_bom) {
        output.push_back((char)0xFF);
        output.push_back((char)0xFE);
    }
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint <= 0xFFFF) {
                             if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
                                 return handle_unrepresentable(
                                     codepoint, "utf-16", mode, output, error);
                             append_u16((uint16_t)codepoint, big_endian, output);
                             return true;
                         }
                         if (codepoint > 0x10FFFF)
                             return handle_unrepresentable(
                                 codepoint, "utf-16", mode, output, error);
                         const uint32_t value = codepoint - 0x10000;
                         append_u16((uint16_t)(0xD800 + (value >> 10)),
                                    big_endian, output);
                         append_u16((uint16_t)(0xDC00 + (value & 0x3FF)),
                                    big_endian, output);
                         return true;
                     });
}

static bool encode_gbk(const std::string &input, ErrorMode mode,
                       std::string &output, std::string &error) {
    const auto &reverse = gb18030_reverse();
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint == 0x20AC) {
                             output.push_back((char)0x80);
                             return true;
                         }
                         if (codepoint < reverse.size()) {
                             const uint32_t index = reverse[codepoint];
                             if (index != kInvalidIndex && gbk_valid(index)) {
                                 const unsigned char lead =
                                     (unsigned char)(0x81 +
                                                     index / 190);
                                 const uint32_t trail_index = index % 190;
                                 const unsigned char trail =
                                     (unsigned char)(trail_index < 0x3F
                                                         ? 0x40 + trail_index
                                                         : 0x41 + trail_index);
                                 output.push_back((char)lead);
                                 output.push_back((char)trail);
                                 return true;
                             }
                         }
                         return handle_unrepresentable(codepoint, "gbk", mode,
                                                       output, error);
                     });
}

static bool encode_gb18030(const std::string &input, ErrorMode mode,
                           std::string &output, std::string &error) {
    const auto &reverse = gb18030_reverse();
    return walk_utf8(input, mode, output, error,
                     [&](uint32_t codepoint) {
                         if (codepoint < 0x80) {
                             output.push_back((char)codepoint);
                             return true;
                         }
                         if (codepoint < reverse.size()) {
                             const uint32_t index = reverse[codepoint];
                             if (index != kInvalidIndex) {
                                 const unsigned char lead =
                                     (unsigned char)(0x81 +
                                                     index / 190);
                                 const uint32_t trail_index = index % 190;
                                 const unsigned char trail =
                                     (unsigned char)(trail_index < 0x3F
                                                         ? 0x40 + trail_index
                                                         : 0x41 + trail_index);
                                 output.push_back((char)lead);
                                 output.push_back((char)trail);
                                 return true;
                             }
                         }
                         uint32_t index = 0;
                         if (gb18030_lookup_codepoint(codepoint, index)) {
                             append_gb18030_four_byte(index, output);
                             return true;
                         }
                         return handle_unrepresentable(
                             codepoint, "gb18030", mode, output, error);
                     });
}

static bool decode_with(const std::string &input, Encoding encoding,
                        ErrorMode mode, std::string &output,
                        std::string &error) {
    switch (encoding) {
        case Encoding::Utf8:
            return decode_utf8(input, mode, output, error);
        case Encoding::Ascii:
            return decode_ascii(input, mode, output, error);
        case Encoding::Latin1:
            return decode_latin1(input, mode, output, error);
        case Encoding::Windows1252:
            return decode_windows1252(input, mode, output, error);
        case Encoding::Utf16Le:
            return decode_utf16(input, false, false, mode, output, error);
        case Encoding::Utf16Be:
            return decode_utf16(input, true, false, mode, output, error);
        case Encoding::Utf16:
            return decode_utf16(input, false, true, mode, output, error);
        case Encoding::Gbk:
            return decode_gbk(input, mode, output, error);
        case Encoding::Gb18030:
            return decode_gb18030(input, mode, output, error);
    }
    error = "unknown encoding";
    return false;
}

static bool encode_with(const std::string &input, Encoding encoding,
                        ErrorMode mode, std::string &output,
                        std::string &error) {
    switch (encoding) {
        case Encoding::Utf8:
            return encode_utf8(input, mode, output, error);
        case Encoding::Ascii:
            return encode_ascii(input, mode, output, error);
        case Encoding::Latin1:
            return encode_latin1(input, mode, output, error);
        case Encoding::Windows1252:
            return encode_windows1252(input, mode, output, error);
        case Encoding::Utf16Le:
            return encode_utf16(input, false, false, mode, output, error);
        case Encoding::Utf16Be:
            return encode_utf16(input, true, false, mode, output, error);
        case Encoding::Utf16:
            return encode_utf16(input, false, true, mode, output, error);
        case Encoding::Gbk:
            return encode_gbk(input, mode, output, error);
        case Encoding::Gb18030:
            return encode_gb18030(input, mode, output, error);
    }
    error = "unknown encoding";
    return false;
}

static bool read_encoding_argument(const std::vector<struct value_t *> &args,
                                   const char *name, Encoding &encoding) {
    if (args[1]->type != T_string) {
        yyerror("%s expects an encoding string, got %s", name,
                typename_of(args[1]));
        return false;
    }
    const std::string encoding_name =
        ((struct string_value_t *)args[1])->value;
    if (!lookup_encoding(encoding_name, encoding)) {
        yyerror("%s: unknown encoding \"%s\"", name,
                encoding_name.c_str());
        return false;
    }
    return true;
}

static bool read_error_mode(const std::vector<struct value_t *> &args,
                            const char *name, ErrorMode &mode) {
    if (args.size() < 3)
        return true;
    if (args[2]->type != T_string) {
        yyerror("%s expects an errors string, got %s", name,
                typename_of(args[2]));
        return false;
    }
    const std::string mode_name = ((struct string_value_t *)args[2])->value;
    if (!parse_error_mode(mode_name, mode)) {
        yyerror("%s: unknown errors mode \"%s\"", name, mode_name.c_str());
        return false;
    }
    return true;
}

}  // namespace

struct value_t *call_encoding_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype != B_encoding_decode && functype != B_encoding_encode &&
        functype != B_encoding_supports) {
        yyerror("unknown encoding function");
        return NULL;
    }
    if (functype == B_encoding_supports) {
        if (args.size() != 1) {
            yyerror("std.encoding.supports expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        if (args[0]->type != T_string) {
            yyerror("std.encoding.supports expects a string, got %s",
                    typename_of(args[0]));
            return NULL;
        }
        Encoding encoding;
        return newboolval(lookup_encoding(
            ((struct string_value_t *)args[0])->value, encoding));
    }

    const bool is_decode = functype == B_encoding_decode;
    const char *name = is_decode ? "std.encoding.decode"
                                 : "std.encoding.encode";
    if (args.size() < 2 || args.size() > 3) {
        yyerror("%s expects 2 or 3 arguments, got %d", name,
                (int)args.size());
        return NULL;
    }

    if (is_decode) {
        if (args[0]->type != T_bytes) {
            yyerror("%s expects bytes, got %s", name, typename_of(args[0]));
            return NULL;
        }
    } else if (args[0]->type != T_string) {
        yyerror("%s expects a string, got %s", name, typename_of(args[0]));
        return NULL;
    }

    Encoding encoding;
    if (!read_encoding_argument(args, name, encoding))
        return NULL;

    ErrorMode mode = ErrorMode::Strict;
    if (!read_error_mode(args, name, mode))
        return NULL;

    std::string output;
    std::string error;
    if (is_decode) {
        const std::string &input = ((struct bytes_value_t *)args[0])->value;
        if (!decode_with(input, encoding, mode, output, error)) {
            yyerror("%s: %s", name, error.c_str());
            return NULL;
        }
        return newstrval(output.c_str());
    }

    const std::string &input = ((struct string_value_t *)args[0])->value;
    if (!encode_with(input, encoding, mode, output, error)) {
        yyerror("%s: %s", name, error.c_str());
        return NULL;
    }
    return newbytesval(std::move(output));
}
