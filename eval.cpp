#include "eval.h"
#include "diagnostics.h"
#include "filetype.h"
#include "format.h"
#include "package.h"
#include "platform.h"
#include "repl.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"

int g_runtime_error_count = 0;

#include <climits>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

struct eval_location_guard {
    int old_line;
    int old_column;

    eval_location_guard(int line, int column)
        : old_line(g_error_line), old_column(g_error_column) {
        if (line > 0) {
            g_error_line = line;
            g_error_column = column > 0 ? column : old_column;
        }
    }

    ~eval_location_guard() {
        g_error_line = old_line;
        g_error_column = old_column;
    }
};

struct call_stack_guard {
    call_stack_guard(const std::string &name, int line, int column) {
        diag_push_call(name, line, column);
    }
    ~call_stack_guard() {
        diag_pop_call();
    }
};

} // namespace

static bool is_numeric(const struct value_t *v) {
    return v && (v->type == T_int || v->type == T_float);
}

static struct value_t *new_int_checked(__int128 n) {
    if (n < LLONG_MIN || n > LLONG_MAX) {
        yyerror("integer overflow");
        return NULL;
    }
    return newintval((long long)n);
}

static double as_double(const struct value_t *v) {
    if (!v) return 0.0;
    if (v->type == T_int) return (double)((struct int_value_t *)v)->value;
    if (v->type == T_float) return ((struct float_value_t *)v)->value;
    return 0.0;
}

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
static int compare_int_double(long long i, double d, bool &unordered) {
    unordered = false;
    if (std::isnan(d)) {
        unordered = true;
        return 0;
    }
    if (std::isinf(d))
        return d > 0 ? -1 : 1;

    /* 2^63, the first double that is outside the int64 range. */
    const double two63 = 9223372036854775808.0;
    if (d >= two63) return -1;
    if (d < -two63) return 1;

    double truncated = std::trunc(d);
    long long ti = (long long)truncated;
    if (i < ti) return -1;
    if (i > ti) return 1;
    if (d > truncated) return -1;
    if (d < truncated) return 1;
    return 0;
}

static int compare_numeric(const struct value_t *a, const struct value_t *b,
                           bool &unordered) {
    unordered = false;
    if (a->type == T_int && b->type == T_int) {
        long long x = ((const struct int_value_t *)a)->value;
        long long y = ((const struct int_value_t *)b)->value;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a->type == T_float && b->type == T_float) {
        double x = ((const struct float_value_t *)a)->value;
        double y = ((const struct float_value_t *)b)->value;
        if (std::isnan(x) || std::isnan(y)) {
            unordered = true;
            return 0;
        }
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a->type == T_int) {
        return compare_int_double(((const struct int_value_t *)a)->value,
                                  ((const struct float_value_t *)b)->value,
                                  unordered);
    }
    int cmp = compare_int_double(((const struct int_value_t *)b)->value,
                                 ((const struct float_value_t *)a)->value,
                                 unordered);
    return unordered ? 0 : -cmp;
}

static size_t utf8_sequence_length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t utf8_valid_sequence_length(const char *s, size_t available) {
    size_t n = utf8_sequence_length((unsigned char)s[0]);
    if (n > available) return 1;
    for (size_t i = 1; i < n; ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            return 1;
    }
    return n;
}

static size_t utf8_length(const char *s) {
    size_t len = std::strlen(s);
    size_t count = 0;
    for (size_t i = 0; i < len; ++count)
        i += utf8_valid_sequence_length(s + i, len - i);
    return count;
}

static bool utf8_char_at(const char *s, size_t index, std::string &out) {
    size_t len = std::strlen(s);
    size_t pos = 0;
    size_t current = 0;
    while (pos < len) {
        size_t n = utf8_valid_sequence_length(s + pos, len - pos);
        if (current == index) {
            out.assign(s + pos, n);
            return true;
        }
        pos += n;
        ++current;
    }
    return false;
}

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

static bool truthy(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int: return ((struct int_value_t *)v)->value != 0;
        case T_float: return ((struct float_value_t *)v)->value != 0.0;
        case T_bool: return ((struct bool_value_t *)v)->value;
        case T_string: {
            const char *s = ((struct string_value_t *)v)->value;
            return s && *s;
        }
        case T_bytes: return !((struct bytes_value_t *)v)->value.empty();
        case T_list: return !((struct list_value_t *)v)->payload->items.empty();
        case T_dict: return !((struct dict_value_t *)v)->payload->items.empty();
        case T_struct: return true;
        case T_func: return true;
        case T_builtin: return true;
        case T_userdata: return true;
        case T_null: return false;
        default: return false;
    }
}

static bool switch_value_supported(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int:
        case T_bool:
        case T_string:
        case T_bytes:
        case T_null:
            return true;
        default:
            return false;
    }
}

static bool case_value_is_condition(struct ast *a) {
    if (!a) return false;
    switch (a->nodetype) {
        case 'A':
        case 'Q':
        case '!':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
            return true;
        default:
            return false;
    }
}

static bool case_values_have_condition(struct ast *values) {
    for (struct list_node *p = (struct list_node *)values; p;
         p = (struct list_node *)p->next) {
        if (case_value_is_condition(p->value))
            return true;
    }
    return false;
}

