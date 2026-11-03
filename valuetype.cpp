#include "valuetype.h"
#include "structtype.h"
#include "unicode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
thread_local int render_depth = 0;
struct render_depth_guard {
    render_depth_guard() { ++render_depth; }
    ~render_depth_guard() { --render_depth; }
};

thread_local int free_depth = 0;
struct free_depth_guard {
    free_depth_guard() { ++free_depth; }
    ~free_depth_guard() { --free_depth; }
};
} // namespace


static char *dupstr(const char *s) {
    if (!s) s = "";
    size_t len = std::strlen(s) + 1;
    char *p = (char *)std::malloc(len);
    if (!p) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    std::memcpy(p, s, len);
    return p;
}

struct value_t *newreturn() {
    struct value_t *v = new value_t;
    v->type = T_return;
    return v;
}

struct value_t *newbreak(const char *label) {
    struct control_value_t *v = new control_value_t;
    v->type = T_break;
    v->label = label ? dupstr(label) : NULL;
    return (struct value_t *)v;
}

struct value_t *newcontinue(const char *label) {
    struct control_value_t *v = new control_value_t;
    v->type = T_continue;
    v->label = label ? dupstr(label) : NULL;
    return (struct value_t *)v;
}

struct value_t *newintval(long long i) {
    struct int_value_t *v = new int_value_t;
    v->type = T_int;
    v->value = i;
    return (struct value_t *)v;
}

struct value_t *newfloatval(double f) {
    struct float_value_t *v = new float_value_t;
    v->type = T_float;
    v->value = f;
    return (struct value_t *)v;
}

struct value_t *newboolval(bool b) {
    struct bool_value_t *v = new bool_value_t;
    v->type = T_bool;
    v->value = b;
    return (struct value_t *)v;
}

struct value_t *newstrval(const char *s) {
    struct string_value_t *v = new string_value_t;
    v->type = T_string;
    v->value = dupstr(s);
    return (struct value_t *)v;
}

struct value_t *newcharval(unsigned int codepoint) {
    struct char_value_t *v = new char_value_t;
    v->type = T_char;
    v->value = codepoint;
    return (struct value_t *)v;
}

struct value_t *newbyteval(unsigned int value) {
    struct byte_value_t *v = new byte_value_t;
    v->type = T_byte;
    v->value = value & 0xFF;
    return (struct value_t *)v;
}

struct value_t *newbytesval(std::string data) {
    struct bytes_value_t *v = new bytes_value_t;
    v->type = T_bytes;
    v->value = std::move(data);
    return (struct value_t *)v;
}

struct value_t *newbuiltinval(int functype) {
    struct builtin_value_t *v = new builtin_value_t;
    v->type = T_builtin;
    v->functype = functype;
    return (struct value_t *)v;
}

struct value_t *newuserdataval(const struct native_type *meta,
                               std::shared_ptr<void> data) {
    struct userdata_value_t *v = new userdata_value_t;
    v->type = T_userdata;
    v->meta = meta;
    v->data = std::move(data);
    return (struct value_t *)v;
}

struct value_t *newerrorval(const std::string &kind,
                            const std::string &message) {
    struct error_value_t *v = new error_value_t;
    v->type = T_error;
    v->kind = kind;
    v->message = message;
    return (struct value_t *)v;
}

struct value_t *newresultok(struct value_t *value) {
    struct result_value_t *v = new result_value_t;
    v->type = T_result;
    v->ok = true;
    v->value = value;
    v->error = NULL;
    return (struct value_t *)v;
}

struct value_t *newresulterr(struct value_t *error) {
    struct result_value_t *v = new result_value_t;
    v->type = T_result;
    v->ok = false;
    v->value = NULL;
    v->error = error;
    return (struct value_t *)v;
}

struct value_t *newresulterr_message(const std::string &kind,
                                     const std::string &message) {
    return newresulterr(newerrorval(kind, message));
}

struct value_t *normalize_error(struct value_t *value,
                                const char *default_kind) {
    if (value && value->type == T_error)
        return value;
    std::string message = value ? value_to_string(value) : "null";
    freeval(value);
    return newerrorval(default_kind ? default_kind : "error", message);
}

