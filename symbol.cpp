#include "symbol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    s->exported = false;
    s->is_const = false;
    return s;
}

struct symlist *newsymlist(struct symbol *sym, struct type_constraint *type,
                           struct symlist *next) {
    struct symlist *sl = (struct symlist *)std::malloc(sizeof(struct symlist));
    if (!sl) {
        yyerror("out of space");
        std::exit(0);
    }
    sl->sym = sym;
    sl->type = type;
    sl->next = next;
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
    return type;
}

void free_type_constraint(struct type_constraint *type) {
    std::free(type);
}

bool type_constraint_matches(const struct type_constraint *type,
                             const struct value_t *value) {
    if (!type || !value)
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
        case TYPE_BYTES:
            return value->type == T_bytes;
        case TYPE_LIST:
            return value->type == T_list;
        case TYPE_DICT:
            return value->type == T_dict;
        case TYPE_FUNCTION:
            return value->type == T_func;
        case TYPE_BUILTIN:
            return value->type == T_builtin;
        case TYPE_CALLABLE:
            return value->type == T_func || value->type == T_builtin;
        case TYPE_NULL:
            return value->type == T_null;
        case TYPE_USERDATA:
            return value->type == T_userdata;
        case TYPE_INVALID:
        default:
            return false;
    }
}

const char *type_constraint_name(const struct type_constraint *type) {
    if (!type)
        return "any";
    switch (type->kind) {
        case TYPE_ANY: return "any";
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_NUMBER: return "number";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_BYTES: return "bytes";
        case TYPE_LIST: return "list";
        case TYPE_DICT: return "dict";
        case TYPE_FUNCTION: return "function";
        case TYPE_BUILTIN: return "builtin";
        case TYPE_CALLABLE: return "callable";
        case TYPE_NULL: return "null";
        case TYPE_USERDATA: return "userdata";
        case TYPE_INVALID:
        default: return "invalid";
    }
}
