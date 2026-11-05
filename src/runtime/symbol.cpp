#include "symbol.h"

#include "structtype.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

struct symbol *newsym(const char *name) {
    if (!name) name = "";
    size_t len = std::strlen(name);
    struct symbol *s =
        (struct symbol *)std::malloc(sizeof(struct symbol) + len + 1);
    if (!s) {
        yyerror("out of space");
        std::exit(0);
    }
    s->name = (char *)(s + 1);
    std::memcpy(s->name, name, len + 1);
    s->name_len = len;
    s->val = NULL;
    s->type.kind = TYPE_ANY;
    s->type.name = NULL;
    s->type.module = NULL;
    s->type.nullable = false;
    s->exported = false;
    s->is_const = false;
    s->is_uninitialized = false;
    s->global_slot = UINT32_MAX;
    return s;
}

static const char *intern_type_name(const char *name) {
    static std::unordered_set<std::string> names;
    if (!name) name = "";
    std::unordered_set<std::string>::iterator it = names.find(name);
    if (it == names.end()) it = names.insert(name).first;
    return it->c_str();
}

struct symlist *newsymlist(struct symbol *sym, struct type_constraint *type,
                           struct symlist *next, bool variadic) {
    struct symlist *sl = (struct symlist *)std::malloc(sizeof(struct symlist));
    if (!sl) {
        yyerror("out of space");
        std::exit(0);
    }
    sl->sym = sym;
    sl->type = type;
    sl->next = next;
    sl->variadic = variadic;
    sl->has_default = false;
    sl->has_pattern = false;
    return sl;
}

void symlistfree(struct symlist *sl) {
    while (sl) {
        struct symlist *next = sl->next;
        if (sl->sym) {
            std::free(sl->sym);
        }
        free_type_constraint(sl->type);
        std::free(sl);
        sl = next;
    }
}

struct type_constraint *new_type_constraint(enum type_constraint_kind kind) {
    struct type_constraint *type =
        (struct type_constraint *)std::malloc(sizeof(struct type_constraint));
    if (!type) {
        yyerror("out of space");
        std::exit(0);
    }
    type->kind = kind;
    type->name = NULL;
    type->module = NULL;
    type->nullable = false;
    return type;
}

struct type_constraint *new_named_type_constraint(const char *name,
                                                  const char *module) {
    struct type_constraint *type = new_type_constraint(TYPE_NAMED);
    type->name = intern_type_name(name);
    type->module = intern_type_name(module);
    return type;
}

namespace {

struct builtin_type_name {
    const char *name;
    const char *canonical;
    enum type_constraint_kind kind;
};

const builtin_type_name kBuiltinTypeNames[] = {
    {"any", "any", TYPE_ANY},
    {"int", "int", TYPE_INT},
    {"float", "float", TYPE_FLOAT},
    {"number", "number", TYPE_NUMBER},
    {"bool", "bool", TYPE_BOOL},
    {"string", "string", TYPE_STRING},
    {"str", "string", TYPE_STRING},
    {"char", "char", TYPE_CHAR},
    {"byte", "byte", TYPE_BYTE},
    {"bytes", "bytes", TYPE_BYTES},
    {"list", "list", TYPE_LIST},
    {"dict", "dict", TYPE_DICT},
    {"deque", "deque", TYPE_DEQUE},
    {"heap", "heap", TYPE_HEAP},
    {"set", "set", TYPE_SET},
    {"function", "function", TYPE_FUNCTION},
    {"func", "function", TYPE_FUNCTION},
    {"builtin", "builtin", TYPE_BUILTIN},
    {"callable", "callable", TYPE_CALLABLE},
    {"null", "null", TYPE_NULL},
    {"userdata", "userdata", TYPE_USERDATA},
    {"result", "result", TYPE_RESULT},
    {"error", "error", TYPE_ERROR},
};

const builtin_type_name *find_builtin_type_name(const char *name) {
    if (!name)
        return NULL;
    for (const builtin_type_name &entry : kBuiltinTypeNames) {
        if (std::strcmp(name, entry.name) == 0)
            return &entry;
    }
    return NULL;
}

} // namespace

