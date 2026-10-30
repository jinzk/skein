#include "scopestack.h"
#include "symbol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

static std::shared_ptr<scope_t> current;
static std::shared_ptr<scope_t> global_scope;
static std::vector<std::shared_ptr<scope_t>> restore_stack;
static std::vector<std::weak_ptr<scope_t>> all_scopes;

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

static void clear_scope(scope_t *s) {
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

static void register_scope(const std::shared_ptr<scope_t> &s) {
    if (all_scopes.size() > 4096) {
        all_scopes.erase(
            std::remove_if(all_scopes.begin(), all_scopes.end(),
                           [](const std::weak_ptr<scope_t> &w) {
                               return w.expired();
                           }),
            all_scopes.end());
    }
    all_scopes.push_back(s);
}

struct scope_t *topscope() {
    return current.get();
}

std::shared_ptr<scope_t> current_scope() {
    return current;
}

static void push_scope_impl(bool is_function,
                            const std::shared_ptr<scope_t> &parent) {
    std::shared_ptr<scope_t> s = std::make_shared<scope_t>();
    s->parent = parent;
    s->is_function = is_function;
    s->ret = NULL;
    s->function_scope =
        is_function ? s.get() : (parent ? parent->function_scope : NULL);
    register_scope(s);
    restore_stack.push_back(current);
    current = s;
}

void pushscope() {
    push_scope_impl(false, current);
}

void pushscope_closure(const std::shared_ptr<scope_t> &parent) {
    push_scope_impl(true, parent ? parent : global_scope);
}

void initscope() {
    if (current) return;
    std::shared_ptr<scope_t> s = std::make_shared<scope_t>();
    s->parent.reset();
    s->is_function = false;
    s->ret = NULL;
    s->function_scope = NULL;
    register_scope(s);
    global_scope = s;
    current = s;
    restore_stack.clear();
}

void popscope() {
    if (!current || restore_stack.empty()) return;
    current = restore_stack.back();
    restore_stack.pop_back();
}

void donescope() {
    restore_stack.clear();

    /*
     * Closures can create reference cycles between a scope and the function
     * values stored in it.  Clearing all symbol tables first breaks those
     * cycles, so the remaining shared_ptr references can be released.
     */
    for (auto &w : all_scopes) {
        if (std::shared_ptr<scope_t> s = w.lock())
            clear_scope(s.get());
    }

    current.reset();
    global_scope.reset();
    all_scopes.clear();
}

struct symbol *lookupsym(const char *sym) {
    if (!sym) return NULL;
    std::string_view key(sym);
    for (scope_t *s = current.get(); s; s = s->parent.get()) {
        if (struct symbol *found = s->syms.find(key))
            return found;
    }
    return NULL;
}

bool isrepeated(const char *sym) {
    if (!current || !sym) return false;
    return current->syms.find(std::string_view(sym)) != NULL;
}

void savesym(struct symbol *sp) {
    if (!current) return;
    current->syms.insert(sp);
}

bool retval(struct value_t *r) {
    if (current && current->function_scope) {
        current->function_scope->ret = r;
        return true;
    }
    return false;
}
