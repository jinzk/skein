#include "format.h"

#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_FORMAT_WIDTH = 10000;

struct format_spec {
    char fill = ' ';
    char align = 0;  // '<', '>', '^', or 0 for the type's default
    bool has_width = false;
    size_t width = 0;
    bool has_precision = false;
    size_t precision = 0;
    char type = 0;  // 0, 's', 'd', 'f', 'x', 'X'
};

size_t utf8_sequence_length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t utf8_valid_sequence_length(const std::string &s, size_t pos) {
    size_t available = s.size() - pos;
    size_t n = utf8_sequence_length((unsigned char)s[pos]);
    if (n > available) return 1;
    for (size_t i = 1; i < n; ++i) {
        if (((unsigned char)s[pos + i] & 0xC0) != 0x80)
            return 1;
    }
    return n;
}

size_t utf8_length(const std::string &s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); ++count)
        i += utf8_valid_sequence_length(s, i);
    return count;
}

std::string utf8_prefix(const std::string &s, size_t chars) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < s.size() && count < chars) {
        pos += utf8_valid_sequence_length(s, pos);
        ++count;
    }
    return s.substr(0, pos);
}

bool is_align(char c) {
    return c == '<' || c == '>' || c == '^';
}

bool is_type(char c) {
    return c == 's' || c == 'd' || c == 'f' || c == 'x' || c == 'X';
}