bool type_constraint_kind_for_builtin_name(const char *name,
                                           enum type_constraint_kind &kind) {
    const builtin_type_name *entry = find_builtin_type_name(name);
    if (!entry)
        return false;
    kind = entry->kind;
    return true;
}

const char *type_constraint_canonical_builtin_name(const char *name) {
    const builtin_type_name *entry = find_builtin_type_name(name);
    return entry ? entry->canonical : NULL;
}

void free_type_constraint(struct type_constraint *type) {
    std::free(type);
}

bool type_constraint_matches(const struct type_constraint *type,
                             const struct value_t *value) {
    if (!type || !value)
        return true;
    if (type->nullable && value->type == T_null)
        return true;

    switch (type->kind) {
        case TYPE_ANY:
            return true;
        case TYPE_INT:
            return value->type == T_int;
        case TYPE_FLOAT:
            return value->type == T_float;
        case TYPE_NUMBER:
            return value->type == T_int || value->type == T_float;
        case TYPE_BOOL:
            return value->type == T_bool;
        case TYPE_STRING:
            return value->type == T_string;
        case TYPE_CHAR:
            return value->type == T_char;
        case TYPE_BYTE:
            return value->type == T_byte;
        case TYPE_BYTES:
            return value->type == T_bytes;
        case TYPE_LIST:
            return value->type == T_list;
        case TYPE_DICT:
            return value->type == T_dict;
        case TYPE_DEQUE:
            return value->type == T_deque;
        case TYPE_HEAP:
            return value->type == T_heap;
        case TYPE_SET:
            return value->type == T_set;
        case TYPE_FUNCTION:
            return value->type == T_func;
        case TYPE_BUILTIN:
            return value->type == T_builtin;
        case TYPE_CALLABLE:
            return value->type == T_func || value->type == T_builtin ||
                   value->type == T_method || value->type == T_native;
        case TYPE_NULL:
            return value->type == T_null;
        case TYPE_USERDATA:
            return value->type == T_userdata;
        case TYPE_RESULT:
            return value->type == T_result;
        case TYPE_ERROR:
            return value->type == T_error;
        case TYPE_NAMED:
            return named_type_matches(type->module, type->name, value);
        case TYPE_INVALID:
        default:
            return false;
    }
}

bool type_constraint_equal(const struct type_constraint *a,
                           const struct type_constraint *b) {
    if (!a || !b)
        return !a && !b;
    if (a->kind != b->kind)
        return false;
    if (a->nullable != b->nullable)
        return false;
    if (a->kind == TYPE_NAMED) {
        if (!a->name || !b->name)
            return a->name == b->name;
        return structtype_named_types_equal(a->module, a->name, b->module,
                                            b->name);
    }
    return true;
}

const char *type_constraint_name(const struct type_constraint *type) {
    if (!type)
        return "any";
    const char *base = "invalid";
    switch (type->kind) {
        case TYPE_ANY: base = "any"; break;
        case TYPE_INT: base = "int"; break;
        case TYPE_FLOAT: base = "float"; break;
        case TYPE_NUMBER: base = "number"; break;
        case TYPE_BOOL: base = "bool"; break;
        case TYPE_STRING: base = "string"; break;
        case TYPE_CHAR: base = "char"; break;
        case TYPE_BYTE: base = "byte"; break;
        case TYPE_BYTES: base = "bytes"; break;
        case TYPE_LIST: base = "list"; break;
        case TYPE_DICT: base = "dict"; break;
        case TYPE_DEQUE: base = "deque"; break;
        case TYPE_HEAP: base = "heap"; break;
        case TYPE_SET: base = "set"; break;
        case TYPE_FUNCTION: base = "function"; break;
        case TYPE_BUILTIN: base = "builtin"; break;
        case TYPE_CALLABLE: base = "callable"; break;
        case TYPE_NULL: base = "null"; break;
        case TYPE_USERDATA: base = "userdata"; break;
        case TYPE_RESULT: base = "result"; break;
        case TYPE_ERROR: base = "error"; break;
        case TYPE_NAMED:
            base = type->name ? type->name : "invalid";
            break;
        case TYPE_INVALID:
        default:
            base = "invalid";
            break;
    }
    if (!type->nullable)
        return base;
    static std::string nullable_name;
    nullable_name = std::string(base) + "?";
    return nullable_name.c_str();
}
