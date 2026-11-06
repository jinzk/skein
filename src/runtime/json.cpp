#include "json.h"

#include "symbol.h"
#include "builtin_ids.h"
#include "dict.h"
#include "structtype.h"
#include "unicode.h"
#include "valuetype.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int MAX_JSON_DEPTH = 512;

bool is_valid_utf8_bytes(const char *data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        unsigned char c = (unsigned char)data[pos];
        if (c < 0x80) {
            ++pos;
            continue;
        }
        size_t n;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            n = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3;
            if (c == 0xE0)
                low = 0xA0;
            else if (c == 0xED)
                high = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4;
            if (c == 0xF0)
                low = 0x90;
            else if (c == 0xF4)
                high = 0x8F;
        } else {
            return false;
        }
        if (pos + n > size)
            return false;
        unsigned char second = (unsigned char)data[pos + 1];
        if (second < low || second > high)
            return false;
        for (size_t i = 2; i < n; ++i) {
            if (((unsigned char)data[pos + i] & 0xC0) != 0x80)
                return false;
        }
        pos += n;
    }
    return true;
}

std::string format_json_double(double value) {
    char buf[64];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, value);
        if (std::strtod(buf, NULL) == value)
            break;
    }

    std::string text(buf);
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    return text;
}

void append_utf8(unsigned int codepoint, std::string &out) {
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

int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : input(text), pos(0) {}

    struct value_t *parse_document() {
        if (!is_valid_utf8_bytes(input.data(), input.size())) {
            pos = 0;
            fail("input is not valid UTF-8");
            return NULL;
        }

        skip_whitespace();
        struct value_t *result = parse_value(0);
        if (!result)
            return NULL;

        skip_whitespace();
        if (pos != input.size()) {
            fail("unexpected trailing content");
            freeval(result);
            return NULL;
        }
        return result;
    }

    const std::string &error_message() const { return error; }

private:
    const std::string &input;
    size_t pos;
    std::string error;

    std::string location(size_t at) const {
        size_t line = 1;
        size_t column = 1;
        size_t limit = at < input.size() ? at : input.size();
        for (size_t i = 0; i < limit; ++i) {
            if (input[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return "line " + std::to_string(line) + ", column " +
               std::to_string(column);
    }

    bool fail(const std::string &message) {
        if (error.empty())
            error = "JSON parse error at " + location(pos) + ": " + message;
        return false;
    }

    struct value_t *fail_value(const std::string &message) {
        fail(message);
        return NULL;
    }

    bool consume(char expected) {
        if (pos < input.size() && input[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool consume_literal(const char *literal) {
        size_t length = std::strlen(literal);
        if (input.compare(pos, length, literal) == 0) {
            pos += length;
            return true;
        }
        return false;
    }

    void skip_whitespace() {
        while (pos < input.size()) {
            char c = input[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos;
            else
                break;
        }
    }

    struct value_t *parse_value(int depth) {
        if (depth > MAX_JSON_DEPTH)
            return fail_value("maximum nesting depth exceeded");

        skip_whitespace();
        if (pos >= input.size())
            return fail_value("unexpected end of input");

        char c = input[pos];
        switch (c) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                std::string text;
                if (!parse_string(text))
                    return NULL;
                return newstrval(text.c_str());
            }
            case 't':
                if (consume_literal("true"))
                    return newboolval(true);
                return fail_value("invalid literal");
            case 'f':
                if (consume_literal("false"))
                    return newboolval(false);
                return fail_value("invalid literal");
            case 'n':
                if (consume_literal("null"))
                    return newnull();
                return fail_value("invalid literal");
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parse_number();
                return fail_value("unexpected character");
        }
    }

    struct value_t *parse_object(int depth) {
        ++pos;  // '{'
        struct dict_value_t *dict = newdictval();
        std::unordered_map<std::string, size_t> index;

        skip_whitespace();
        if (consume('}'))
            return (struct value_t *)dict;

        while (true) {
            skip_whitespace();
            if (pos >= input.size()) {
                fail("unterminated object");
                freeval((struct value_t *)dict);
                return NULL;
            }
            if (input[pos] != '"') {
                fail("expected string key");
                freeval((struct value_t *)dict);
                return NULL;
            }

            std::string key;
            if (!parse_string(key)) {
                freeval((struct value_t *)dict);
                return NULL;
            }

            skip_whitespace();
            if (!consume(':')) {
                fail("expected ':' after key");
                freeval((struct value_t *)dict);
                return NULL;
            }

            struct value_t *value = parse_value(depth + 1);
            if (!value) {
                freeval((struct value_t *)dict);
                return NULL;
            }

            auto existing = index.find(key);
            if (existing != index.end()) {
                freeval(dict->payload->items[existing->second].second);
                dict->payload->items[existing->second].second = value;
            } else {
                size_t item = dict->payload->items.size();
                dict_append_item(dict, newstrval(key.c_str()), value);
                index.emplace(std::move(key), item);
            }

            skip_whitespace();
            if (consume('}'))
                break;
            if (!consume(',')) {
                fail("expected ',' or '}'");
                freeval((struct value_t *)dict);
                return NULL;
            }
        }

        return (struct value_t *)dict;
    }

    struct value_t *parse_array(int depth) {
        ++pos;  // '['
        struct list_value_t *list = newlistval();

        skip_whitespace();
        if (consume(']'))
            return (struct value_t *)list;

        while (true) {
            struct value_t *value = parse_value(depth + 1);
            if (!value) {
                freeval((struct value_t *)list);
                return NULL;
            }
            list->payload->items.push_back(value);

            skip_whitespace();
            if (consume(']'))
                break;
            if (!consume(',')) {
                fail("expected ',' or ']'");
                freeval((struct value_t *)list);
                return NULL;
            }
        }

        return (struct value_t *)list;
    }

    bool parse_hex4(unsigned int &value) {
        if (pos + 4 > input.size())
            return fail("incomplete unicode escape");

        unsigned int result = 0;
        for (int i = 0; i < 4; ++i) {
            int digit = hex_digit_value(input[pos++]);
            if (digit < 0)
                return fail("invalid unicode escape");
            result = (result << 4) | (unsigned int)digit;
        }
        value = result;
        return true;
    }

    bool parse_string(std::string &out) {
        if (!consume('"'))
            return fail("expected string");

        out.clear();
        while (pos < input.size()) {
            unsigned char c = (unsigned char)input[pos++];
            if (c == '"')
                return true;

            if (c == '\\') {
                if (pos >= input.size())
                    return fail("unterminated escape sequence");
                char escape = input[pos++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int codepoint = 0;
                        if (!parse_hex4(codepoint))
                            return false;

                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            if (pos + 1 >= input.size() ||
                                input[pos] != '\\' || input[pos + 1] != 'u') {
                                return fail("unpaired high surrogate");
                            }
                            pos += 2;
                            unsigned int low = 0;
                            if (!parse_hex4(low))
                                return false;
                            if (low < 0xDC00 || low > 0xDFFF)
                                return fail("invalid low surrogate");
                            codepoint = 0x10000 +
                                        ((codepoint - 0xD800) << 10) +
                                        (low - 0xDC00);
                        } else if (codepoint >= 0xDC00 &&
                                   codepoint <= 0xDFFF) {
                            return fail("unpaired low surrogate");
                        }

                        if (codepoint == 0)
                            return fail("NUL characters are not supported");
                        append_utf8(codepoint, out);
                        break;
                    }
                    default:
                        return fail("invalid escape sequence");
                }
            } else if (c < 0x20) {
                return fail("unescaped control character in string");
            } else {
                out.push_back((char)c);
            }
        }

        return fail("unterminated string");
    }

    struct value_t *parse_number() {
        size_t start = pos;

        consume('-');
        if (pos >= input.size())
            return fail_value("invalid number");

        if (input[pos] == '0') {
            ++pos;
            if (pos < input.size() &&
                std::isdigit((unsigned char)input[pos]))
                return fail_value("leading zero in number");
        } else if (input[pos] >= '1' && input[pos] <= '9') {
            while (pos < input.size() &&
                   std::isdigit((unsigned char)input[pos]))
                ++pos;
        } else {
            return fail_value("invalid number");
        }

        bool is_float = false;
        if (pos < input.size() && input[pos] == '.') {
            is_float = true;
            ++pos;
            size_t digits = pos;
            while (pos < input.size() &&
                   std::isdigit((unsigned char)input[pos]))
                ++pos;
            if (pos == digits)
                return fail_value("expected digit after decimal point");
        }

        if (pos < input.size() &&
            (input[pos] == 'e' || input[pos] == 'E')) {
            is_float = true;
            ++pos;
            if (pos < input.size() &&
                (input[pos] == '+' || input[pos] == '-'))
                ++pos;
            size_t digits = pos;
            while (pos < input.size() &&
                   std::isdigit((unsigned char)input[pos]))
                ++pos;
            if (pos == digits)
                return fail_value("expected digit in exponent");
        }

        std::string text = input.substr(start, pos - start);
        if (!is_float) {
            errno = 0;
            char *end = NULL;
            long long integer = std::strtoll(text.c_str(), &end, 10);
            if (end == text.c_str() + text.size() && errno != ERANGE)
                return newintval(integer);
        }

        errno = 0;
        char *end = NULL;
        double number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(number))
            return fail_value("number is out of range");
        return newfloatval(number);
    }
};

class JsonEncoder {
public:
    std::string encode(const struct value_t *value) {
        out.clear();
        error.clear();
        if (!encode_value(value, 0))
            return std::string();
        return out;
    }

    const std::string &error_message() const { return error; }

private:
    std::string out;
    std::string error;

    bool fail(const std::string &message) {
        if (error.empty())
            error = message;
        return false;
    }

    bool encode_string(const char *text) {
        if (!text)
            text = "";
        size_t length = std::strlen(text);
        if (!is_valid_utf8_bytes(text, length))
            return fail("JSON strings must be valid UTF-8");

        static const char hex[] = "0123456789abcdef";
        out.push_back('"');
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = (unsigned char)text[i];
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out.push_back(hex[c >> 4]);
                        out.push_back(hex[c & 0x0F]);
                    } else {
                        out.push_back((char)c);
                    }
                    break;
            }
        }
        out.push_back('"');
        return true;
    }

    bool encode_value(const struct value_t *value, int depth) {
        if (depth > MAX_JSON_DEPTH)
            return fail("maximum JSON nesting depth exceeded");

        if (!value) {
            out += "null";
            return true;
        }

        switch (value->type) {
            case T_null:
                out += "null";
                return true;
            case T_bool:
                out += ((const struct bool_value_t *)value)->value
                           ? "true"
                           : "false";
                return true;
            case T_int:
                out += std::to_string(
                    ((const struct int_value_t *)value)->value);
                return true;
            case T_float: {
                double number =
                    ((const struct float_value_t *)value)->value;
                if (!std::isfinite(number))
                    return fail("JSON cannot encode NaN or infinity");
                out += format_json_double(number);
                return true;
            }
            case T_string:
                return encode_string(
                    ((const struct string_value_t *)value)->value);
            case T_char: {
                std::string text;
                if (!utf8_encode(
                        ((const struct char_value_t *)value)->value, text))
                    return fail("invalid char value");
                return encode_string(text.c_str());
            }
            case T_byte:
                out += std::to_string(
                    ((const struct byte_value_t *)value)->value);
                return true;
            case T_list: {
                const struct list_value_t *list =
                    (const struct list_value_t *)value;
                out.push_back('[');
                for (size_t i = 0; i < list->payload->items.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_value(list->payload->items[i], depth + 1))
                        return false;
                }
                out.push_back(']');
                return true;
            }
            case T_deque: {
                const struct deque_value_t *deque =
                    (const struct deque_value_t *)value;
                std::vector<value_t *> items;
                deque_items_in_order(deque->payload.get(), items);
                out.push_back('[');
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_value(items[i], depth + 1))
                        return false;
                }
                out.push_back(']');
                return true;
            }
            case T_heap: {
                const struct heap_value_t *heap =
                    (const struct heap_value_t *)value;
                std::vector<value_t *> items;
                heap_items_level_order(heap->payload.get(), items);
                out.push_back('[');
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_value(items[i], depth + 1))
                        return false;
                }
                out.push_back(']');
                return true;
            }
            case T_set: {
                const struct set_value_t *set =
                    (const struct set_value_t *)value;
                std::vector<value_t *> items;
                set_values_in_order(set->payload.get(), items);
                out.push_back('[');
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_value(items[i], depth + 1))
                        return false;
                }
                out.push_back(']');
                return true;
            }
            case T_dict: {
                const struct dict_value_t *dict =
                    (const struct dict_value_t *)value;
                out.push_back('{');
                for (size_t i = 0; i < dict->payload->items.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    const struct value_t *key =
                        dict->payload->items[i].first;
                    if (!key || key->type != T_string) {
                        return fail(
                            "JSON object keys must be strings, got " +
                            std::string(typename_of(key)));
                    }
                    if (!encode_string(
                            ((const struct string_value_t *)key)->value))
                        return false;
                    out.push_back(':');
                    if (!encode_value(dict->payload->items[i].second,
                                      depth + 1))
                        return false;
                }
                out.push_back('}');
                return true;
            }
            case T_struct: {
                const struct struct_value_t *sv =
                    (const struct struct_value_t *)value;
                if (!sv->def || !sv->payload)
                    return fail("invalid struct value");
                out.push_back('{');
                for (size_t i = 0; i < sv->def->fields.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_string(
                            sv->def->fields[i].name.c_str()))
                        return false;
                    out.push_back(':');
                    if (!encode_value(sv->payload->fields[i], depth + 1))
                        return false;
                }
                out.push_back('}');
                return true;
            }
            case T_enum: {
                const struct enum_value_t *ev =
                    (const struct enum_value_t *)value;
                if (!ev->def || ev->variant < 0 ||
                    (size_t)ev->variant >= ev->def->variants.size())
                    return fail("invalid enum value");
                const struct variant_def &variant =
                    ev->def->variants[(size_t)ev->variant];
                out += "{\"$enum\":";
                std::string tag = ev->def->name + "." + variant.name;
                if (!encode_string(tag.c_str()))
                    return false;
                out += ",\"payload\":{";
                for (size_t i = 0; i < variant.fields.size(); ++i) {
                    if (i)
                        out.push_back(',');
                    if (!encode_string(variant.fields[i].name.c_str()))
                        return false;
                    out.push_back(':');
                    if (!encode_value(ev->payload->fields[i], depth + 1))
                        return false;
                }
                out += "}}";
                return true;
            }
            default:
                return fail("cannot encode " +
                            std::string(typename_of(value)) + " as JSON");
        }
    }
};

