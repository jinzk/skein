#include "builtins.h"

#include "ast.h"
#include "eval_helpers.h"
#include "net.h"
#include "task.h"
#ifdef CALC_ENABLE_FFI
#include "ffi_module.h"
#endif
#include "filetype.h"
#include "format.h"
#include "platform.h"
#include "repl.h"
#include "structtype.h"
#include "valuetype.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char *skip_space(const char *p) {
    while (*p && std::isspace((unsigned char)*p)) ++p;
    return p;
}

static bool parse_int_string(const char *s, long long &out) {
    if (!s) return false;
    errno = 0;
    char *end = NULL;
    long long v = std::strtoll(s, &end, 10);
    if (end == s || errno == ERANGE) return false;
    if (*skip_space(end) != '\0') return false;
    out = v;
    return true;
}

static bool parse_float_string(const char *s, double &out) {
    if (!s) return false;
    errno = 0;
    char *end = NULL;
    double v = std::strtod(s, &end);
    if (end == s) return false;
    if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) return false;
    if (*skip_space(end) != '\0') return false;
    out = v;
    return true;
}

/*
 * Compare an int64 with a double exactly, returning the sign of (i - d).
 * `unordered` is set for NaN, where all comparisons are false.
 */
static size_t utf8_length_prefix(const char *s, size_t bytes) {
    size_t len = std::strlen(s);
    if (bytes > len)
        bytes = len;
    size_t pos = 0;
    size_t count = 0;
    while (pos < bytes) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++count;
    }
    return count;
}

static std::string utf8_substr(const char *s, size_t start, size_t count) {
    size_t len = std::strlen(s);
    size_t pos = 0;
    size_t current = 0;
    while (pos < len && current < start) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++current;
    }
    size_t begin = pos;
    size_t taken = 0;
    while (pos < len && taken < count) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++taken;
    }
    return std::string(s + begin, pos - begin);
}

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

static const char *kind_of_value(const struct value_t *v) {
    if (!v) return "null";
    switch (v->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_bool: return "bool";
        case T_string: return "string";
        case T_bytes: return "bytes";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_func: return "function";
        case T_builtin: return "builtin";
        case T_userdata: return "userdata";
        case T_struct: return "struct";
        case T_method: return "method";
        case T_null: return "null";
        case T_class: return "class";
        case T_return: return "return";
        case T_break: return "break";
        case T_continue: return "continue";
        default: return "unknown";
    }
}

static bool collect_command_env(
    const std::vector<struct value_t *> &args, size_t index,
    const char *who,
    std::vector<std::pair<std::string, std::string>> &env) {
    if (index >= args.size())
        return true;

    struct value_t *value = args[index];
    if (value->type != T_dict) {
        yyerror("%s env must be a dict, got %s", who, typename_of(value));
        return false;
    }

    struct dict_value_t *dict = (struct dict_value_t *)value;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        struct value_t *key = dict->payload->items[i].first;
        struct value_t *val = dict->payload->items[i].second;
        if (key->type != T_string) {
            yyerror("%s env keys must be strings", who);
            return false;
        }
        if (val->type != T_string) {
            yyerror("%s env values must be strings", who);
            return false;
        }
        const char *name = ((struct string_value_t *)key)->value;
        if (!name[0] || std::strchr(name, '=') != NULL) {
            yyerror("invalid environment variable name");
            return false;
        }
        env.emplace_back(name,
                         ((struct string_value_t *)val)->value);
    }
    return true;
}

static bool get_file_resource(struct value_t *value,
                              std::shared_ptr<file_resource> &file,
                              const char *who) {
    if (!is_file_value(value)) {
        yyerror("%s expects a file, got %s", who, typename_of(value));
        return false;
    }
    file = file_value(value);
    if (!file) {
        yyerror("%s expects a file", who);
        return false;
    }
    return true;
}

static void erase_last_utf8_character(std::string &line) {
    if (line.empty())
        return;
    size_t pos = line.size() - 1;
    while (pos > 0 && (((unsigned char)line[pos]) & 0xC0) == 0x80)
        --pos;
    line.erase(pos);
}