const char *error_kind_of(const struct value_t *value) {
    if (!value || value->type != T_error)
        return "";
    return ((const struct error_value_t *)value)->kind.c_str();
}

std::string error_message_of(const struct value_t *value) {
    if (!value || value->type != T_error)
        return value ? value_to_string(value) : "";
    return ((const struct error_value_t *)value)->message;
}

struct value_t *newnull() {
    struct value_t *v = new value_t;
    v->type = T_null;
    return v;
}

struct value_t *newfuncval(std::shared_ptr<function_code> code,
                           std::shared_ptr<struct scope_t> closure) {
    struct func_value_t *v = new func_value_t;
    v->type = T_func;
    v->def = std::make_shared<function_def>();
    v->def->code = code;
    v->def->closure = closure;
    return (struct value_t *)v;
}

struct list_value_t *newlistval() {
    struct list_value_t *v = new list_value_t;
    v->type = T_list;
    v->payload = std::make_shared<list_payload>();
    return v;
}

struct dict_value_t *newdictval() {
    struct dict_value_t *v = new dict_value_t;
    v->type = T_dict;
    v->payload = std::make_shared<dict_payload>();
    return v;
}

struct lru_value_t *newlruval(std::size_t capacity) {
    struct lru_value_t *v = new lru_value_t;
    v->type = T_lru;
    v->payload = std::make_shared<lru_payload>();
    v->payload->capacity = capacity;
    return v;
}

list_payload::~list_payload() {
    for (size_t i = 0; i < items.size(); ++i)
        freeval(items[i]);
}

dict_payload::~dict_payload() {
    for (size_t i = 0; i < items.size(); ++i) {
        freeval(items[i].first);
        freeval(items[i].second);
    }
}

lru_payload::~lru_payload() {
    for (auto &entry : items) {
        freeval(entry.first);
        freeval(entry.second);
    }
}

