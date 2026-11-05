#ifndef _SCOPESTACK_H_
#define _SCOPESTACK_H_

#include <cstddef>
#include <cstdint>
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

struct scope_context {
    std::shared_ptr<scope_t> current;
    std::shared_ptr<scope_t> global_scope;
    std::vector<std::shared_ptr<scope_t>> restore_stack;
    std::vector<std::weak_ptr<scope_t>> all_scopes;
    /* Stable index table for global symbols, used by LOAD_GLOBAL et al. */
    std::vector<struct symbol *> global_slots;
    int statements_since_collect = 0;
    size_t scopes_at_last_collect = 0;
    uint64_t global_generation = 0;
};

scope_context &active_scope_context();
void set_active_scope_context(scope_context *context);
uint64_t scope_global_generation();

struct scope_t *topscope();
std::shared_ptr<scope_t> current_scope();
void pushscope();
void pushscope_isolated();
void pushscope_closure(const std::shared_ptr<struct scope_t> &);
void initscope();
void initscope(scope_context &context);
void popscope();
void donescope();
void donescope(scope_context &context);
void adopt_scopes(scope_context &from, scope_context &to);
bool scope_teardown_active();
void clear_scope(struct scope_t *s);
void prune_scopes(scope_context &context);
void save_global_symbol(const char *name, value_t *v);
value_t *lookup_global_symbol(const char *name);
/* Stable global slot for `name`, or -1 if it is not a current global. */
int global_symbol_slot(const char *name);
struct symbol *lookupsym(const char *);
bool isrepeated(const char *);
void savesym(struct symbol *);
bool retval(struct value_t *);

#endif