static bool read_console_line(const char *prompt, std::string &line,
                              bool &at_eof) {
    line.clear();
    at_eof = false;

    if (prompt) {
        std::fputs(prompt, stdout);
        std::fflush(stdout);
    }

    for (;;) {
        int c = std::fgetc(stdin);
        if (c == EOF) {
            if (g_interrupted) {
                std::clearerr(stdin);
                return false;
            }
            if (std::ferror(stdin)) {
                std::clearerr(stdin);
                yyerror("input: read error");
                return false;
            }
            at_eof = true;
            return true;
        }
        if (c == '\n')
            return true;
        if (c == '\r')
            continue;
        if (c == '\b' || c == 0x7f) {
            erase_last_utf8_character(line);
            continue;
        }
        line.push_back((char)c);
    }
}

static const char *math_builtin_name(int functype) {
    switch (functype) {
        case B_sin: return "sin";
        case B_cos: return "cos";
        case B_tan: return "tan";
        case B_asin: return "asin";
        case B_acos: return "acos";
        case B_atan: return "atan";
        case B_atan2: return "atan2";
        case B_pow: return "pow";
        case B_floor: return "floor";
        case B_ceil: return "ceil";
        case B_round: return "round";
        case B_trunc: return "trunc";
        case B_log2: return "log2";
        case B_log10: return "log10";
        case B_hypot: return "hypot";
        case B_min: return "min";
        case B_max: return "max";
        default: return "math function";
    }
}

