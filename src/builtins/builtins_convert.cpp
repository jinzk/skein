#include "builtins_internal.h"

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

static const char *kind_of_value(const struct value_t *v) {
    if (!v) return "null";
    switch (v->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_bool: return "bool";
        case T_string: return "string";
        case T_char: return "char";
        case T_byte: return "byte";
        case T_bytes: return "bytes";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_func: return "function";
        case T_builtin: return "builtin";
        case T_native: return "native";
        case T_userdata: return "userdata";
        case T_result: return "result";
        case T_error: return "error";
        case T_struct: return "struct";
        case T_enum: return "enum";
        case T_lru: return "lru";
        case T_deque: return "deque";
        case T_heap: return "heap";
        case T_set: return "set";
        case T_method: return "method";
        case T_null: return "null";
        case T_class: return "class";
        case T_return: return "return";
        case T_break: return "break";
        case T_continue: return "continue";
        default: return "unknown";
    }
}

struct value_t *call_convert_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_kind_of:
            ret = newstrval(kind_of_value(arg));
            break;
        case B_type_name:
            ret = newstrval(typename_of(arg));
            break;

        case B_type_package:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(
                    sv->def ? sv->def->package.c_str() : "");
            } else if (arg && arg->type == T_enum) {
                struct enum_value_t *ev = (struct enum_value_t *)arg;
                ret = newstrval(
                    ev->def ? ev->def->package.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
        case B_type_module_name:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(
                    sv->def ? sv->def->module.c_str() : "");
            } else if (arg && arg->type == T_enum) {
                struct enum_value_t *ev = (struct enum_value_t *)arg;
                ret = newstrval(
                    ev->def ? ev->def->module.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
        case B_type_source:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(sv->def ? sv->def->source.c_str() : "");
            } else if (arg && arg->type == T_enum) {
                struct enum_value_t *ev = (struct enum_value_t *)arg;
                ret = newstrval(ev->def ? ev->def->source.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
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
            } else if (arg->type == T_char) {
                ret = newintval(((struct char_value_t *)arg)->value);
            } else if (arg->type == T_byte) {
                ret = newintval(((struct byte_value_t *)arg)->value);
            } else if (arg->type == T_string) {
                long long value = 0;
                if (parse_int_string(((struct string_value_t *)arg)->value, value))
                    ret = newintval(value);
                else
                    yyerror("int() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("int() expects int, float, bool, char, or string, got %s",
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
        case B_char:
            if (arg->type == T_char) {
                ret = dupval(arg);
            } else if (arg->type == T_int) {
                long long value = ((struct int_value_t *)arg)->value;
                if (value < 0 || value > 0x10FFFF ||
                    (value >= 0xD800 && value <= 0xDFFF)) {
                    yyerror("char() codepoint out of range");
                } else {
                    ret = newcharval((unsigned int)value);
                }
            } else if (arg->type == T_string) {
                const char *text = ((struct string_value_t *)arg)->value;
                size_t consumed = 0;
                unsigned int codepoint = 0;
                if (!utf8_decode_one(text, std::strlen(text), consumed,
                                     codepoint) ||
                    text[consumed] != '\0') {
                    yyerror("char() expects exactly one UTF-8 character");
                } else {
                    ret = newcharval(codepoint);
                }
            } else if (arg->type == T_bytes) {
                const std::string &data =
                    ((struct bytes_value_t *)arg)->value;
                size_t consumed = 0;
                unsigned int codepoint = 0;
                if (!utf8_decode_one(data.data(), data.size(), consumed,
                                     codepoint) ||
                    consumed != data.size()) {
                    yyerror("char() expects exactly one UTF-8 character");
                } else {
                    ret = newcharval(codepoint);
                }
            } else {
                yyerror("char() expects int, char, string, or bytes, got %s",
                        typename_of(arg));
            }
            break;
        case B_byte:
            if (arg->type == T_byte) {
                ret = dupval(arg);
            } else if (arg->type == T_int) {
                long long value = ((struct int_value_t *)arg)->value;
                if (value < 0 || value > 255) {
                    yyerror("byte() value out of range");
                } else {
                    ret = newbyteval((unsigned int)value);
                }
            } else {
                yyerror("byte() expects int or byte, got %s",
                        typename_of(arg));
            }
            break;
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