struct value_t *dupval(struct value_t *v) {
    if (!v) return NULL;
    switch (v->type) {
        case T_int:
            return newintval(((struct int_value_t *)v)->value);
        case T_float:
            return newfloatval(((struct float_value_t *)v)->value);
        case T_bool:
            return newboolval(((struct bool_value_t *)v)->value);
        case T_string:
            return newstrval(((struct string_value_t *)v)->value);
        case T_char:
            return newcharval(((struct char_value_t *)v)->value);
        case T_byte:
            return newbyteval(((struct byte_value_t *)v)->value);
        case T_bytes:
            return newbytesval(((struct bytes_value_t *)v)->value);
        case T_list: {
            struct list_value_t *src = (struct list_value_t *)v;
            struct list_value_t *dst = new list_value_t;
            dst->type = T_list;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_dict: {
            struct dict_value_t *src = (struct dict_value_t *)v;
            struct dict_value_t *dst = new dict_value_t;
            dst->type = T_dict;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_func: {
            struct func_value_t *src = (struct func_value_t *)v;
            struct func_value_t *dst = new func_value_t;
            dst->type = T_func;
            dst->def = src->def;
            return (struct value_t *)dst;
        }
        case T_builtin:
            return newbuiltinval(((struct builtin_value_t *)v)->functype);
        case T_userdata: {
            struct userdata_value_t *src = (struct userdata_value_t *)v;
            return newuserdataval(src->meta, src->data);
        }
        case T_error: {
            struct error_value_t *src = (struct error_value_t *)v;
            return newerrorval(src->kind, src->message);
        }
        case T_result: {
            struct result_value_t *src = (struct result_value_t *)v;
            if (!src->ok)
                return newresulterr(dupval(src->error));
            return newresultok(dupval(src->value));
        }
        case T_struct: {
            struct struct_value_t *src = (struct struct_value_t *)v;
            struct struct_value_t *dst = new struct_value_t;
            dst->type = T_struct;
            dst->def = src->def;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_enum: {
            struct enum_value_t *src = (struct enum_value_t *)v;
            struct enum_value_t *dst = new enum_value_t;
            dst->type = T_enum;
            dst->def = src->def;
            dst->variant = src->variant;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_lru: {
            struct lru_value_t *src = (struct lru_value_t *)v;
            struct lru_value_t *dst = new lru_value_t;
            dst->type = T_lru;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_method: {
            struct method_value_t *src = (struct method_value_t *)v;
            struct method_value_t *dst = new method_value_t;
            dst->type = T_method;
            dst->def = src->def;
            dst->receiver = src->receiver;
            dst->method = src->method;
            return (struct value_t *)dst;
        }
        case T_null:
            return newnull();
        case T_return:
            return newreturn();
        case T_break:
            return newbreak(((struct control_value_t *)v)->label);
        case T_continue:
            return newcontinue(((struct control_value_t *)v)->label);
        default:
            return NULL;
    }
}

void freeval(struct value_t *v) {
    if (!v) return;
    if (free_depth > 1000)
        return; /* avoid C++ stack overflow on very deep values */
    free_depth_guard guard;
    switch (v->type) {
        case T_int:
            delete (struct int_value_t *)v;
            break;
        case T_float:
            delete (struct float_value_t *)v;
            break;
        case T_bool:
            delete (struct bool_value_t *)v;
            break;
        case T_string: {
            struct string_value_t *s = (struct string_value_t *)v;
            std::free(s->value);
            delete s;
            break;
        }
        case T_char:
            delete (struct char_value_t *)v;
            break;
        case T_byte:
            delete (struct byte_value_t *)v;
            break;
        case T_bytes:
            delete (struct bytes_value_t *)v;
            break;
        case T_list:
            delete (struct list_value_t *)v;
            break;
        case T_dict:
            delete (struct dict_value_t *)v;
            break;
        case T_func:
            delete (struct func_value_t *)v;
            break;
        case T_builtin:
            delete (struct builtin_value_t *)v;
            break;
        case T_userdata:
            delete (struct userdata_value_t *)v;
            break;
        case T_error:
            delete (struct error_value_t *)v;
            break;
        case T_result: {
            struct result_value_t *r = (struct result_value_t *)v;
            freeval(r->value);
            freeval(r->error);
            delete r;
            break;
        }
        case T_struct:
            delete (struct struct_value_t *)v;
            break;
        case T_enum:
            delete (struct enum_value_t *)v;
            break;
        case T_lru:
            delete (struct lru_value_t *)v;
            break;
        case T_method:
            delete (struct method_value_t *)v;
            break;
        case T_null:
            delete v;
            break;
        case T_return:
            delete v;
            break;
        case T_break:
        case T_continue: {
            struct control_value_t *c = (struct control_value_t *)v;
            std::free(c->label);
            delete c;
            break;
        }
        default:
            break;
    }
}

const char *typename_of(const struct value_t *v) {
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
        case T_userdata: {
            const struct userdata_value_t *u = (const struct userdata_value_t *)v;
            return u->meta ? u->meta->name : "userdata";
        }
        case T_result: return "result";
        case T_error: return "error";
        case T_struct: {
            const struct struct_value_t *s = (const struct struct_value_t *)v;
            return s->def ? s->def->name.c_str() : "struct";
        }
        case T_enum: {
            const struct enum_value_t *e = (const struct enum_value_t *)v;
            return e->def ? e->def->name.c_str() : "enum";
        }
        case T_lru: return "lru";
        case T_method: return "method";
        case T_null: return "null";
        case T_class: return "class";
        case T_return: return "return";
        case T_break: return "break";
        case T_continue: return "continue";
        default: return "unknown";
    }
}

static std::string format_double(double d) {
    if (std::isnan(d))
        return std::signbit(d) ? "-nan" : "nan";
    if (std::isinf(d))
        return d < 0 ? "-inf" : "inf";

    char buf[64];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, d);
        if (std::strtod(buf, NULL) == d)
            break;
    }

    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    return s;
}

std::string value_to_string(const struct value_t *v) {
    if (!v) return "null";
    if (render_depth > 100)
        return "...";
    render_depth_guard guard;
    switch (v->type) {
        case T_int:
            return std::to_string(((const struct int_value_t *)v)->value);
        case T_float:
            return format_double(((const struct float_value_t *)v)->value);
        case T_bool:
            return ((const struct bool_value_t *)v)->value ? "true" : "false";
        case T_string: {
            const char *s = ((const struct string_value_t *)v)->value;
            return s ? s : "";
        }
        case T_char: {
            std::string out;
            if (!utf8_encode(((const struct char_value_t *)v)->value, out))
                return "<char>";
            return out;
        }
        case T_byte:
            return std::to_string(((const struct byte_value_t *)v)->value);
        case T_bytes: {
            const struct bytes_value_t *b = (const struct bytes_value_t *)v;
            return "<bytes: " + std::to_string(b->value.size()) + ">";
        }
        case T_list: {
            const struct list_value_t *l = (const struct list_value_t *)v;
            std::string out = "[";
            for (size_t i = 0; i < l->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(l->payload->items[i]);
            }
            out += "]";
            return out;
        }
        case T_dict: {
            const struct dict_value_t *d = (const struct dict_value_t *)v;
            std::string out = "{";
            for (size_t i = 0; i < d->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(d->payload->items[i].first);
                out += ": ";
                out += value_to_string(d->payload->items[i].second);
            }
            out += "}";
            return out;
        }
        case T_func:
            return "<function>";
        case T_builtin:
            return "<builtin>";
        case T_userdata: {
            const struct userdata_value_t *u = (const struct userdata_value_t *)v;
            if (u->meta && u->meta->describe)
                return u->meta->describe(u->data.get());
            return "<userdata>";
        }
        case T_error:
            return ((const struct error_value_t *)v)->message;
        case T_result: {
            const struct result_value_t *r =
                (const struct result_value_t *)v;
            if (r->ok)
                return "ok(" + value_to_string(r->value) + ")";
            return "err(" + value_to_string(r->error) + ")";
        }
        case T_struct: {
            const struct struct_value_t *s = (const struct struct_value_t *)v;
            if (!s->def || !s->payload)
                return "<struct>";
            if (s->def->fields.empty())
                return s->def->name + " {}";
            std::string out = s->def->name + " { ";
            for (size_t i = 0; i < s->def->fields.size(); ++i) {
                if (i) out += ", ";
                out += s->def->fields[i].name;
                out += ": ";
                out += (i < s->payload->fields.size())
                           ? value_to_string(s->payload->fields[i])
                           : "null";
            }
            out += " }";
            return out;
        }
        case T_enum: {
            const struct enum_value_t *e = (const struct enum_value_t *)v;
            if (!e->def || !e->payload)
                return "<enum>";
            if (e->variant < 0 ||
                (size_t)e->variant >= e->def->variants.size())
                return "<enum>";
            const struct variant_def &variant =
                e->def->variants[(size_t)e->variant];
            std::string out = e->def->name + "::" + variant.name;
            if (variant.fields.empty())
                return out;
            out += " { ";
            for (size_t i = 0; i < variant.fields.size(); ++i) {
                if (i) out += ", ";
                out += variant.fields[i].name;
                out += ": ";
                out += (i < e->payload->fields.size())
                           ? value_to_string(e->payload->fields[i])
                           : "null";
            }
            out += " }";
            return out;
        }
        case T_lru: {
            const struct lru_value_t *l = (const struct lru_value_t *)v;
            std::string out = "lru(" +
                              std::to_string(l->payload->items.size()) +
                              "/" + std::to_string(l->payload->capacity) +
                              ", {";
            for (size_t i = 0; i < l->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(l->payload->items[i].first);
                out += ": ";
                out += value_to_string(l->payload->items[i].second);
            }
            out += "})";
            return out;
        }
        case T_method:
            return "<method>";
        case T_null:
            return "null";
        case T_class:
            return "<class>";
        case T_return:
            return "<return>";
        case T_break:
            return "<break>";
        case T_continue:
            return "<continue>";
        default:
            return "<unknown>";
    }
}

void printvalue(struct value_t *v) {
    std::string s = value_to_string(v);
    std::fputs(s.c_str(), stdout);
}