struct value_t *call_builtin(int functype,
                             const std::vector<struct value_t *> &args) {
    if (functype >= B_net_tcp_connect && functype <= B_net_peer_addr)
        return call_net_builtin(functype, args);
#ifdef CALC_ENABLE_FFI
    if (functype >= B_ffi_open && functype <= B_ffi_buffer_free)
        return call_ffi_builtin(functype, args);
#endif
    if (functype >= B_task_sleep && functype <= B_task_done)
        return call_task_builtin(functype, args);

    if (functype == B_list || functype == B_dict) {
        if (!args.empty()) {
            yyerror("built-in function expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        return (functype == B_list)
                   ? (struct value_t *)newlistval()
                   : (struct value_t *)newdictval();
    }

    if (functype == B_format || functype == B_print) {
        struct value_t *ret = NULL;
        if (functype == B_format && args.empty()) {
            yyerror("format expects at least 1 argument");
        } else if (functype == B_print && args.empty()) {
            std::printf("\n");
            ret = newnull();
        } else if (functype == B_print && args.size() == 1) {
            printvalue(args[0]);
            std::printf("\n");
            ret = newnull();
        } else if (args[0]->type != T_string) {
            yyerror("%s first argument must be a string, got %s",
                    functype == B_format ? "format" : "print",
                    typename_of(args[0]));
        } else {
            std::vector<struct value_t *> fmt_args(args.begin() + 1, args.end());
            std::string out;
            std::string error;
            if (!format_values(((struct string_value_t *)args[0])->value,
                               fmt_args, out, error)) {
                yyerror("%s", error.c_str());
            } else if (functype == B_format) {
                ret = newstrval(out.c_str());
            } else {
                std::printf("%s\n", out.c_str());
                ret = newnull();
            }
        }
        return ret;
    }

    if (functype == B_command || functype == B_command_status) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d",
                    functype == B_command ? "command" : "command_status",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_write_file || functype == B_append_file) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_write_file ? "write_file" : "append_file",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_open || functype == B_file_write) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_file_open ? "file_open" : "file_write",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_seek) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("file_seek expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_input) {
        if (args.size() > 1) {
            yyerror("input expects 0 or 1 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_split || functype == B_find) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_split ? "split" : "find",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_substr) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("substr expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_get || functype == B_bytes_concat ||
               functype == B_write_bytes || functype == B_append_bytes ||
               functype == B_file_write_bytes) {
        if (args.size() != 2) {
            const char *name =
                functype == B_bytes_get ? "bytes_get" :
                functype == B_bytes_concat ? "bytes_concat" :
                functype == B_write_bytes ? "write_bytes" :
                functype == B_append_bytes ? "append_bytes" :
                "file_write_bytes";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("bytes_slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_to_int) {
        if (args.size() < 3 || args.size() > 5) {
            yyerror("bytes_to_int expects 3 to 5 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_int_to_bytes) {
        if (args.size() != 3) {
            yyerror("int_to_bytes expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read_bytes) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read_bytes expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_range) {
        if (args.empty() || args.size() > 3) {
            yyerror("range expects 1 to 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_contains || functype == B_list_index ||
               functype == B_dict_has || functype == B_string_join ||
               functype == B_string_starts_with ||
               functype == B_string_ends_with) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_contains ? "std.list.contains" :
                functype == B_list_index ? "std.list.index" :
                functype == B_dict_has ? "std.dict.has" :
                functype == B_string_join ? "std.string.join" :
                functype == B_string_starts_with ? "std.string.starts_with" :
                "std.string.ends_with";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_dict_get) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.dict.get expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_string_replace) {
        if (args.size() != 3) {
            yyerror("std.string.replace expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_append || functype == B_dict_delete ||
               functype == B_dict_merge || functype == B_string_contains ||
               functype == B_string_repeat || functype == B_list_map ||
               functype == B_list_filter) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_append ? "std.list.append" :
                functype == B_dict_delete ? "std.dict.delete" :
                functype == B_dict_merge ? "std.dict.merge" :
                functype == B_string_contains ? "std.string.contains" :
                functype == B_string_repeat ? "std.string.repeat" :
                functype == B_list_map ? "std.list.map" : "std.list.filter";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_reduce) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.reduce expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_sin || functype == B_cos ||
               functype == B_tan || functype == B_asin ||
               functype == B_acos || functype == B_atan ||
               functype == B_floor || functype == B_ceil ||
               functype == B_round || functype == B_trunc ||
               functype == B_log2 || functype == B_log10) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_atan2 || functype == B_pow ||
               functype == B_hypot) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_min || functype == B_max) {
        if (args.empty()) {
            yyerror("%s expects at least 1 argument", math_builtin_name(functype));
            return NULL;
        }
    } else if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        return NULL;
    }

    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_sin:
        case B_cos:
        case B_tan:
        case B_asin:
        case B_acos:
        case B_atan: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            double result = 0.0;
            if (functype == B_sin) result = std::sin(value);
            else if (functype == B_cos) result = std::cos(value);
            else if (functype == B_tan) result = std::tan(value);
            else if (functype == B_asin) result = std::asin(value);
            else if (functype == B_acos) result = std::acos(value);
            else result = std::atan(value);
            ret = newfloatval(result);
            break;
        }
        case B_atan2:
        case B_pow:
        case B_hypot: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("%s expects two numbers, got %s and %s",
                        math_builtin_name(functype), typename_of(args[0]),
                        typename_of(args[1]));
                break;
            }
            double a = as_double(args[0]);
            double b = as_double(args[1]);
            if (functype == B_atan2) ret = newfloatval(std::atan2(a, b));
            else if (functype == B_pow) ret = newfloatval(std::pow(a, b));
            else ret = newfloatval(std::hypot(a, b));
            break;
        }
        case B_floor:
        case B_ceil:
        case B_round:
        case B_trunc: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (std::isnan(value) || std::isinf(value)) {
                yyerror("%s expects a finite number", math_builtin_name(functype));
                break;
            }
            double result = 0.0;
            if (functype == B_floor) result = std::floor(value);
            else if (functype == B_ceil) result = std::ceil(value);
            else if (functype == B_round) result = std::round(value);
            else result = std::trunc(value);
            if (result < -9223372036854775808.0 ||
                result >= 9223372036854775808.0)
                yyerror("%s result out of integer range",
                        math_builtin_name(functype));
            else
                ret = newintval((long long)result);
            break;
        }
        case B_log2:
        case B_log10:
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            ret = newfloatval(functype == B_log2
                                  ? std::log2(as_double(arg))
                                  : std::log10(as_double(arg)));
            break;
        case B_min:
        case B_max: {
            bool all_int = true;
            for (const struct value_t *value : args) {
                if (!is_numeric(value)) {
                    yyerror("%s expects numbers, got %s",
                            math_builtin_name(functype), typename_of(value));
                    return NULL;
                }
                if (value->type != T_int)
                    all_int = false;
            }
            if (all_int) {
                long long best = ((struct int_value_t *)args[0])->value;
                for (size_t i = 1; i < args.size(); ++i) {
                    long long value =
                        ((struct int_value_t *)args[i])->value;
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newintval(best);
            } else {
                double best = as_double(args[0]);
                for (size_t i = 1; i < args.size(); ++i) {
                    double value = as_double(args[i]);
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newfloatval(best);
            }
            break;
        }
        case B_kind_of:
            ret = newstrval(kind_of_value(arg));
            break;
        case B_type_name:
            ret = newstrval(typename_of(arg));
            break;
        case B_type_module:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(
                    sv->def ? sv->def->module_name.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
        case B_type_source:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(sv->def ? sv->def->module.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
        case B_abs:
            if (arg->type == T_int) {
                long long n = ((struct int_value_t *)arg)->value;
                ret = new_int_checked(n < 0 ? -(__int128)n : (__int128)n);
            } else if (arg->type == T_float) {
                ret = newfloatval(std::fabs(((struct float_value_t *)arg)->value));
            } else {
                yyerror("abs expects a number, got %s", typename_of(arg));
            }
            break;
        case B_len:
            if (arg->type == T_string) {
                ret = newintval((long long)utf8_length(
                    ((struct string_value_t *)arg)->value));
            } else if (arg->type == T_bytes) {
                ret = newintval((long long)((struct bytes_value_t *)arg)->value.size());
            } else if (arg->type == T_list) {
                ret = newintval((long long)((struct list_value_t *)arg)->payload->items.size());
            } else if (arg->type == T_dict) {
                ret = newintval((long long)((struct dict_value_t *)arg)->payload->items.size());
            } else {
                yyerror("len expects string, bytes, list, or dict, got %s",
                        typename_of(arg));
            }
            break;
        case B_range: {
            long long start = 0;
            long long length = 0;
            long long step = 1;

            if (args.size() == 1) {
                if (args[0]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[0])->value;
            } else {
                if (args[0]->type != T_int) {
                    yyerror("range start must be an integer");
                    break;
                }
                start = ((struct int_value_t *)args[0])->value;
                if (args[1]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[1])->value;
                if (args.size() == 3) {
                    if (args[2]->type != T_int) {
                        yyerror("range step must be an integer");
                        break;
                    }
                    step = ((struct int_value_t *)args[2])->value;
                }
            }

            if (length < 0) {
                yyerror("range length must be non-negative");
                break;
            }
            if (step == 0) {
                yyerror("range step must not be zero");
                break;
            }

            struct list_value_t *list = newlistval();
            list->payload->items.reserve((size_t)length);
            for (long long i = 0; i < length; ++i) {
                __int128 value = (__int128)start + (__int128)i * step;
                if (value < LLONG_MIN || value > LLONG_MAX) {
                    yyerror("range value overflow");
                    freeval((struct value_t *)list);
                    list = NULL;
                    break;
                }
                list->payload->items.push_back(newintval((long long)value));
            }
            if (list)
                ret = (struct value_t *)list;
            break;
        }
        case B_list_slice: {
            if (args[0]->type != T_list) {
                yyerror("std.list.slice expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("std.list.slice start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("std.list.slice length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            struct list_value_t *source = (struct list_value_t *)args[0];
            size_t size = source->payload->items.size();
            if (start > size)
                start = size;
            if (count > size - start)
                count = size - start;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(count);
            for (size_t i = 0; i < count; ++i)
                out->payload->items.push_back(
                    dupval(source->payload->items[start + i]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_contains:
        case B_list_index: {
            if (args[0]->type != T_list) {
                yyerror("%s expects a list, got %s",
                        functype == B_list_contains ? "std.list.contains"
                                                   : "std.list.index",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            long long found = -1;
            for (size_t i = 0; i < list->payload->items.size(); ++i) {
                if (value_equals(list->payload->items[i], args[1])) {
                    found = (long long)i;
                    break;
                }
            }
            if (functype == B_list_contains)
                ret = newintval(found >= 0 ? 1 : 0);
            else
                ret = newintval(found);
            break;
        }
        case B_list_reverse: {
            if (arg->type != T_list) {
                yyerror("std.list.reverse expects a list, got %s",
                        typename_of(arg));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(source->payload->items.size());
            for (size_t i = source->payload->items.size(); i > 0; --i)
                out->payload->items.push_back(
                    dupval(source->payload->items[i - 1]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_sort: {
            if (arg->type != T_list) {
                yyerror("std.list.sort expects a list, got %s",
                        typename_of(arg));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)arg;
            bool numeric = true;
            bool strings = true;
            for (value_t *item : source->payload->items) {
                if (!is_numeric(item))
                    numeric = false;
                if (item->type != T_string)
                    strings = false;
            }
            if (!numeric && !strings) {
                yyerror("std.list.sort expects all numbers or all strings");
                break;
            }
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(source->payload->items.size());
            for (value_t *item : source->payload->items)
                out->payload->items.push_back(dupval(item));
            bool sort_numeric = numeric;
            std::stable_sort(
                out->payload->items.begin(), out->payload->items.end(),
                [sort_numeric](value_t *a, value_t *b) {
                    if (sort_numeric) {
                        bool unordered = false;
                        int cmp = compare_numeric(a, b, unordered);
                        return !unordered && cmp < 0;
                    }
                    return std::strcmp(
                               ((struct string_value_t *)a)->value,
                               ((struct string_value_t *)b)->value) < 0;
                });
            ret = (struct value_t *)out;
            break;
        }
        case B_list_append: {
            if (args[0]->type != T_list) {
                yyerror("std.list.append expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)args[0];
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(source->payload->items.size() + 1);
            for (value_t *item : source->payload->items)
                out->payload->items.push_back(dupval(item));
            out->payload->items.push_back(dupval(args[1]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_map:
        case B_list_filter:
        case B_list_reduce: {
            if (args[0]->type != T_list) {
                yyerror("%s expects a list, got %s",
                        functype == B_list_map ? "std.list.map" :
                        functype == B_list_filter ? "std.list.filter" :
                        "std.list.reduce",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            struct value_t *fn = args[1];

            if (functype == B_list_reduce) {
                struct value_t *acc = NULL;
                size_t index = 0;
                if (args.size() == 3) {
                    acc = dupval(args[2]);
                } else {
                    if (list->payload->items.empty()) {
                        yyerror("std.list.reduce empty list without initial value");
                        break;
                    }
                    acc = dupval(list->payload->items[0]);
                    index = 1;
                }
                for (; index < list->payload->items.size(); ++index) {
                    std::vector<struct value_t *> call_args;
                    call_args.push_back(dupval(acc));
                    call_args.push_back(
                        dupval(list->payload->items[index]));
                    struct value_t *next =
                        call_value_args(fn, std::move(call_args));
                    if (!next) {
                        freeval(acc);
                        acc = NULL;
                        break;
                    }
                    freeval(acc);
                    acc = next;
                }
                ret = acc;
                break;
            }

            struct list_value_t *out = newlistval();
            out->payload->items.reserve(list->payload->items.size());
            for (value_t *item : list->payload->items) {
                std::vector<struct value_t *> call_args;
                call_args.push_back(dupval(item));
                struct value_t *result =
                    call_value_args(fn, std::move(call_args));
                if (!result) {
                    freeval((struct value_t *)out);
                    out = NULL;
                    break;
                }
                if (functype == B_list_map) {
                    out->payload->items.push_back(result);
                } else {
                    bool keep = truthy(result);
                    freeval(result);
                    if (keep)
                        out->payload->items.push_back(dupval(item));
                }
            }
            if (out)
                ret = (struct value_t *)out;
            break;
        }
        case B_dict_get:
        case B_dict_has: {
            if (args[0]->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_get ? "std.dict.get"
                                               : "std.dict.has",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)args[0];
            struct value_t *found = NULL;
            for (auto &entry : dict->payload->items) {
                if (value_equals(entry.first, args[1])) {
                    found = entry.second;
                    break;
                }
            }
            if (functype == B_dict_has) {
                ret = newintval(found ? 1 : 0);
            } else if (found) {
                ret = dupval(found);
            } else if (args.size() == 3) {
                ret = dupval(args[2]);
            } else {
                ret = newnull();
            }
            break;
        }
        case B_dict_keys:
        case B_dict_values: {
            if (arg->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_keys ? "std.dict.keys"
                                                : "std.dict.values",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items)
                out->payload->items.push_back(dupval(
                    functype == B_dict_keys ? entry.first : entry.second));
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_items: {
            if (arg->type != T_dict) {
                yyerror("std.dict.items expects a dict, got %s",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items) {
                struct list_value_t *pair = newlistval();
                pair->payload->items.push_back(dupval(entry.first));
                pair->payload->items.push_back(dupval(entry.second));
                out->payload->items.push_back((struct value_t *)pair);
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_delete: {
            if (args[0]->type != T_dict) {
                yyerror("std.dict.delete expects a dict, got %s",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *source = (struct dict_value_t *)args[0];
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(source->payload->items.size());
            for (auto &entry : source->payload->items) {
                if (value_equals(entry.first, args[1]))
                    continue;
                out->payload->items.push_back(std::make_pair(
                    dupval(entry.first), dupval(entry.second)));
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_merge: {
            if (args[0]->type != T_dict || args[1]->type != T_dict) {
                yyerror("std.dict.merge expects two dicts");
                break;
            }
            struct dict_value_t *left = (struct dict_value_t *)args[0];
            struct dict_value_t *right = (struct dict_value_t *)args[1];
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(left->payload->items.size() +
                                        right->payload->items.size());
            for (auto &entry : left->payload->items) {
                out->payload->items.push_back(std::make_pair(
                    dupval(entry.first), dupval(entry.second)));
            }
            for (auto &entry : right->payload->items) {
                bool replaced = false;
                for (auto &target : out->payload->items) {
                    if (value_equals(target.first, entry.first)) {
                        freeval(target.second);
                        target.second = dupval(entry.second);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    out->payload->items.push_back(std::make_pair(
                        dupval(entry.first), dupval(entry.second)));
                }
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_string_join: {
            if (args[0]->type != T_list || args[1]->type != T_string) {
                yyerror("std.string.join expects a list and a string separator");
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            const char *separator = ((struct string_value_t *)args[1])->value;
            std::string result;
            bool bad = false;
            for (size_t i = 0; i < list->payload->items.size(); ++i) {
                struct value_t *item = list->payload->items[i];
                if (item->type != T_string) {
                    yyerror("std.string.join expects string elements, got %s",
                            typename_of(item));
                    bad = true;
                    break;
                }
                if (i)
                    result += separator;
                result += ((struct string_value_t *)item)->value;
            }
            if (!bad)
                ret = newstrval(result.c_str());
            break;
        }
        case B_string_replace: {
            if (args[0]->type != T_string || args[1]->type != T_string ||
                args[2]->type != T_string) {
                yyerror("std.string.replace expects three strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *old_text = ((struct string_value_t *)args[1])->value;
            const char *new_text = ((struct string_value_t *)args[2])->value;
            if (!*old_text) {
                yyerror("std.string.replace search must not be empty");
                break;
            }
            std::string source(text);
            std::string old_value(old_text);
            std::string new_value(new_text);
            std::string result;
            size_t position = 0;
            for (;;) {
                size_t found = source.find(old_value, position);
                if (found == std::string::npos) {
                    result.append(source, position, std::string::npos);
                    break;
                }
                result.append(source, position, found - position);
                result += new_value;
                position = found + old_value.size();
            }
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_starts_with:
        case B_string_ends_with: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects two strings",
                        functype == B_string_starts_with
                            ? "std.string.starts_with"
                            : "std.string.ends_with");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *part = ((struct string_value_t *)args[1])->value;
            size_t text_len = std::strlen(text);
            size_t part_len = std::strlen(part);
            bool match = part_len <= text_len;
            if (match && functype == B_string_starts_with)
                match = std::memcmp(text, part, part_len) == 0;
            else if (match)
                match = std::memcmp(text + text_len - part_len, part,
                                    part_len) == 0;
            ret = newintval(match ? 1 : 0);
            break;
        }
        case B_string_contains: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("std.string.contains expects two strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            ret = newintval(std::strstr(text, needle) ? 1 : 0);
            break;
        }
        case B_string_repeat: {
            if (args[0]->type != T_string || args[1]->type != T_int) {
                yyerror("std.string.repeat expects a string and an integer");
                break;
            }
            long long count = ((struct int_value_t *)args[1])->value;
            if (count < 0) {
                yyerror("std.string.repeat count must be non-negative");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            std::string result;
            size_t length = std::strlen(text);
            result.reserve(length * (size_t)count);
            for (long long i = 0; i < count; ++i)
                result += text;
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_trim_start:
        case B_string_trim_end: {
            if (arg->type != T_string) {
                yyerror("%s expects a string",
                        functype == B_string_trim_start
                            ? "std.string.trim_start"
                            : "std.string.trim_end");
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            if (functype == B_string_trim_start) {
                while (begin < end && std::isspace((unsigned char)*begin))
                    ++begin;
            } else {
                while (end > begin && std::isspace((unsigned char)end[-1]))
                    --end;
            }
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
        case B_int:
            if (arg->type == T_int) {
                ret = newintval(((struct int_value_t *)arg)->value);
            } else if (arg->type == T_float) {
                double d = ((struct float_value_t *)arg)->value;
                const double two63 = 9223372036854775808.0;
                if (!std::isfinite(d)) {
                    yyerror("int() value out of range");
                } else {
                    d = std::trunc(d);
                    if (d < -two63 || d >= two63)
                        yyerror("int() value out of range");
                    else
                        ret = newintval((long long)d);
                }
            } else if (arg->type == T_bool) {
                ret = newintval(((struct bool_value_t *)arg)->value ? 1 : 0);
            } else if (arg->type == T_string) {
                long long value = 0;
                if (parse_int_string(((struct string_value_t *)arg)->value, value))
                    ret = newintval(value);
                else
                    yyerror("int() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("int() expects int, float, bool, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_float:
            if (arg->type == T_int) {
                ret = newfloatval((double)((struct int_value_t *)arg)->value);
            } else if (arg->type == T_float) {
                ret = newfloatval(((struct float_value_t *)arg)->value);
            } else if (arg->type == T_bool) {
                ret = newfloatval(((struct bool_value_t *)arg)->value ? 1.0 : 0.0);
            } else if (arg->type == T_string) {
                double value = 0.0;
                if (parse_float_string(((struct string_value_t *)arg)->value, value))
                    ret = newfloatval(value);
                else
                    yyerror("float() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("float() expects int, float, bool, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_bool:
            ret = newboolval(truthy(arg));
            break;
        case B_str:
            ret = newstrval(value_to_string(arg).c_str());
            break;
        case B_command: {
            if (arg->type != T_string) {
                yyerror("command expects a string, got %s", typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command", env))
                    break;
                std::string output;
                int status = 0;
                if (!platform_run_command(
                        ((struct string_value_t *)arg)->value, env, output,
                        status))
                    yyerror("command failed to start");
                else
                    ret = newstrval(output.c_str());
            }
            break;
        }
        case B_command_status: {
            if (arg->type != T_string) {
                yyerror("command_status expects a string, got %s",
                        typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command_status", env))
                    break;
                int status = 0;
                if (!platform_run_command_status(
                        ((struct string_value_t *)arg)->value, env, status))
                    yyerror("command_status failed to start");
                else
                    ret = newintval(status);
            }
            break;
        }
        case B_read_file: {
            if (arg->type != T_string) {
                yyerror("read_file expects a string path, got %s",
                        typename_of(arg));
            } else {
                std::string data;
                std::string error;
                if (!platform_read_file(
                        ((struct string_value_t *)arg)->value, data, error))
                    yyerror("read_file: %s", error.c_str());
                else if (data.find('\0') != std::string::npos)
                    yyerror("read_file: file contains NUL byte");
                else
                    ret = newstrval(data.c_str());
            }
            break;
        }
        case B_write_file:
        case B_append_file: {
            const char *name =
                functype == B_write_file ? "write_file" : "append_file";
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects a string path and data, got %s and %s",
                        name, typename_of(args[0]), typename_of(args[1]));
            } else {
                std::string error;
                if (!platform_write_file(
                        ((struct string_value_t *)args[0])->value,
                        ((struct string_value_t *)args[1])->value,
                        functype == B_append_file, error))
                    yyerror("%s: %s", name, error.c_str());
                else
                    ret = newnull();
            }
            break;
        }
        case B_file_exists:
            if (arg->type != T_string) {
                yyerror("file_exists expects a string path, got %s",
                        typename_of(arg));
            } else {
                ret = newintval(platform_file_exists(
                                    ((struct string_value_t *)arg)->value)
                                    ? 1
                                    : 0);
            }
            break;
        case B_file_open: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("file_open expects a string path and mode, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string mode_error;
            std::string mode = normalize_file_mode(
                ((struct string_value_t *)args[1])->value, mode_error);
            if (mode.empty()) {
                yyerror("file_open: %s", mode_error.c_str());
                break;
            }
            std::string error;
            const char *path = ((struct string_value_t *)args[0])->value;
            platform_file_ptr file = platform_file_open(path, mode, error);
            if (!file)
                yyerror("file_open: %s", error.c_str());
            else
                ret = new_file_value(file, path);
            break;
        }
        case B_file_read: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_read"))
                break;
            size_t limit = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((struct int_value_t *)args[1])->value < 0) {
                    yyerror("file_read size must be a non-negative integer");
                    break;
                }
                limit = (size_t)((struct int_value_t *)args[1])->value;
            }
            std::string data;
            std::string error;
            if (!platform_file_read(file->file, limit, data, error)) {
                yyerror("file_read: %s", error.c_str());
            } else if (data.find('\0') != std::string::npos) {
                yyerror("file_read: file contains NUL byte");
            } else {
                ret = newstrval(data.c_str());
            }
            break;
        }
        case B_file_write: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_write"))
                break;
            if (args[1]->type != T_string) {
                yyerror("file_write expects string data, got %s",
                        typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_file_write(
                    file->file,
                    ((struct string_value_t *)args[1])->value, error))
                yyerror("file_write: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_seek: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_seek"))
                break;
            if (args[1]->type != T_int) {
                yyerror("file_seek offset must be an integer");
                break;
            }
            int whence = 0;
            if (args.size() == 3) {
                if (args[2]->type != T_int) {
                    yyerror("file_seek whence must be an integer");
                    break;
                }
                long long value = ((struct int_value_t *)args[2])->value;
                if (value < 0 || value > 2) {
                    yyerror("file_seek whence must be 0, 1, or 2");
                    break;
                }
                whence = (int)value;
            }
            std::string error;
            if (!platform_file_seek(
                    file->file,
                    ((struct int_value_t *)args[1])->value, whence, error))
                yyerror("file_seek: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_tell: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_tell"))
                break;
            std::string error;
            long long position = platform_file_tell(file->file, error);
            if (position < 0)
                yyerror("file_tell: %s", error.c_str());
            else
                ret = newintval(position);
            break;
        }
        case B_file_eof: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_eof"))
                break;
            if (!platform_file_is_open(file->file)) {
                yyerror("file_eof: file is closed");
                break;
            }
            ret = newintval(platform_file_eof(file->file) ? 1 : 0);
            break;
        }
        case B_file_close: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_close"))
                break;
            std::string error;
            platform_file_close(file->file, error);
            if (!error.empty())
                yyerror("file_close: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_flush: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_flush"))
                break;
            std::string error;
            if (!platform_file_flush(file->file, error))
                yyerror("file_flush: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_input: {
            const char *prompt = NULL;
            if (!args.empty()) {
                if (args[0]->type != T_string) {
                    yyerror("input prompt must be a string, got %s",
                            typename_of(args[0]));
                    break;
                }
                prompt = ((struct string_value_t *)args[0])->value;
            }
            std::string line;
            bool at_eof = false;
            if (!read_console_line(prompt, line, at_eof))
                break;
            if (at_eof && line.empty()) {
                ret = newnull();
            } else if (line.find('\0') != std::string::npos) {
                yyerror("input: line contains NUL byte");
            } else {
                ret = newstrval(line.c_str());
            }
            break;
        }
        case B_split: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("split expects string text and separator, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *separator = ((struct string_value_t *)args[1])->value;
            if (!separator[0]) {
                yyerror("split separator must not be empty");
                break;
            }
            std::string input = text;
            std::string sep = separator;
            struct list_value_t *parts = newlistval();
            size_t position = 0;
            for (;;) {
                size_t found = input.find(sep, position);
                if (found == std::string::npos) {
                    parts->payload->items.push_back(
                        newstrval(input.substr(position).c_str()));
                    break;
                }
                parts->payload->items.push_back(
                    newstrval(input.substr(position, found - position).c_str()));
                position = found + sep.size();
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_split_ws: {
            if (arg->type != T_string) {
                yyerror("split_ws expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            struct list_value_t *parts = newlistval();
            const char *p = text;
            while (*p) {
                while (*p && std::isspace((unsigned char)*p))
                    ++p;
                if (!*p)
                    break;
                const char *start = p;
                while (*p && !std::isspace((unsigned char)*p))
                    ++p;
                std::string part(start, (size_t)(p - start));
                parts->payload->items.push_back(newstrval(part.c_str()));
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_trim: {
            if (arg->type != T_string) {
                yyerror("trim expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            while (begin < end && std::isspace((unsigned char)*begin))
                ++begin;
            while (end > begin && std::isspace((unsigned char)end[-1]))
                --end;
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
        case B_find: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("find expects string text and substring, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            const char *found = std::strstr(text, needle);
            if (!found) {
                ret = newintval(-1);
            } else {
                ret = newintval((long long)utf8_length_prefix(
                    text, (size_t)(found - text)));
            }
            break;
        }
        case B_substr: {
            if (arg->type != T_string) {
                yyerror("substr expects a string, got %s", typename_of(arg));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("substr start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("substr length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            std::string part = utf8_substr(
                ((struct string_value_t *)arg)->value, start, count);
            ret = newstrval(part.c_str());
            break;
        }
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
                    ((struct string_value_t *)arg)->value, data, error))
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
                    ((struct string_value_t *)args[0])->value,
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
        case B_sqrt:
        case B_exp:
        case B_log:
            if (!is_numeric(arg)) {
                yyerror("math function expects a number, got %s", typename_of(arg));
                break;
            }
            if (functype == B_sqrt)
                ret = newfloatval(std::sqrt(as_double(arg)));
            else if (functype == B_exp)
                ret = newfloatval(std::exp(as_double(arg)));
            else
                ret = newfloatval(std::log(as_double(arg)));
            break;
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}