bool parse_size(const std::string &s, size_t begin, size_t end, size_t &out) {
    if (begin >= end) return false;
    size_t value = 0;
    for (size_t i = begin; i < end; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!std::isdigit(c)) return false;
        size_t digit = (size_t)(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_spec(const std::string &text, format_spec &spec, std::string &error) {
    size_t i = 0;

    if (text.size() >= 2 && is_align(text[1])) {
        spec.fill = text[0];
        spec.align = text[1];
        i = 2;
    } else if (i < text.size() && is_align(text[i])) {
        spec.align = text[i];
        i = 1;
    } else if (text.size() >= 2 && text[0] == '0' &&
               std::isdigit((unsigned char)text[1])) {
        /* Python-style zero padding: {:08d}. */
        spec.fill = '0';
        spec.align = '>';
    }

    size_t start = i;
    while (i < text.size() && std::isdigit((unsigned char)text[i])) ++i;
    if (i > start) {
        if (!parse_size(text, start, i, spec.width)) {
            error = "invalid width in format spec";
            return false;
        }
        if (spec.width > MAX_FORMAT_WIDTH) {
            error = "format width is too large";
            return false;
        }
        spec.has_width = true;
    }

    if (i < text.size() && text[i] == '.') {
        ++i;
        start = i;
        while (i < text.size() && std::isdigit((unsigned char)text[i])) ++i;
        if (i == start) {
            error = "missing precision in format spec";
            return false;
        }
        if (!parse_size(text, start, i, spec.precision)) {
            error = "invalid precision in format spec";
            return false;
        }
        if (spec.precision > MAX_FORMAT_WIDTH) {
            error = "format precision is too large";
            return false;
        }
        spec.has_precision = true;
    }

    if (i < text.size()) {
        char type = text[i++];
        if (!is_type(type)) {
            error = std::string("unknown format type '") + type + "'";
            return false;
        }
        spec.type = type;
    }

    if (i != text.size()) {
        error = "invalid format spec";
        return false;
    }
    return true;
}

std::string format_hex(long long value, bool upper) {
    bool negative = value < 0;
    unsigned long long magnitude =
        negative ? (unsigned long long)(-(value + 1)) + 1
                 : (unsigned long long)value;
    char buf[32];
    std::snprintf(buf, sizeof buf, upper ? "%llX" : "%llx", magnitude);
    std::string out(buf);
    if (negative) out.insert(out.begin(), '-');
    return out;
}

std::string format_float(double value, size_t precision) {
    /* %f needs the integer part, sign, dot and precision; 400 is ample. */
    std::vector<char> buf(precision + 400);
    int n = std::snprintf(buf.data(), buf.size(), "%.*f",
                          (int)precision, value);
    if (n < 0) return "";
    return std::string(buf.data(), (size_t)n);
}

void pad(std::string &content, const format_spec &spec, bool numeric) {
    if (!spec.has_width)
        return;
    size_t len = utf8_length(content);
    if (len >= spec.width)
        return;

    size_t total = spec.width - len;
    char align = spec.align ? spec.align : (numeric ? '>' : '<');
    if (align == '>') {
        if (spec.fill == '0' && !content.empty() &&
            (content[0] == '-' || content[0] == '+'))
            content.insert(1, total, spec.fill);
        else
            content.insert(0, total, spec.fill);
    } else if (align == '^') {
        size_t left = total / 2;
        content.insert(0, left, spec.fill);
        content.append(total - left, spec.fill);
    } else {
        content.append(total, spec.fill);
    }
}

bool format_one(const struct value_t *value, const format_spec &spec,
                std::string &out, std::string &error) {
    std::string content;
    bool numeric = false;

    switch (spec.type) {
        case 0:
            if (spec.has_precision) {
                error = "precision requires a format type";
                return false;
            }
            content = value_to_string(value);
            numeric = value->type == T_int || value->type == T_float;
            break;
        case 's':
            if (value->type != T_string) {
                error = "'s' format requires a string";
                return false;
            }
            content = ((const struct string_value_t *)value)->value;
            if (spec.has_precision)
                content = utf8_prefix(content, spec.precision);
            break;
        case 'd':
            if (value->type != T_int) {
                error = "'d' format requires an integer";
                return false;
            }
            if (spec.has_precision) {
                error = "precision is not allowed for integer formats";
                return false;
            }
            content = std::to_string(((const struct int_value_t *)value)->value);
            numeric = true;
            break;
        case 'f': {
            if (value->type != T_int && value->type != T_float) {
                error = "'f' format requires a number";
                return false;
            }
            double d = value->type == T_int
                           ? (double)((const struct int_value_t *)value)->value
                           : ((const struct float_value_t *)value)->value;
            size_t precision = spec.has_precision ? spec.precision : 6;
            content = format_float(d, precision);
            numeric = true;
            break;
        }
        case 'x':
        case 'X':
            if (value->type != T_int) {
                error = "'x' format requires an integer";
                return false;
            }
            if (spec.has_precision) {
                error = "precision is not allowed for integer formats";
                return false;
            }
            content = format_hex(((const struct int_value_t *)value)->value,
                                 spec.type == 'X');
            numeric = true;
            break;
        default:
            error = "unknown format type";
            return false;
    }

    pad(content, spec, numeric);
    out = content;
    return true;
}

}  // namespace

bool format_values(const std::string &fmt,
                   const std::vector<struct value_t *> &args,
                   std::string &out, std::string &error) {
    out.clear();
    size_t next_auto = 0;
    bool used_auto = false;
    bool used_manual = false;

    for (size_t i = 0; i < fmt.size();) {
        char c = fmt[i];
        if (c == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                out += '{';
                i += 2;
                continue;
            }

            size_t close = fmt.find('}', i + 1);
            if (close == std::string::npos) {
                error = "unmatched '{' in format string";
                return false;
            }

            std::string field = fmt.substr(i + 1, close - i - 1);
            std::string index_text = field;
            std::string spec_text;
            size_t colon = field.find(':');
            if (colon != std::string::npos) {
                index_text = field.substr(0, colon);
                spec_text = field.substr(colon + 1);
            }

            size_t index = 0;
            if (index_text.empty()) {
                if (used_manual) {
                    error = "cannot mix automatic and explicit field numbering";
                    return false;
                }
                used_auto = true;
                index = next_auto++;
            } else {
                if (used_auto) {
                    error = "cannot mix automatic and explicit field numbering";
                    return false;
                }
                used_manual = true;
                if (!parse_size(index_text, 0, index_text.size(), index)) {
                    error = "invalid field index in format string";
                    return false;
                }
            }

            if (index >= args.size()) {
                error = "format argument index " + std::to_string(index) +
                        " is out of range";
                return false;
            }

            format_spec spec;
            if (!parse_spec(spec_text, spec, error))
                return false;

            std::string piece;
            if (!format_one(args[index], spec, piece, error))
                return false;

            out += piece;
            i = close + 1;
        } else if (c == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                out += '}';
                i += 2;
            } else {
                error = "unmatched '}' in format string";
                return false;
            }
        } else {
            out += c;
            ++i;
        }
    }
    return true;
}