struct value_t *json_parse(const std::vector<struct value_t *> &args,
                           bool try_mode) {
    const char *name =
        try_mode ? "std.json.try_parse" : "std.json.parse";
    if (args.size() != 1) {
        yyerror("%s expects 1 argument, got %d", name, (int)args.size());
        return NULL;
    }
    if (args[0]->type != T_string && args[0]->type != T_bytes) {
        yyerror("%s expects a string or bytes, got %s", name,
                typename_of(args[0]));
        return NULL;
    }

    std::string text =
        args[0]->type == T_string
            ? ((struct string_value_t *)args[0])->value
            : ((struct bytes_value_t *)args[0])->value;
    JsonParser parser(text);
    struct value_t *value = parser.parse_document();
    if (!value) {
        if (try_mode)
            return newresulterr_message("json", parser.error_message());
        yyerror("%s", parser.error_message().c_str());
        return NULL;
    }

    if (try_mode)
        return newresultok(value);
    return value;
}

struct value_t *json_stringify(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.json.stringify expects 1 argument, got %d",
                (int)args.size());
        return NULL;
    }

    JsonEncoder encoder;
    std::string text = encoder.encode(args[0]);
    if (!encoder.error_message().empty()) {
        yyerror("%s", encoder.error_message().c_str());
        return NULL;
    }
    return newstrval(text.c_str());
}

}  // namespace

struct value_t *json_parse_text(const std::string &text, std::string &error) {
    JsonParser parser(text);
    struct value_t *value = parser.parse_document();
    if (!value) {
        error = parser.error_message();
        return NULL;
    }
    return value;
}

struct value_t *call_json_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_json_parse:
            return json_parse(args, false);
        case B_json_try_parse:
            return json_parse(args, true);
        case B_json_stringify:
            return json_stringify(args);
        default:
            yyerror("unknown json function");
            return NULL;
    }
}