static bool value_equals(struct value_t *a, struct value_t *b) {
    if (!a || !b) return a == b;
    if (is_numeric(a) && is_numeric(b)) {
        bool unordered = false;
        int cmp = compare_numeric(a, b, unordered);
        return !unordered && cmp == 0;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_bool:
            return ((struct bool_value_t *)a)->value ==
                   ((struct bool_value_t *)b)->value;
        case T_string:
            return std::strcmp(((struct string_value_t *)a)->value,
                               ((struct string_value_t *)b)->value) == 0;
        case T_bytes:
            return ((struct bytes_value_t *)a)->value ==
                   ((struct bytes_value_t *)b)->value;
        case T_list: {
            struct list_value_t *la = (struct list_value_t *)a;
            struct list_value_t *lb = (struct list_value_t *)b;
            if (la->payload->items.size() != lb->payload->items.size()) return false;
            for (size_t i = 0; i < la->payload->items.size(); ++i)
                if (!value_equals(la->payload->items[i], lb->payload->items[i])) return false;
            return true;
        }
        case T_dict: {
            struct dict_value_t *da = (struct dict_value_t *)a;
            struct dict_value_t *db = (struct dict_value_t *)b;
            if (da->payload->items.size() != db->payload->items.size()) return false;
            for (size_t i = 0; i < da->payload->items.size(); ++i) {
                bool found = false;
                for (size_t j = 0; j < db->payload->items.size(); ++j) {
                    if (value_equals(da->payload->items[i].first, db->payload->items[j].first)) {
                        if (!value_equals(da->payload->items[i].second, db->payload->items[j].second))
                            return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        case T_func: {
            struct func_value_t *fa = (struct func_value_t *)a;
            struct func_value_t *fb = (struct func_value_t *)b;
            return fa->def == fb->def;
        }
        case T_builtin:
            return ((struct builtin_value_t *)a)->functype ==
                   ((struct builtin_value_t *)b)->functype;
        case T_userdata: {
            struct userdata_value_t *ua = (struct userdata_value_t *)a;
            struct userdata_value_t *ub = (struct userdata_value_t *)b;
            return ua->meta == ub->meta && ua->data == ub->data;
        }
        case T_struct: {
            struct struct_value_t *sa = (struct struct_value_t *)a;
            struct struct_value_t *sb = (struct struct_value_t *)b;
            if (sa->def != sb->def) return false;
            if (!sa->payload || !sb->payload) return sa->payload == sb->payload;
            if (sa->payload->fields.size() != sb->payload->fields.size())
                return false;
            for (size_t i = 0; i < sa->payload->fields.size(); ++i)
                if (!value_equals(sa->payload->fields[i],
                                  sb->payload->fields[i]))
                    return false;
            return true;
        }
        case T_null:
            return true;
        default:
            return false;
    }
}

static struct value_t *do_compare(int op, struct value_t *l, struct value_t *r) {
    if (!l || !r) return NULL;

    if (!(is_numeric(l) && is_numeric(r)) &&
        !(l->type == T_string && r->type == T_string)) {
        if (op == 3 || op == 4) {
            bool eq = value_equals(l, r);
            return newboolval((op == 4) ? eq : !eq);
        }
        yyerror("cannot compare %s and %s", typename_of(l), typename_of(r));
        return NULL;
    }

    if (is_numeric(l) && is_numeric(r)) {
        bool unordered = false;
        int cmp = compare_numeric(l, r, unordered);
        if (unordered) {
            /* IEEE 754: NaN is unequal to everything and unordered. */
            if (op == 3) return newboolval(true);
            return newboolval(false);
        }
        switch (op) {
            case 1: return newboolval(cmp > 0);
            case 2: return newboolval(cmp < 0);
            case 3: return newboolval(cmp != 0);
            case 4: return newboolval(cmp == 0);
            case 5: return newboolval(cmp >= 0);
            case 6: return newboolval(cmp <= 0);
            default: return NULL;
        }
    }

    int cmp = std::strcmp(((struct string_value_t *)l)->value,
                          ((struct string_value_t *)r)->value);
    cmp = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);

    switch (op) {
        case 1: return newboolval(cmp > 0);
        case 2: return newboolval(cmp < 0);
        case 3: return newboolval(cmp != 0);
        case 4: return newboolval(cmp == 0);
        case 5: return newboolval(cmp >= 0);
        case 6: return newboolval(cmp <= 0);
        default: return NULL;
    }
}

static void free_args(std::vector<struct value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

/*
 * Function arguments are represented as a right-nested 'L' chain.  Evaluate
 * them left-to-right without using eval('L'), because eval('L') intentionally
 * frees intermediate statement values.
 */
static bool collect_args(struct ast *a, std::vector<struct value_t *> &out) {
    if (!a) return true;
    if (out.capacity() == 0)
        out.reserve(8);
    if (a->nodetype == 'L') {
        if (!collect_args(a->l, out)) return false;
        return collect_args(a->r, out);
    }
    struct value_t *v = eval(a);
    if (!v) return false;
    if (v->type == T_return || v->type == T_break || v->type == T_continue) {
        freeval(v);
        yyerror("invalid argument expression");
        return false;
    }
    out.push_back(v);
    return true;
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

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args);
static struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args);

static struct value_t *call_builtin(int functype,
                                    const std::vector<struct value_t *> &args) {
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
    } else if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        return NULL;
    }

    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
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

static constexpr int MAX_CALL_DEPTH = 1000;
static int call_depth = 0;

namespace {
struct call_depth_guard {
    call_depth_guard() { ++call_depth; }
    ~call_depth_guard() { --call_depth; }
};
}

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }

    if (callee->type == T_builtin) {
        struct value_t *ret = call_builtin(
            ((struct builtin_value_t *)callee)->functype, args);
        free_args(args);
        return ret;
    }

    if (callee->type != T_func) {
        yyerror("cannot call %s", typename_of(callee));
        free_args(args);
        return NULL;
    }

    struct func_value_t *fv = (struct func_value_t *)callee;
    /*
     * Keep the function definition alive for the whole call.  The body can
     * reassign/free the variable that holds this function value.
     */
    std::shared_ptr<function_def> def = fv->def;
    int nparams = def->code->nparams;
    if ((int)args.size() != nparams) {
        yyerror("function expects %d argument(s), got %d",
                nparams, (int)args.size());
        free_args(args);
        return NULL;
    }

    std::string frame_name =
        def->code->name.empty() ? "<anonymous>" : def->code->name;

    int i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        if (!type_constraint_matches(sl->type, args[i])) {
            yyerror("function %s argument %d (%s) expects %s, got %s",
                    frame_name.c_str(), i + 1, sl->sym->name,
                    type_constraint_name(sl->type), typename_of(args[i]));
            free_args(args);
            return NULL;
        }
    }

    int errors_before = g_runtime_error_count;
    int frame_line = g_error_line;
    int frame_column = g_error_column;
    if (frame_line <= 0 && def->code->ast) {
        frame_line = def->code->ast->line;
        frame_column = def->code->ast->column;
    }
    call_stack_guard frame_guard(frame_name, frame_line, frame_column);
    pushscope_closure(def->closure);
    i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        struct symbol *sym = newsym(sl->sym->name);
        sym->val = args[i];
        if (sl->type) {
            sym->type.kind = sl->type->kind;
            sym->type.name = sl->type->name;
        }
        if (i == 0 && def->code->is_method) {
            sym->is_self = true;
            sym->is_const = true;
        }
        savesym(sym);
    }
    args.clear(); /* ownership transferred to the function scope */

    struct value_t *body_ret = eval(def->code->ast);
    struct value_t *result = topscope()->ret;
    topscope()->ret = NULL;

    if (body_ret) {
        if (body_ret->type == T_break) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("break outside loop in function");
        } else if (body_ret->type == T_continue) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("continue outside loop in function");
        }
        freeval(body_ret);
    }

    popscope();

    bool errored = g_runtime_error_count != errors_before;
    if (g_interrupted || errored) {
        freeval(result);
        return NULL;
    }
    return result ? result : newnull();
}

static struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        free_args(args);
        return NULL;
    }
    call_depth_guard depth_guard;
    return invoke_value(callee, args);
}

static const char *import_binding_name(const char *path) {
    while (path && (*path == '.' || *path == '@'))
        ++path;
    if (!path || !*path)
        return "";
    const char *dot = std::strrchr(path, '.');
    return dot ? dot + 1 : path;
}

static struct value_t *callvalue(struct value_t *callee, struct ast *args_ast) {
    if (!callee)
        return NULL;

    /*
     * Keep the call-depth guard before argument evaluation so deeply nested
     * call expressions cannot exhaust the C++ stack.
     */
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    if (!collect_args(args_ast, args)) {
        free_args(args);
        return NULL;
    }

    return invoke_value(callee, args);
}

static struct ast *index_base(struct ast *a) {
    while (a && a->nodetype == 'G') a = a->l;
    return a;
}

/*
 * Collect the index expressions of an lvalue chain such as a[i][j].  The base
 * must be a variable; assigning through function calls or literals is not
 * allowed.
 */
static bool collect_index_asts(struct ast *a, std::vector<struct ast *> &out) {
    if (!a) return false;
    if (a->nodetype == 'G') {
        if (!collect_index_asts(a->l, out)) return false;
        out.push_back(a->r);
        return true;
    }
    return a->nodetype == 'N';
}

