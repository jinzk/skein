#include "builtins_internal.h"
#include "runtime_state.h"

static bool is_valid_utf8(const std::string &data) {
    size_t pos = 0;
    while (pos < data.size()) {
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
        if (pos + n > data.size())
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

static std::string bytes_to_hex_string(const std::string &data) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char byte = (unsigned char)data[i];
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_byteorder(const char *text, bool &little) {
    if (std::strcmp(text, "little") == 0 || std::strcmp(text, "le") == 0 ||
        std::strcmp(text, "little-endian") == 0) {
        little = true;
        return true;
    }
    if (std::strcmp(text, "big") == 0 || std::strcmp(text, "be") == 0 ||
        std::strcmp(text, "big-endian") == 0) {
        little = false;
        return true;
    }
    return false;
}

struct value_t *call_bytes_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_bytes_from_string: {
            if (arg->type != T_string) {
                yyerror("bytes_from_string expects a string, got %s",
                        typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            ret = newbytesval(std::string(text, std::strlen(text)));
            break;
        }
        case B_bytes_to_string: {
            if (arg->type != T_bytes) {
                yyerror("bytes_to_string expects bytes, got %s",
                        typename_of(arg));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            if (data.find('\0') != std::string::npos) {
                yyerror("bytes_to_string: bytes contain NUL byte");
                break;
            }
            if (!is_valid_utf8(data)) {
                yyerror("bytes_to_string: bytes are not valid UTF-8");
                break;
            }
            ret = newstrval(data.c_str());
            break;
        }
        case B_bytes_from_char: {
            if (arg->type != T_char) {
                yyerror("bytes_from_char expects char, got %s",
                        typename_of(arg));
                break;
            }
            std::string data;
            if (!utf8_encode(((struct char_value_t *)arg)->value, data)) {
                yyerror("bytes_from_char: invalid char value");
                break;
            }
            ret = newbytesval(data);
            break;
        }
        case B_char_from_bytes: {
            if (arg->type != T_bytes) {
                yyerror("char_from_bytes expects bytes, got %s",
                        typename_of(arg));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            size_t consumed = 0;
            unsigned int codepoint = 0;
            if (!utf8_decode_one(data.data(), data.size(), consumed,
                                 codepoint) ||
                consumed != data.size()) {
                yyerror("char_from_bytes expects exactly one UTF-8 character");
                break;
            }
            ret = newcharval(codepoint);
            break;
        }
        case B_bytes_len: {
            if (arg->type != T_bytes) {
                yyerror("bytes_len expects bytes, got %s", typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct bytes_value_t *)arg)->value.size());
            break;
        }
        case B_bytes_get: {
            if (args[0]->type != T_bytes) {
                yyerror("bytes_get expects bytes, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("bytes_get index must be an integer, got %s",
                        typename_of(args[1]));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)args[0])->value;
            long long i = ((struct int_value_t *)args[1])->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes_get index %lld out of range", i);
                break;
            }
            ret = newintval((unsigned char)data[(size_t)i]);
            break;
        }
        case B_bytes_slice: {
            if (arg->type != T_bytes) {
                yyerror("bytes_slice expects bytes, got %s", typename_of(arg));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("bytes_slice start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("bytes_slice length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            if (start > data.size())
                start = data.size();
            if (count > data.size() - start)
                count = data.size() - start;
            ret = newbytesval(data.substr(start, count));
            break;
        }
        case B_bytes_concat: {
            if (args[0]->type != T_bytes || args[1]->type != T_bytes) {
                yyerror("bytes_concat expects two bytes values, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string data = ((struct bytes_value_t *)args[0])->value;
            data += ((struct bytes_value_t *)args[1])->value;
            ret = newbytesval(std::move(data));
            break;
        }
        case B_bytes_hex: {
            if (arg->type != T_bytes) {
                yyerror("bytes_hex expects bytes, got %s", typename_of(arg));
                break;
            }
            ret = newstrval(bytes_to_hex_string(
                ((struct bytes_value_t *)arg)->value).c_str());
            break;
        }
        case B_bytes_from_hex: {
            if (arg->type != T_string) {
                yyerror("bytes_from_hex expects a string, got %s",
                        typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            size_t len = std::strlen(text);
            if (len % 2 != 0) {
                yyerror("bytes_from_hex expects an even number of hex digits");
                break;
            }
            std::string data;
            data.reserve(len / 2);
            bool ok = true;
            for (size_t i = 0; i < len; i += 2) {
                int hi = hex_digit_value(text[i]);
                int lo = hex_digit_value(text[i + 1]);
                if (hi < 0 || lo < 0) {
                    yyerror("bytes_from_hex: invalid hex digit at position %zu",
                            i);
                    ok = false;
                    break;
                }
                data.push_back((char)(unsigned char)((hi << 4) | lo));
            }
            if (ok)
                ret = newbytesval(std::move(data));
            break;
        }
        case B_bytes_to_int: {
            if (args[0]->type != T_bytes) {
                yyerror("bytes_to_int expects bytes, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("bytes_to_int size must be an integer");
                break;
            }
            long long size = ((struct int_value_t *)args[1])->value;
            if (size < 1 || size > 8) {
                yyerror("bytes_to_int size must be between 1 and 8");
                break;
            }
            if (args[2]->type != T_string) {
                yyerror("bytes_to_int byteorder must be a string");
                break;
            }
            bool little = false;
            if (!parse_byteorder(((struct string_value_t *)args[2])->value,
                                 little)) {
                yyerror("bytes_to_int byteorder must be \"little\" or \"big\"");
                break;
            }
            long long offset = 0;
            if (args.size() >= 4) {
                if (args[3]->type != T_int ||
                    ((struct int_value_t *)args[3])->value < 0) {
                    yyerror("bytes_to_int offset must be a non-negative integer");
                    break;
                }
                offset = ((struct int_value_t *)args[3])->value;
            }
            bool is_signed = args.size() == 5 && truthy(args[4]);
            const std::string &data = ((struct bytes_value_t *)args[0])->value;
            size_t start = (size_t)offset;
            size_t count = (size_t)size;
            if (start > data.size() || count > data.size() - start) {
                yyerror("bytes_to_int: need %lld bytes at offset %lld, got %zu",
                        size, offset, data.size());
                break;
            }
            unsigned long long value = 0;
            for (size_t i = 0; i < count; ++i) {
                size_t pos = little ? start + count - 1 - i : start + i;
                value = (value << 8) | (unsigned char)data[pos];
            }
            if (is_signed) {
                if (count < 8) {
                    unsigned long long sign_bit = 1ULL << (count * 8 - 1);
                    if (value & sign_bit)
                        value -= (1ULL << (count * 8));
                }
                ret = newintval((long long)value);
            } else if (value > 0x7FFFFFFFFFFFFFFFULL) {
                yyerror("bytes_to_int: unsigned 64-bit value is too large");
            } else {
                ret = newintval((long long)value);
            }
            break;
        }
        case B_int_to_bytes: {
            if (args[0]->type != T_int) {
                yyerror("int_to_bytes expects an integer value, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("int_to_bytes size must be an integer");
                break;
            }
            long long size = ((struct int_value_t *)args[1])->value;
            if (size < 1 || size > 8) {
                yyerror("int_to_bytes size must be between 1 and 8");
                break;
            }
            if (args[2]->type != T_string) {
                yyerror("int_to_bytes byteorder must be a string");
                break;
            }
            bool little = false;
            if (!parse_byteorder(((struct string_value_t *)args[2])->value,
                                 little)) {
                yyerror("int_to_bytes byteorder must be \"little\" or \"big\"");
                break;
            }
            long long value = ((struct int_value_t *)args[0])->value;
            if (size < 8) {
                unsigned bits = (unsigned)(size * 8);
                long long minimum = -(1LL << (bits - 1));
                long long maximum = (1LL << bits) - 1;
                if (value < minimum || value > maximum) {
                    yyerror("int_to_bytes value %lld does not fit in %lld bytes",
                            value, size);
                    break;
                }
            }
            unsigned long long bits = (unsigned long long)value;
            std::string data((size_t)size, '\0');
            for (long long i = 0; i < size; ++i) {
                unsigned char byte = (unsigned char)(bits & 0xFF);
                size_t pos = little ? (size_t)i : (size_t)(size - 1 - i);
                data[pos] = (char)byte;
                bits >>= 8;
            }
            ret = newbytesval(std::move(data));
            break;
        }
        case B_read_bytes: {
            if (arg->type != T_string) {
                yyerror("read_bytes expects a string path, got %s",
                        typename_of(arg));
                break;
            }
            std::string data;
            std::string error;
            if (!platform_read_file(
                    thread_resolve_path(
                        ((struct string_value_t *)arg)->value),
                    data, error))
                yyerror("read_bytes: %s", error.c_str());
            else
                ret = newbytesval(std::move(data));
            break;
        }
        case B_write_bytes:
        case B_append_bytes: {
            const char *name =
                functype == B_write_bytes ? "write_bytes" : "append_bytes";
            if (args[0]->type != T_string || args[1]->type != T_bytes) {
                yyerror("%s expects a string path and bytes, got %s and %s",
                        name, typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_write_file(
                    thread_resolve_path(
                        ((struct string_value_t *)args[0])->value),
                    ((struct bytes_value_t *)args[1])->value,
                    functype == B_append_bytes, error))
                yyerror("%s: %s", name, error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_read_bytes: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_read_bytes"))
                break;
            size_t limit = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((struct int_value_t *)args[1])->value < 0) {
                    yyerror("file_read_bytes size must be a non-negative integer");
                    break;
                }
                limit = (size_t)((struct int_value_t *)args[1])->value;
            }
            std::string data;
            std::string error;
            if (!platform_file_read(file->file, limit, data, error))
                yyerror("file_read_bytes: %s", error.c_str());
            else
                ret = newbytesval(std::move(data));
            break;
        }
        case B_file_write_bytes: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_write_bytes"))
                break;
            if (args[1]->type != T_bytes) {
                yyerror("file_write_bytes expects bytes data, got %s",
                        typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_file_write(
                    file->file, ((struct bytes_value_t *)args[1])->value,
                    error))
                yyerror("file_write_bytes: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
