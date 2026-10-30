#include "valuetype.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

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

struct value_t *newbreak() {
    struct value_t *v = new value_t;
    v->type = T_break;
    return v;
}

struct value_t *newcontinue() {
    struct value_t *v = new value_t;
    v->type = T_continue;
    return v;
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

struct value_t *newstrval(const char *s) {
    struct string_value_t *v = new string_value_t;
    v->type = T_string;
    v->value = dupstr(s);
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

struct value_t *dupval(struct value_t *v) {
    if (!v) return NULL;
    switch (v->type) {
        case T_int:
            return newintval(((struct int_value_t *)v)->value);
        case T_float:
            return newfloatval(((struct float_value_t *)v)->value);
        case T_string:
            return newstrval(((struct string_value_t *)v)->value);
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
        case T_null:
            return newnull();
        case T_return:
            return newreturn();
        case T_break:
            return newbreak();
        case T_continue:
            return newcontinue();
        default:
            return NULL;
    }
}

void freeval(struct value_t *v) {
    if (!v) return;
    switch (v->type) {
        case T_int:
            delete (struct int_value_t *)v;
            break;
        case T_float:
            delete (struct float_value_t *)v;
            break;
        case T_string: {
            struct string_value_t *s = (struct string_value_t *)v;
            std::free(s->value);
            delete s;
            break;
        }
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
        case T_null:
            delete v;
            break;
        case T_return:
        case T_break:
        case T_continue:
            delete v;
            break;
        default:
            break;
    }
}

const char *typename_of(const struct value_t *v) {
    if (!v) return "null";
    switch (v->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_string: return "string";
        case T_bytes: return "bytes";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_func: return "function";
        case T_builtin: return "builtin";
        case T_userdata: {
            const struct userdata_value_t *u = (const struct userdata_value_t *)v;
            return u->meta ? u->meta->name : "userdata";
        }
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
    switch (v->type) {
        case T_int:
            return std::to_string(((const struct int_value_t *)v)->value);
        case T_float:
            return format_double(((const struct float_value_t *)v)->value);
        case T_string: {
            const char *s = ((const struct string_value_t *)v)->value;
            return s ? s : "";
        }
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