/*
 * Detach a shared payload before mutating it.  The new payload owns fresh
 * wrapper values; nested containers are still shared through their own
 * copy-on-write payloads.
 */
static void ensure_unique_list(struct list_value_t *list) {
    if (!list->payload) {
        list->payload = std::make_shared<list_payload>();
        return;
    }
    if (list->payload.use_count() == 1)
        return;

    std::shared_ptr<list_payload> copy = std::make_shared<list_payload>();
    copy->items.reserve(list->payload->items.size());
    for (size_t i = 0; i < list->payload->items.size(); ++i)
        copy->items.push_back(dupval(list->payload->items[i]));
    list->payload = std::move(copy);
}

static void ensure_unique_dict(struct dict_value_t *dict) {
    if (!dict->payload) {
        dict->payload = std::make_shared<dict_payload>();
        return;
    }
    if (dict->payload.use_count() == 1)
        return;

    std::shared_ptr<dict_payload> copy = std::make_shared<dict_payload>();
    copy->items.reserve(dict->payload->items.size());
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        copy->items.push_back(std::make_pair(
            dupval(dict->payload->items[i].first),
            dupval(dict->payload->items[i].second)));
    }
    dict->payload = std::move(copy);
}

static void ensure_unique_container(struct value_t *container) {
    if (!container) return;
    if (container->type == T_list)
        ensure_unique_list((struct list_value_t *)container);
    else if (container->type == T_dict)
        ensure_unique_dict((struct dict_value_t *)container);
    else if (container->type == T_struct)
        ensure_unique_struct((struct struct_value_t *)container);
}

static struct value_t **index_slot_ptr(struct value_t *container,
                                       struct value_t *idx) {
    if (!container || !idx) return NULL;
    if (container->type == T_list) {
        if (idx->type != T_int) return NULL;
        long long i = ((struct int_value_t *)idx)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) return NULL;
        return &lv->payload->items[(size_t)i];
    }
    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->payload->items.size(); ++i)
            if (value_equals(dv->payload->items[i].first, idx))
                return &dv->payload->items[i].second;
    }
    if (container->type == T_struct) {
        if (idx->type != T_string) return NULL;
        struct struct_value_t *sv = (struct struct_value_t *)container;
        if (!sv->def || !sv->payload) return NULL;
        int field = structtype_field_index(
            sv->def, ((struct string_value_t *)idx)->value);
        if (field < 0 || (size_t)field >= sv->payload->fields.size())
            return NULL;
        return &sv->payload->fields[(size_t)field];
    }
    return NULL;
}

static struct value_t *apply_increment(struct value_t *v, int delta) {
    if (v && v->type == T_int) {
        long long n = ((struct int_value_t *)v)->value;
        return new_int_checked((__int128)n + delta);
    }
    if (v && v->type == T_float) {
        double d = ((struct float_value_t *)v)->value;
        return newfloatval(d + (double)delta);
    }
    yyerror("cannot %s %s", delta > 0 ? "increment" : "decrement",
            typename_of(v));
    return NULL;
}

/*
 * Resolve the container that owns the final index of a write path.  Every
 * intermediate container is made unique before its child is inspected, so a
 * subsequent write cannot mutate a value that is still shared.
 */
static bool resolve_write_container(
    struct symbol *s, const std::vector<struct value_t *> &indices,
    struct value_t *&container) {
    container = s ? s->val : NULL;
    for (size_t k = 0; k + 1 < indices.size(); ++k) {
        ensure_unique_container(container);
        struct value_t **slot = index_slot_ptr(container, indices[k]);
        if (!slot || !*slot) {
            yyerror("invalid index assignment path");
            return false;
        }
        struct value_t *elem = *slot;
        if (elem->type != T_list && elem->type != T_dict &&
            elem->type != T_struct) {
            yyerror("cannot index-assign into %s", typename_of(elem));
            return false;
        }
        container = elem;
    }
    return true;
}

/* Assigns through an index/field slot.  Ownership of `val` is consumed. */
static struct value_t *write_member_value(struct value_t *container,
                                          struct value_t *idx,
                                          struct value_t *val) {
    struct value_t *ret = NULL;

    if (container->type == T_list) {
        if (idx->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            struct list_value_t *lv = (struct list_value_t *)container;
            if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
                yyerror("list index %lld out of range", i);
            } else {
                ensure_unique_list(lv);
                freeval(lv->payload->items[(size_t)i]);
                lv->payload->items[(size_t)i] = val;
                val = NULL;
                ret = dupval(lv->payload->items[(size_t)i]);
            }
        }
    } else if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        ensure_unique_dict(dv);
        bool found = false;
        for (size_t i = 0; i < dv->payload->items.size(); ++i) {
            if (value_equals(dv->payload->items[i].first, idx)) {
                freeval(dv->payload->items[i].second);
                dv->payload->items[i].second = val;
                val = NULL;
                found = true;
                ret = dupval(dv->payload->items[i].second);
                break;
            }
        }
        if (!found) {
            dv->payload->items.push_back(std::make_pair(dupval(idx), val));
            val = NULL;
            ret = dupval(dv->payload->items.back().second);
        }
    } else if (container->type == T_struct) {
        if (idx->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(idx));
        } else {
            struct struct_value_t *sv = (struct struct_value_t *)container;
            const char *name = ((struct string_value_t *)idx)->value;
            int field = sv->def ? structtype_field_index(sv->def, name) : -1;
            if (field < 0 || !sv->def ||
                (size_t)field >= sv->def->fields.size()) {
                yyerror("field %s not found in %s", name,
                        sv->def ? sv->def->name.c_str() : "struct");
            } else if (!type_constraint_matches(
                           &sv->def->fields[(size_t)field].type, val)) {
                yyerror("field %s of %s expects %s, got %s", name,
                        sv->def->name.c_str(),
                        type_constraint_name(
                            &sv->def->fields[(size_t)field].type),
                        typename_of(val));
            } else {
                ensure_unique_struct(sv);
                if (!sv->payload ||
                    (size_t)field >= sv->payload->fields.size()) {
                    yyerror("invalid struct field assignment");
                } else {
                    freeval(sv->payload->fields[(size_t)field]);
                    sv->payload->fields[(size_t)field] = val;
                    val = NULL;
                    ret = dupval(sv->payload->fields[(size_t)field]);
                }
            }
        }
    } else if (container->type == T_bytes) {
        if (idx->type != T_int) {
            yyerror("bytes index must be an integer, got %s", typename_of(idx));
        } else if (val->type != T_int) {
            yyerror("bytes value must be an integer, got %s", typename_of(val));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            long long byte = ((struct int_value_t *)val)->value;
            std::string &data = ((struct bytes_value_t *)container)->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes index %lld out of range", i);
            } else if (byte < 0 || byte > 255) {
                yyerror("bytes value %lld out of range", byte);
            } else {
                data[(size_t)i] = (char)(unsigned char)byte;
                ret = newintval(byte);
            }
        }
    } else {
        yyerror("cannot index-assign into %s", typename_of(container));
    }

    freeval(val);
    return ret;
}

