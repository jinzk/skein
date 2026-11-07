#ifndef _SYMBOL_H_
#define _SYMBOL_H_
#include "valuetype.h"
extern int yylineno;
void yyerror(const char *s, ...);

enum type_constraint_kind {
    TYPE_ANY,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_NUMBER,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_BYTE,
    TYPE_BYTES,
    TYPE_LIST,
    TYPE_DICT,
    TYPE_DEQUE,
    TYPE_HEAP,
    TYPE_SET,
    TYPE_FUNCTION,
    TYPE_BUILTIN,
    TYPE_CALLABLE,
    TYPE_NULL,
    TYPE_USERDATA,
    TYPE_RESULT,
    TYPE_ERROR,
    TYPE_NAMED,
    TYPE_INVALID,
    /* Static-only type variable used by builtin polymorphic signatures. */
    TYPE_VAR,
    /* Static-only wrapper for async task results and generator streams. */
    TYPE_TASK,
    TYPE_GENERATOR,
    /* Static-only opaque userdata categories. */
    TYPE_SOCKET,
    TYPE_LISTENER,
    TYPE_WEBSOCKET,
    TYPE_STREAM
};

struct type_constraint {
    enum type_constraint_kind kind;
    /* Interned; non-NULL only for TYPE_NAMED. */
    const char *name;
    const char *module;
    bool nullable;
    /*
     * Static-checker metadata.  Both pointers are interned and non-owning;
     * the runtime never interprets detail and only uses display for
     * diagnostics/meta output.  Plain annotations leave them NULL.
     */
    const char *display = NULL;
    const void *detail = NULL;
};

struct symbol {
    char *name;
    size_t name_len;
    struct value_t *val;
    struct type_constraint type;
    bool exported;
    bool is_const;
    /* True for a parameter bound without a value while evaluating defaults. */
    bool is_uninitialized;
    /* Stable index in scope_context::global_slots, or UINT32_MAX. */
    uint32_t global_slot = UINT32_MAX;
    /* True when a scope owns this symbol and will free it on pop. */
    bool scope_owned = false;
};

struct symlist {
    struct symbol *sym;
    struct type_constraint *type;
    struct symlist *next;
    bool variadic;
    /* True when this parameter has a default expression. */
    bool has_default;
    /* True when this parameter has a destructuring pattern. */
    bool has_pattern;
};

struct symbol *newsym(const char *name);
struct symlist *newsymlist(struct symbol *sym, struct type_constraint *type,
                           struct symlist *next, bool variadic = false);
void symlistfree(struct symlist *sl);

struct type_constraint *new_type_constraint(enum type_constraint_kind kind);
struct type_constraint *new_named_type_constraint(const char *name,
                                                  const char *module);
void type_constraint_assign(struct type_constraint *dst,
                            const struct type_constraint *src);
void type_constraint_set_any(struct type_constraint *dst);

/* Built-in type names accepted in annotations, including aliases such as
 * str -> string and func -> function. */
bool type_constraint_kind_for_builtin_name(const char *name,
                                           enum type_constraint_kind &kind);
const char *type_constraint_canonical_builtin_name(const char *name);
void free_type_constraint(struct type_constraint *type);
bool type_constraint_matches(const struct type_constraint *type,
                             const struct value_t *value);
bool type_constraint_equal(const struct type_constraint *a,
                           const struct type_constraint *b);
const char *type_constraint_name(const struct type_constraint *type);

#endif