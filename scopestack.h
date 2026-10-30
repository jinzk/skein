#ifndef _SCOPESTACK_H_
#define _SCOPESTACK_H_

#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "valuetype.h"

struct symbol;

/*
 * Scopes are usually tiny, so a linear vector beats a hash table for the
 * common case.  Once a scope grows past the threshold we build an index and
 * switch to hash lookups.
 */
struct symbol_table {
    static constexpr std::size_t linear_limit = 8;

    std::vector<struct symbol *> small;
    std::unordered_map<std::string_view, struct symbol *> index;
    bool indexed = false;

    struct symbol *find(std::string_view name) const;
    void insert(struct symbol *sym);
    void clear();
};

struct scope_t {
    std::shared_ptr<scope_t> parent;
    bool is_function;
    bool is_global;
    value_t *ret;
    symbol_table syms;
    scope_t *function_scope;

    ~scope_t();
};

struct scope_t *topscope();
std::shared_ptr<scope_t> current_scope();
void pushscope();
void pushscope_isolated();
void pushscope_closure(const std::shared_ptr<struct scope_t> &);
void initscope();
void popscope();
void donescope();
void maybe_collect_cycles();
void save_global_symbol(const char *name, value_t *v);
value_t *lookup_global_symbol(const char *name);
struct symbol *lookupsym(const char *);
bool isrepeated(const char *);
void savesym(struct symbol *);
bool retval(struct value_t *);

#endif