static struct value_t *increment_member_value(struct value_t *container,
                                              struct value_t *idx, int delta,
                                              bool prefix) {
    struct value_t *ret = NULL;

    if (container->type == T_list) {
        if (idx->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            struct list_value_t *lv = (struct list_value_t *)container;
            if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
                yyerror("list index %lld out of range", i);
            } else {
                ensure_unique_list(lv);
                struct value_t **slot = &lv->payload->items[(size_t)i];
                struct value_t *newv = apply_increment(*slot, delta);
                if (newv) {
                    ret = prefix ? dupval(newv) : dupval(*slot);
                    freeval(*slot);
                    *slot = newv;
                }
            }
        }
    } else if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        ensure_unique_dict(dv);
        bool found = false;
        for (size_t i = 0; i < dv->payload->items.size(); ++i) {
            if (!value_equals(dv->payload->items[i].first, idx))
                continue;
            found = true;
            struct value_t *old = dv->payload->items[i].second;
            struct value_t *newv = apply_increment(old, delta);
            if (newv) {
                ret = prefix ? dupval(newv) : dupval(old);
                freeval(old);
                dv->payload->items[i].second = newv;
            }
            break;
        }
        if (!found)
            yyerror("key not found");
    } else if (container->type == T_struct) {
        if (idx->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(idx));
        } else {
            struct struct_value_t *sv = (struct struct_value_t *)container;
            const char *name = ((struct string_value_t *)idx)->value;
            int field = sv->def ? structtype_field_index(sv->def, name) : -1;
            if (field < 0 || !sv->def ||
                (size_t)field >= sv->def->fields.size()) {
                yyerror("field %s not found in %s", name,
                        sv->def ? sv->def->name.c_str() : "struct");
            } else {
                ensure_unique_struct(sv);
                if (!sv->payload ||
                    (size_t)field >= sv->payload->fields.size()) {
                    yyerror("invalid struct field assignment");
                } else {
                    struct value_t *old =
                        sv->payload->fields[(size_t)field];
                    struct value_t *newv = apply_increment(old, delta);
                    if (newv &&
                        !type_constraint_matches(
                            &sv->def->fields[(size_t)field].type, newv)) {
                        yyerror("field %s of %s expects %s, got %s", name,
                                sv->def->name.c_str(),
                                type_constraint_name(
                                    &sv->def->fields[(size_t)field].type),
                                typename_of(newv));
                        freeval(newv);
                    } else if (newv) {
                        ret = prefix ? dupval(newv) : dupval(old);
                        freeval(old);
                        sv->payload->fields[(size_t)field] = newv;
                    }
                }
            }
        }
    } else if (container->type == T_bytes) {
        if (idx->type != T_int) {
            yyerror("bytes index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            std::string &data = ((struct bytes_value_t *)container)->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes index %lld out of range", i);
            } else {
                long long old = (unsigned char)data[(size_t)i];
                long long updated = old + delta;
                if (updated < 0 || updated > 255) {
                    yyerror("bytes value %lld out of range", updated);
                } else {
                    ret = newintval(prefix ? updated : old);
                    data[(size_t)i] = (char)(unsigned char)updated;
                }
            }
        }
    } else {
        yyerror("cannot index-assign into %s", typename_of(container));
    }

    return ret;
}

static struct value_t *assign_index(struct ast *target, struct ast *value_ast) {
    std::vector<struct ast *> index_asts;
    if (!collect_index_asts(target, index_asts)) {
        yyerror("cannot assign through this expression");
        return NULL;
    }
    struct ast *base = index_base(target);
    if (!base || base->nodetype != 'N') {
        yyerror("cannot assign through this expression");
        return NULL;
    }

    /*
     * Evaluate the right-hand side and every index expression before walking
     * the container.  This prevents user code from invalidating pointers into
     * a list/dict vector while we are assigning.
     */
    struct value_t *val = eval(value_ast);
    if (!val) return NULL;

    std::vector<struct value_t *> indices;
    for (size_t i = 0; i < index_asts.size(); ++i) {
        struct value_t *idx = eval(index_asts[i]);
        if (!idx) {
            freeval(val);
            free_args(indices);
            return NULL;
        }
        indices.push_back(idx);
    }

