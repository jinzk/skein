#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

static scope_context fallback_context;
static scope_context *active_context = NULL;
static bool tearing_down = false;

bool scope_teardown_active() {
    return tearing_down;
}

scope_context &active_scope_context() {
    return active_context ? *active_context : fallback_context;
}

void set_active_scope_context(scope_context *context) {
    active_context = context;
}

uint64_t scope_global_generation() {
    return active_scope_context().global_generation;
}

struct symbol *symbol_table::find(std::string_view name) const {
    if (indexed) {
        auto it = index.find(name);
        return it == index.end() ? NULL : it->second;
    }
    for (struct symbol *sym : small) {
        if (sym->name_len == name.size() &&
            std::memcmp(sym->name, name.data(), name.size()) == 0)
            return sym;
    }
    return NULL;
}

void symbol_table::insert(struct symbol *sym) {
    std::string_view key(sym->name, sym->name_len);
    small.push_back(sym);

    if (indexed) {
        index.emplace(key, sym);
    } else if (small.size() > linear_limit) {
        index.reserve(small.size() * 2);
        for (struct symbol *s : small)
            index.emplace(std::string_view(s->name, s->name_len), s);
        indexed = true;
    }
}

void symbol_table::clear() {
    index.clear();
    small.clear();
}

void clear_scope(scope_t *s) {
    if (!s) return;
    s->syms.index.clear();
    for (struct symbol *sym : s->syms.small) {
        if (sym) {
            freeval(sym->val);
            std::free(sym);
        }
    }
    s->syms.clear();
}

scope_t::~scope_t() {
    clear_scope(this);
    if (ret) {
        freeval(ret);
        ret = NULL;
    }
}

void prune_scopes(scope_context &context) {
    context.all_scopes.erase(
        std::remove_if(context.all_scopes.begin(), context.all_scopes.end(),
                       [](const std::weak_ptr<scope_t> &w) {
                           return w.expired();
                       }),
        context.all_scopes.end());
}

static void register_scope(scope_context &context,
                           const std::shared_ptr<scope_t> &s) {
    if (context.all_scopes.size() > 4096)
        prune_scopes(context);
    context.all_scopes.push_back(s);
}

struct scope_t *topscope() {
    return active_scope_context().current.get();
}

std::shared_ptr<scope_t> current_scope() {
    return active_scope_context().current;
}

static void push_scope_impl(bool is_function,
                            const std::shared_ptr<scope_t> &parent) {
    scope_context &context = active_scope_context();
    std::shared_ptr<scope_t> s = std::make_shared<scope_t>();
    s->parent = parent;
    s->is_function = is_function;
    s->is_global = false;
    s->ret = NULL;
    s->function_scope =
        is_function ? s.get() : (parent ? parent->function_scope : NULL);
    register_scope(context, s);
    context.restore_stack.push_back(context.current);
    context.current = s;
}

void pushscope() {
    push_scope_impl(false, active_scope_context().current);
}

void pushscope_isolated() {
    push_scope_impl(false, nullptr);
}

void pushscope_closure(const std::shared_ptr<scope_t> &parent) {
    scope_context &context = active_scope_context();
    push_scope_impl(true, parent ? parent : context.global_scope);
}

void initscope() {
    initscope(active_scope_context());
}

void initscope(scope_context &context) {
    if (context.current) return;
    std::shared_ptr<scope_t> s = std::make_shared<scope_t>();
    s->parent.reset();
    s->is_function = false;
    s->is_global = true;
    s->ret = NULL;
    s->function_scope = NULL;
    register_scope(context, s);
    context.global_scope = s;
    context.current = s;
    context.restore_stack.clear();
    context.global_slots.clear();
    ++context.global_generation;
}

void popscope() {
    scope_context &context = active_scope_context();
    if (!context.current || context.restore_stack.empty()) return;
    context.current = context.restore_stack.back();
    context.restore_stack.pop_back();
}

void donescope() {
    donescope(active_scope_context());
}

void adopt_scopes(scope_context &from, scope_context &to) {
    if (tearing_down)
        return;
    for (auto &scope : from.all_scopes)
        to.all_scopes.push_back(scope);
    from.all_scopes.clear();
    from.restore_stack.clear();
    from.global_slots.clear();
    from.current.reset();
    from.global_scope.reset();
}

void donescope(scope_context &context) {
    bool previous_tearing_down = tearing_down;
    tearing_down = true;
    context.restore_stack.clear();

    /*
     * Closures can create reference cycles between a scope and the function
     * values stored in it.  Clearing all symbol tables first breaks those
     * cycles, so the remaining shared_ptr references can be released.
     */
    for (auto &w : context.all_scopes) {
        if (std::shared_ptr<scope_t> s = w.lock())
            clear_scope(s.get());
    }

    context.current.reset();
    context.global_scope.reset();
    context.all_scopes.clear();
    context.global_slots.clear();
    context.statements_since_collect = 0;
    context.scopes_at_last_collect = 0;
    ++context.global_generation;
    tearing_down = previous_tearing_down;
}

void save_global_symbol(const char *name, value_t *v) {
    scope_context &context = active_scope_context();
    if (!context.global_scope || !name)
        return;
    std::string_view key(name);
    struct symbol *existing = context.global_scope->syms.find(key);
    if (existing) {
        freeval(existing->val);
        existing->val = v;
        return;
    }
    struct symbol *sym = newsym(name);
    sym->val = v;
    sym->scope_owned = true;
    context.global_scope->syms.insert(sym);
    if (sym->global_slot == UINT32_MAX) {
        sym->global_slot = static_cast<uint32_t>(context.global_slots.size());
        context.global_slots.push_back(sym);
    }
    ++context.global_generation;
}

int global_symbol_slot(const char *name) {
    scope_context &context = active_scope_context();
    if (!context.global_scope || !name)
        return -1;
    struct symbol *sym =
        context.global_scope->syms.find(std::string_view(name));
    if (!sym || sym->global_slot == UINT32_MAX)
        return -1;
    return static_cast<int>(sym->global_slot);
}

value_t *lookup_global_symbol(const char *name) {
    scope_context &context = active_scope_context();
    if (!context.global_scope || !name)
        return NULL;
    struct symbol *sym =
        context.global_scope->syms.find(std::string_view(name));
    return sym ? sym->val : NULL;
}

struct symbol *lookupsym(const char *sym) {
    if (!sym) return NULL;
    scope_context &context = active_scope_context();
    std::string_view key(sym);
    for (scope_t *s = context.current.get(); s; s = s->parent.get()) {
        if (struct symbol *found = s->syms.find(key))
            return found;
    }
    return NULL;
}

bool isrepeated(const char *sym) {
    scope_context &context = active_scope_context();
    if (!context.current || !sym) return false;
    return context.current->syms.find(std::string_view(sym)) != NULL;
}

void savesym(struct symbol *sp) {
    scope_context &context = active_scope_context();
    if (!context.current) return;
    context.current->syms.insert(sp);
    if (sp)
        sp->scope_owned = true;
    if (context.current->is_global) {
        if (sp && sp->global_slot == UINT32_MAX) {
            sp->global_slot =
                static_cast<uint32_t>(context.global_slots.size());
            context.global_slots.push_back(sp);
        }
        ++context.global_generation;
    }
}

bool retval(struct value_t *r) {
    scope_context &context = active_scope_context();
    if (context.current && context.current->function_scope) {
        context.current->function_scope->ret = r;
        return true;
    }
    return false;
}