    struct symbol *s = lookupsym(((struct symref *)base)->name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        freeval(val);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    if (!resolve_write_container(s, indices, container)) {
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret =
        write_member_value(container, indices.back(), val);
    free_args(indices);
    return ret;
}

static struct value_t *increment_index(struct ast *target, int delta,
                                       bool prefix) {
    std::vector<struct ast *> index_asts;
    if (!collect_index_asts(target, index_asts)) {
        yyerror("cannot assign through this expression");
        return NULL;
    }
    struct ast *base = index_base(target);
    if (!base || base->nodetype != 'N') {
        yyerror("cannot assign through this expression");
        return NULL;
    }

    std::vector<struct value_t *> indices;
    for (size_t i = 0; i < index_asts.size(); ++i) {
        struct value_t *idx = eval(index_asts[i]);
        if (!idx) {
            free_args(indices);
            return NULL;
        }
        indices.push_back(idx);
    }

    struct symbol *s = lookupsym(((struct symref *)base)->name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    if (!resolve_write_container(s, indices, container)) {
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = increment_member_value(
        container, indices.back(), delta, prefix);
    free_args(indices);
    return ret;
}

static bool is_read_only(const struct ast *a) {
    if (!a) return true;
    switch (a->nodetype) {
        case 'V':
        case 'N':
            return true;
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case 'M':
        case '!':
        case '~':
        case 'G':
            return is_read_only(a->l) && is_read_only(a->r);
        default:
            return false;
    }
}

/*
 * Read one indexing step without copying the container.  For lists and
 * dictionaries `next` points into the container and stays borrowed; for
 * strings a new one-character string is returned and must be freed.
 */
static bool index_read_step(struct value_t *container, struct value_t *index,
                            struct value_t **next, bool *next_owned) {
    *next = NULL;
    *next_owned = false;

    if (container->type == T_list) {
        if (index->type != T_int) {
            yyerror("list index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
            yyerror("list index %lld out of range", i);
            return false;
        }
        *next = lv->payload->items[(size_t)i];
        return true;
    }

    if (container->type == T_string) {
        if (index->type != T_int) {
            yyerror("string index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        const char *s = ((struct string_value_t *)container)->value;
        size_t len = utf8_length(s);
        if (i < 0 || (unsigned long long)i >= len) {
            yyerror("string index %lld out of range", i);
            return false;
        }
        std::string ch;
        if (!utf8_char_at(s, (size_t)i, ch))
            return false;
        *next = newstrval(ch.c_str());
        *next_owned = true;
        return true;
    }

    if (container->type == T_bytes) {
        if (index->type != T_int) {
            yyerror("bytes index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        std::string &data = ((struct bytes_value_t *)container)->value;
        if (i < 0 || (unsigned long long)i >= data.size()) {
            yyerror("bytes index %lld out of range", i);
            return false;
        }
        *next = newintval((unsigned char)data[(size_t)i]);
        *next_owned = true;
        return true;
    }

    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->payload->items.size(); ++i) {
            if (value_equals(dv->payload->items[i].first, index)) {
                *next = dv->payload->items[i].second;
                return true;
            }
        }
        yyerror("key not found");
        return false;
    }

    yyerror("cannot index %s", typename_of(container));
    return false;
}

/*
 * Fast read path for expressions rooted at a variable, e.g. a[i] or a[i][j].
 * It borrows the variable's container instead of duplicating the whole value.
 * Only used when every index expression is side-effect free, so evaluating
 * the indices cannot invalidate the borrowed container.
 */
static bool read_index_chain(struct ast *a, struct value_t *&out) {
    std::vector<struct ast *> index_asts;
    if (!collect_index_asts(a, index_asts))
        return false;
    for (struct ast *idx : index_asts) {
        if (!is_read_only(idx))
            return false;
    }

    struct ast *base = index_base(a);
    struct symbol *sym = lookupsym(((struct symref *)base)->name);
    if (!sym || !sym->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        out = NULL;
        return true;
    }

    struct value_t *container = sym->val;
    bool container_owned = false;

    for (size_t k = 0; k < index_asts.size(); ++k) {
        if (container->type == T_struct) {
            if (container_owned) freeval(container);
            return false;
        }
        struct value_t *idx = eval(index_asts[k]);
        if (!idx) {
            if (container_owned) freeval(container);
            out = NULL;
            return true;
        }

        struct value_t *next = NULL;
        bool next_owned = false;
        bool ok = index_read_step(container, idx, &next, &next_owned);
        freeval(idx);
        if (!ok) {
            if (container_owned) freeval(container);
            out = NULL;
            return true;
        }

        if (k + 1 == index_asts.size()) {
            out = next_owned ? next : dupval(next);
            if (container_owned) freeval(container);
            return true;
        }

        /*
         * A borrowed child of an owned string cannot occur in the current
         * type system, but duplicate defensively if it ever does.
         */
        if (container_owned && !next_owned) {
            next = dupval(next);
            next_owned = true;
        }
        if (container_owned) freeval(container);
        container = next;
        container_owned = next_owned;
    }

    out = NULL;
    return true;
}

static struct value_t *default_value_for_type(
    const struct type_constraint *type) {
    enum type_constraint_kind kind = type ? type->kind : TYPE_ANY;
    switch (kind) {
        case TYPE_INT:
        case TYPE_NUMBER:
            return newintval(0);
        case TYPE_FLOAT:
            return newfloatval(0.0);
        case TYPE_BOOL:
            return newboolval(false);
        case TYPE_STRING:
            return newstrval("");
        case TYPE_BYTES:
            return newbytesval(std::string());
        case TYPE_LIST:
            return (struct value_t *)newlistval();
        case TYPE_DICT:
            return (struct value_t *)newdictval();
        case TYPE_NULL:
        case TYPE_ANY:
            return newnull();
        case TYPE_FUNCTION:
        case TYPE_BUILTIN:
        case TYPE_CALLABLE:
        case TYPE_USERDATA:
        case TYPE_INVALID:
        default:
            return NULL;
    }
}

static struct value_t *assign_name(struct symref *ref, struct ast *value_ast) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", ref->name);
        return NULL;
    }
    struct value_t *v = eval(value_ast);
    if (!v) return NULL;
    if (!type_constraint_matches(&s->type, v)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(v));
        freeval(v);
        return NULL;
    }
    freeval(s->val);
    s->val = v;
    return dupval(v);
}

static struct value_t *increment_name(struct symref *ref, int delta,
                                      bool prefix) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", ref->name);
        return NULL;
    }
    struct value_t *newv = apply_increment(s->val, delta);
    if (!newv) return NULL;
    if (!type_constraint_matches(&s->type, newv)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(newv));
        freeval(newv);
        return NULL;
    }
    struct value_t *ret = prefix ? dupval(newv) : dupval(s->val);
    freeval(s->val);
    s->val = newv;
    return ret;
}

static struct value_t *read_member_value(struct value_t *container,
                                         struct value_t *index) {
    if (!container || !index)
        return NULL;

    if (container->type == T_list) {
        if (index->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
            yyerror("list index %lld out of range", i);
            return NULL;
        }
        return dupval(lv->payload->items[(size_t)i]);
    }

    if (container->type == T_string) {
        if (index->type != T_int) {
            yyerror("string index must be an integer, got %s",
                    typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        const char *s = ((struct string_value_t *)container)->value;
        size_t len = utf8_length(s);
        if (i < 0 || (unsigned long long)i >= len) {
            yyerror("string index %lld out of range", i);
            return NULL;
        }
        std::string ch;
        if (!utf8_char_at(s, (size_t)i, ch))
            return NULL;
        return newstrval(ch.c_str());
    }

    if (container->type == T_bytes) {
        if (index->type != T_int) {
            yyerror("bytes index must be an integer, got %s",
                    typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        std::string &data = ((struct bytes_value_t *)container)->value;
        if (i < 0 || (unsigned long long)i >= data.size()) {
            yyerror("bytes index %lld out of range", i);
            return NULL;
        }
        return newintval((unsigned char)data[(size_t)i]);
    }

    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->payload->items.size(); ++i)
            if (value_equals(dv->payload->items[i].first, index))
                return dupval(dv->payload->items[i].second);
        yyerror("key not found");
        return NULL;
    }

    if (container->type == T_struct) {
        if (index->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(index));
            return NULL;
        }
        struct struct_value_t *sv = (struct struct_value_t *)container;
        if (!sv->def) {
            yyerror("invalid struct value");
            return NULL;
        }
        const char *name = ((struct string_value_t *)index)->value;
        int field = structtype_field_index(sv->def, name);
        if (field >= 0 && sv->payload &&
            (size_t)field < sv->payload->fields.size())
            return dupval(sv->payload->fields[(size_t)field]);
        if (sv->def->methods.count(name) != 0)
            yyerror("method %s must be called", name);
        else
            yyerror("field %s not found in %s", name, sv->def->name.c_str());
        return NULL;
    }

    yyerror("cannot index %s", typename_of(container));
    return NULL;
}

static struct value_t *call_method_value(struct value_t *receiver,
                                         std::shared_ptr<function_def> def,
                                         struct ast *args_ast) {
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    args.reserve(8);
    args.push_back(dupval(receiver));
    if (!collect_args(args_ast, args)) {
        free_args(args);
        return NULL;
    }

    struct func_value_t callee;
    callee.type = T_func;
    callee.def = std::move(def);
    return invoke_value((struct value_t *)&callee, args);
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    if (g_interrupted) return NULL;
    eval_location_guard location_guard(a->line, a->column);
    struct value_t *ret = NULL;

    switch (a->nodetype) {
        case 'V':
            ret = dupval(((struct val *)a)->val);
            break;

        case 'N': {
            struct symbol *s = lookupsym(((struct symref *)a)->name);
            if (!s) {
                yyerror("symbol not found %s", ((struct symref *)a)->name);
            } else {
                ret = dupval(s->val);
            }
            break;
        }

        case 'J': {
            struct import_node *imp = (struct import_node *)a;
            std::string error;
            int errors_before = diag_error_count();
            struct value_t *ns = package_import(imp->path, imp->base, error);
            if (!ns) {
                if (diag_error_count() == errors_before)
                    yyerror("import %s: %s", imp->path, error.c_str());
                else
                    diag_mark_script_failure();
                break;
            }
            const char *name = imp->alias
                                   ? imp->alias
                                   : import_binding_name(imp->path);
            if (isrepeated(name)) {
                yyerror("import name already exists: %s", name);
                freeval(ns);
            } else {
                struct symbol *sym = newsym(name);
                sym->val = ns;
                savesym(sym);
            }
            break;
        }

        case 'K': {
            struct package_node *pkg = (struct package_node *)a;
            int errors_before = g_runtime_error_count;
            std::string error;

            if (package_is_loading())
                pushscope();
            else
                pushscope_isolated();

            struct value_t *body_ret = eval(pkg->body);
            if (body_ret &&
                (body_ret->type == T_return || body_ret->type == T_break ||
                 body_ret->type == T_continue)) {
                yyerror("invalid control flow in package %s", pkg->name);
            }
            value_t *ns = NULL;
            if (g_runtime_error_count == errors_before)
                ns = package_build_namespace(current_scope().get(), error);
            popscope();

            freeval(body_ret);

            if (!ns) {
                if (g_runtime_error_count == errors_before)
                    yyerror("package %s: %s", pkg->name,
                            error.empty() ? "cannot build namespace"
                                          : error.c_str());
                break;
            }

            if (!package_publish(pkg->name, ns, error)) {
                yyerror("package %s: %s", pkg->name, error.c_str());
                freeval(ns);
            }
            break;
        }

        case 'D': {
            struct symdef *def = (struct symdef *)a;
            char *name = def->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else if (!validate_type_constraint(def->type)) {
                break;
            } else {
                struct value_t *v = def->v
                    ? eval(def->v)
                    : default_value_for_type(def->type);
                if (!v) {
                    if (!def->v)
                        yyerror("variable %s of type %s requires an initializer",
                                name, type_constraint_name(def->type));
                    break;
                }
                if (!type_constraint_matches(def->type, v)) {
                    yyerror("variable %s expects %s, got %s", name,
                            type_constraint_name(def->type), typename_of(v));
                    freeval(v);
                    break;
                }
                struct symbol *s = newsym(name);
                s->val = v;
                if (def->type) {
                    s->type.kind = def->type->kind;
                    s->type.name = def->type->name;
                }
                s->exported = def->exported;
                s->is_const = def->is_const;
                savesym(s);
            }
            break;
        }

        case 'Y':
            eval_struct_decl((struct struct_node *)a);
            break;

        case 'H':
            eval_impl_decl((struct impl_node *)a);
            break;

        case 't':
            eval_trait_decl((struct trait_node *)a);
            break;

        case 'y':
            ret = eval_struct_lit((struct struct_lit_node *)a);
            break;

        case 'Z': {
            struct funcdef *f = (struct funcdef *)a;
            char *name = f->name;
            struct symlist *sl = f->sl;
            const char *display_name = name ? name : "<anonymous>";
            bool bad = false;
            for (struct symlist *p = sl; p && !bad; p = p->next)
                for (struct symlist *q = p->next; q; q = q->next)
                    if (!std::strcmp(p->sym->name, q->sym->name)) {
                        yyerror("duplicate parameter %s in function %s",
                                p->sym->name, display_name);
                        bad = true;
                        break;
                    }
            for (struct symlist *p = sl; p && !bad; p = p->next)
                if (!validate_type_constraint(p->type))
                    bad = true;
            if (bad)
                break;

            std::shared_ptr<scope_t> closure = current_scope();
            /* Top-level functions resolve globals through the fallback parent. */
            if (closure && closure->is_global)
                closure.reset();

            if (!name) {
                std::shared_ptr<function_code> code = get_function_code(f);
                f->moved = 1;
                ret = newfuncval(code, closure);
            } else if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                std::shared_ptr<function_code> code = get_function_code(f);
                struct symbol *sym = newsym(name);
                sym->val = newfuncval(code, closure);
                sym->exported = f->exported;
                f->moved = 1;
                savesym(sym);
            }
            break;
        }

        case 'R': {
            struct ast *value_ast = ((struct fnret *)a)->v;
            struct value_t *v = eval(value_ast);
            if (value_ast && !v)
                break;
            if (!v)
                v = newnull();
            if (!retval(v)) {
                yyerror("return outside function");
                freeval(v);
                break;
            }
            ret = newreturn();
            break;
        }

        case 'B':
            diag_set_flow_location(a->line, a->column);
            ret = newbreak();
            break;

        case 'O':
            diag_set_flow_location(a->line, a->column);
            ret = newcontinue();
            break;

        case 'X': {
            pushscope();
            ret = eval(((struct nscope *)a)->ast);
            popscope();
            break;
        }

        case '=': {
            struct symasgn *asgn = (struct symasgn *)a;
            if (asgn->target->nodetype == 'N')
                ret = assign_name((struct symref *)asgn->target, asgn->v);
            else if (asgn->target->nodetype == 'G')
                ret = assign_index(asgn->target, asgn->v);
            else
                yyerror("cannot assign to this expression");
            break;
        }

        case 'U': {
            struct incdec_node *u = (struct incdec_node *)a;
            if (u->target->nodetype == 'N')
                ret = increment_name((struct symref *)u->target, u->delta,
                                     u->prefix);
            else if (u->target->nodetype == 'G')
                ret = increment_index(u->target, u->delta, u->prefix);
            else
                yyerror("cannot %s this expression",
                        u->delta > 0 ? "increment" : "decrement");
            break;
        }

        case 'l': {
            struct list_node *p = (struct list_node *)a;
            struct list_value_t *v = newlistval();
            while (p != NULL) {
                if (p->value) {
                    struct value_t *item = eval(p->value);
                    if (!item) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    v->payload->items.push_back(item);
                }
                p = (struct list_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            struct dict_value_t *v = newdictval();
            while (p != NULL) {
                if (p->key && p->value) {
                    struct value_t *key = eval(p->key);
                    struct value_t *val = eval(p->value);
                    if (!key || !val) {
                        freeval(key);
                        freeval(val);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    bool replaced = false;
                    for (size_t i = 0; i < v->payload->items.size(); ++i) {
                        if (value_equals(v->payload->items[i].first, key)) {
                            freeval(v->payload->items[i].second);
                            v->payload->items[i].second = val;
                            freeval(key);
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced)
                        v->payload->items.push_back(std::make_pair(key, val));
                }
                p = (struct dict_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'M': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type == T_int) {
                long long n = ((struct int_value_t *)v)->value;
                ret = new_int_checked(-(__int128)n);
            } else if (v->type == T_float) {
                ret = newfloatval(-((struct float_value_t *)v)->value);
            } else {
                yyerror("cannot negate %s", typename_of(v));
            }
            freeval(v);
            break;
        }

        case '+': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x + y);
            } else if (is_numeric(l) && is_numeric(r)) {
                ret = newfloatval(as_double(l) + as_double(r));
            } else if (l->type == T_string && r->type == T_string) {
                std::string s = ((struct string_value_t *)l)->value;
                s += ((struct string_value_t *)r)->value;
                ret = newstrval(s.c_str());
            } else if (l->type == T_list && r->type == T_list) {
                struct list_value_t *lv = newlistval();
                struct list_value_t *ll = (struct list_value_t *)l;
                struct list_value_t *rl = (struct list_value_t *)r;
                lv->payload->items.reserve(ll->payload->items.size() + rl->payload->items.size());
                for (size_t i = 0; i < ll->payload->items.size(); ++i)
                    lv->payload->items.push_back(dupval(ll->payload->items[i]));
                for (size_t i = 0; i < rl->payload->items.size(); ++i)
                    lv->payload->items.push_back(dupval(rl->payload->items[i]));
                ret = (struct value_t *)lv;
            } else {
                yyerror("cannot add %s and %s", typename_of(l), typename_of(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '-':
        case '*': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot use %c on %s and %s", a->nodetype,
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked(a->nodetype == '-'
                                          ? (__int128)x - y
                                          : (__int128)x * y);
            } else {
                double x = as_double(l), y = as_double(r);
                ret = newfloatval(a->nodetype == '-' ? x - y : x * y);
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '/': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot divide %s by %s", typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (as_double(r) == 0.0) {
                yyerror("division by zero");
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x / y);
            } else {
                ret = newfloatval(as_double(l) / as_double(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '%': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot take %s modulo %s", typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (as_double(r) == 0.0) {
                yyerror("modulo by zero");
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x % y);
            } else {
                ret = newfloatval(std::fmod(as_double(l), as_double(r)));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case 'A': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (!left) {
                ret = newboolval(false);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case 'Q': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (left) {
                ret = newboolval(true);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case '!': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            ret = newboolval(!truthy(v));
            freeval(v);
            break;
        }

        case '~': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type != T_int) {
                yyerror("bitwise not expects an int, got %s", typename_of(v));
            } else {
                ret = newintval(~((struct int_value_t *)v)->value);
            }
            freeval(v);
            break;
        }

        case '&':
        case '|':
        case '^': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type != T_int || r->type != T_int) {
                yyerror("bitwise operator expects ints, got %s and %s",
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            if (a->nodetype == '&')
                ret = newintval(x & y);
            else if (a->nodetype == '|')
                ret = newintval(x | y);
            else
                ret = newintval(x ^ y);
            freeval(l);
            freeval(r);
            break;
        }

        case 's':
        case 'r': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type != T_int || r->type != T_int) {
                yyerror("shift operator expects ints, got %s and %s",
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            if (y < 0 || y >= 64) {
                yyerror("shift count %lld out of range", y);
            } else if (a->nodetype == 's') {
                ret = newintval((long long)((unsigned long long)x << y));
            } else {
                ret = newintval(x >> y);
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l && r)
                ret = do_compare(a->nodetype - '0', l, r);
            freeval(l);
            freeval(r);
            break;
        }

        case 'S': {
            struct switch_node *sw = (struct switch_node *)a;
            int errors_before = g_runtime_error_count;
            struct value_t *subject = eval(sw->expr);
            if (!subject || g_runtime_error_count != errors_before) {
                freeval(subject);
                break;
            }
            if (!switch_value_supported(subject)) {
                yyerror("switch does not support %s", typename_of(subject));
                freeval(subject);
                break;
            }

            auto run_body = [&](struct ast *body) {
                pushscope();
                int body_errors = g_runtime_error_count;
                struct value_t *body_ret = eval(body);
                if (g_runtime_error_count != body_errors) {
                    freeval(body_ret);
                } else if (body_ret) {
                    if (body_ret->type == T_break) {
                        freeval(body_ret);
                    } else if (body_ret->type == T_continue ||
                               body_ret->type == T_return) {
                        ret = body_ret;
                    } else {
                        freeval(body_ret);
                    }
                }
                popscope();
            };

            struct case_node *default_case = NULL;
            bool matched = false;
            bool stop = false;

            for (struct case_node *c = (struct case_node *)sw->cases;
                 c && !stop; c = (struct case_node *)c->next) {
                if (!c->values) {
                    default_case = c;
                    continue;
                }
                if (case_values_have_condition(c->values)) {
                    yyerror("logical or comparison expressions are not allowed "
                            "in case values");
                    stop = true;
                    break;
                }

                errors_before = g_runtime_error_count;
                struct value_t *values = eval(c->values);
                if (!values || g_runtime_error_count != errors_before) {
                    freeval(values);
                    stop = true;
                    break;
                }
                if (values->type != T_list) {
                    yyerror("switch case values must be a list");
                    freeval(values);
                    stop = true;
                    break;
                }

                struct list_value_t *list = (struct list_value_t *)values;
                for (size_t i = 0; i < list->payload->items.size(); ++i) {
                    if (!switch_value_supported(list->payload->items[i])) {
                        yyerror("switch does not support %s case value",
                                typename_of(list->payload->items[i]));
                        stop = true;
                        break;
                    }
                }
                if (!stop) {
                    for (size_t i = 0; i < list->payload->items.size(); ++i) {
                        if (value_equals(subject, list->payload->items[i])) {
                            matched = true;
                            break;
                        }
                    }
                }
                freeval(values);

                if (stop) break;
                if (matched) {
                    run_body(c->body);
                    stop = true;
                    break;
                }
            }

            if (!stop && !matched && default_case)
                run_body(default_case->body);

            freeval(subject);
            break;
        }

        case 'T': {
            struct flow *f = (struct flow *)a;
            int errors_before = g_runtime_error_count;
            struct value_t *cond = eval(f->cond);
            if (g_runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            ret = eval(b ? f->tl : f->el);
            break;
        }

        case 'I': {
            int errors_before = g_runtime_error_count;
            struct value_t *cond = eval(((struct flow *)a)->cond);
            if (g_runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            if (b) {
                if (((struct flow *)a)->tl)
                    ret = eval(((struct flow *)a)->tl);
            } else {
                if (((struct flow *)a)->el)
                    ret = eval(((struct flow *)a)->el);
            }
            break;
        }

        case 'E': {
            struct flow *f = (struct flow *)a;
            if (!f->cond || !f->tl) break;

            for (;;) {
                if (g_interrupted) break;

                int errors_before = g_runtime_error_count;
                struct value_t *body = eval(f->tl);
                if (g_runtime_error_count != errors_before) {
                    freeval(body);
                    break;
                }

                bool do_break = false;
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        break;
                    }
                    if (body->type == T_break) {
                        freeval(body);
                        do_break = true;
                    } else if (body->type == T_continue) {
                        freeval(body);
                    } else {
                        freeval(body);
                    }
                }
                if (do_break) break;

                errors_before = g_runtime_error_count;
                struct value_t *cond = eval(f->cond);
                if (g_runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;
            }
            break;
        }

        case 'W': {
            struct ast *cond_ast = ((struct flow *)a)->cond;
            struct ast *body = ((struct flow *)a)->tl;
            if (!body) break;
            for (;;) {
                if (g_interrupted) break;
                int errors_before = g_runtime_error_count;
                struct value_t *cond = eval(cond_ast);
                if (g_runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;

                errors_before = g_runtime_error_count;
                ret = eval(body);
                if (g_runtime_error_count != errors_before) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_return)
                    break;
                if (ret && ret->type == T_break) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_continue) {
                    freeval(ret);
                    ret = NULL;
                    continue;
                }
                freeval(ret);
                ret = NULL;
            }
            break;
        }

        case 'F': {
            struct forinloop *f = (struct forinloop *)a;
            int errors_before = g_runtime_error_count;
            struct value_t *iter = eval(f->iterable);
            if (!iter || g_runtime_error_count != errors_before) {
                freeval(iter);
                break;
            }

            size_t count = 0;
            if (iter->type == T_list)
                count = ((struct list_value_t *)iter)->payload->items.size();
            else if (iter->type == T_dict)
                count = ((struct dict_value_t *)iter)->payload->items.size();
            else if (iter->type == T_string)
                count = utf8_length(((struct string_value_t *)iter)->value);
            else if (iter->type == T_bytes)
                count = ((struct bytes_value_t *)iter)->value.size();
            else {
                yyerror("cannot iterate %s", typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name && iter->type != T_dict) {
                yyerror("two-variable for-in requires dict, got %s",
                        typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name &&
                std::strcmp(f->name, f->value_name) == 0) {
                yyerror("loop variable repeated: %s", f->name);
                freeval(iter);
                break;
            }

            pushscope();
            struct symbol *sym = newsym(f->name);
            sym->val = NULL;
            savesym(sym);
            struct symbol *value_sym = NULL;
            if (f->value_name) {
                value_sym = newsym(f->value_name);
                value_sym->val = NULL;
                savesym(value_sym);
            }

            bool leave = false;
            for (size_t i = 0; i < count && !leave; ++i) {
                if (g_interrupted)
                    break;

                struct value_t *item = NULL;
                struct value_t *second = NULL;
                if (iter->type == T_list) {
                    item = dupval(
                        ((struct list_value_t *)iter)->payload->items[i]);
                } else if (iter->type == T_dict) {
                    struct dict_value_t *dict = (struct dict_value_t *)iter;
                    item = dupval(dict->payload->items[i].first);
                    if (f->value_name)
                        second = dupval(dict->payload->items[i].second);
                } else if (iter->type == T_string) {
                    std::string ch;
                    if (!utf8_char_at(((struct string_value_t *)iter)->value, i,
                                      ch)) {
                        yyerror("invalid UTF-8 string iteration");
                        leave = true;
                        break;
                    }
                    item = newstrval(ch.c_str());
                } else {
                    item = newintval((unsigned char)
                        ((struct bytes_value_t *)iter)->value[i]);
                }

                freeval(sym->val);
                sym->val = item;
                if (value_sym) {
                    freeval(value_sym->val);
                    value_sym->val = second;
                }

                errors_before = g_runtime_error_count;
                struct value_t *body = eval(f->body);
                if (g_runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                        break;
                    }
                    if (body->type == T_break) {
                        freeval(body);
                        leave = true;
                        break;
                    }
                    if (body->type == T_continue) {
                        freeval(body);
                        continue;
                    }
                    freeval(body);
                }
            }

            popscope();
            freeval(iter);
            break;
        }

        case 'P': {
            struct forloop *f = (struct forloop *)a;
            pushscope();
            bool leave = false;

            if (f->init) {
                int errors_before = g_runtime_error_count;
                struct value_t *v = eval(f->init);
                if (g_runtime_error_count != errors_before) {
                    freeval(v);
                    leave = true;
                } else if (v) {
                    if (v->type == T_return) {
                        ret = v;
                        leave = true;
                    } else {
                        if (v->type == T_break || v->type == T_continue) {
                            diag_set_location(g_flow_line, g_flow_column);
                            yyerror("break/continue outside loop");
                            leave = true;
                        }
                        freeval(v);
                    }
                }
            }

            for (; !leave;) {
                if (g_interrupted) break;

                bool cond_true = true;
                if (f->cond) {
                    int errors_before = g_runtime_error_count;
                    struct value_t *cond = eval(f->cond);
                    if (g_runtime_error_count != errors_before) {
                        freeval(cond);
                        leave = true;
                        break;
                    }
                    if (!cond) break;
                    cond_true = truthy(cond);
                    freeval(cond);
                }
                if (!cond_true) break;

                bool do_break = false;
                int errors_before = g_runtime_error_count;
                struct value_t *body = eval(f->body);
                if (g_runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                    } else if (body->type == T_break) {
                        freeval(body);
                        do_break = true;
                    } else {
                        /* normal value and T_continue both run the step */
                        freeval(body);
                    }
                }
                if (leave || do_break) break;

                if (f->step) {
                    errors_before = g_runtime_error_count;
                    struct value_t *step = eval(f->step);
                    if (g_runtime_error_count != errors_before) {
                        freeval(step);
                        leave = true;
                        break;
                    }
                    if (step) {
                        if (step->type == T_return) {
                            ret = step;
                            leave = true;
                        } else {
                            if (step->type == T_break || step->type == T_continue) {
                                diag_set_location(g_flow_line, g_flow_column);
                                yyerror("break/continue outside loop");
                                leave = true;
                            }
                            freeval(step);
                        }
                    }
                }
            }

            popscope();
            break;
        }

        case 'L': {
            int errors_before = g_runtime_error_count;
            ret = eval(a->l);
            if (g_runtime_error_count != errors_before) {
                freeval(ret);
                ret = NULL;
                break;
            }
            if (ret && (ret->type == T_return ||
                        ret->type == T_break ||
                        ret->type == T_continue))
                break;
            freeval(ret);
            ret = eval(a->r);
            break;
        }

        case 'G': {
            struct value_t *fast = NULL;
            if (read_index_chain(a, fast)) {
                ret = fast;
                break;
            }
            struct value_t *container = eval(a->l);
            struct value_t *index = eval(a->r);
            if (!container || !index) {
                freeval(container);
                freeval(index);
                break;
            }
            ret = read_member_value(container, index);
            freeval(container);
            freeval(index);
            break;
        }

        case 'C': {
            struct ufncall *call = (struct ufncall *)a;
            if (call->callee && call->callee->nodetype == 'G') {
                struct value_t *receiver = eval(call->callee->l);
                struct value_t *key = eval(call->callee->r);
                if (!receiver || !key) {
                    freeval(receiver);
                    freeval(key);
                    break;
                }
                if (receiver->type == T_struct) {
                    if (key->type != T_string) {
                        yyerror("struct method name must be a string, got %s",
                                typename_of(key));
                    } else {
                        struct struct_value_t *sv =
                            (struct struct_value_t *)receiver;
                        const char *name =
                            ((struct string_value_t *)key)->value;
                        int field =
                            sv->def ? structtype_field_index(sv->def, name)
                                    : -1;
                        if (field >= 0 && sv->payload &&
                            (size_t)field < sv->payload->fields.size()) {
                            struct value_t *member =
                                dupval(sv->payload->fields[(size_t)field]);
                            freeval(receiver);
                            freeval(key);
                            if (member) {
                                ret = callvalue(member, call->args);
                                freeval(member);
                            }
                            break;
                        }
                        bool called = false;
                        if (sv->def) {
                            auto method = sv->def->methods.find(name);
                            if (method != sv->def->methods.end()) {
                                ret = call_method_value(receiver,
                                                        method->second,
                                                        call->args);
                                called = true;
                            }
                        }
                        if (!called) {
                            yyerror("method %s not found in %s", name,
                                    sv->def ? sv->def->name.c_str()
                                            : "struct");
                        }
                    }
                    freeval(receiver);
                    freeval(key);
                    break;
                }

                struct value_t *member = read_member_value(receiver, key);
                freeval(receiver);
                freeval(key);
                if (member) {
                    ret = callvalue(member, call->args);
                    freeval(member);
                }
                break;
            }

            struct value_t *callee = eval(call->callee);
            if (callee) {
                ret = callvalue(callee, call->args);
                freeval(callee);
            }
            break;
        }

        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    return ret;
}
