#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_set>
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

static void prune_scopes() {
    all_scopes.erase(
        std::remove_if(all_scopes.begin(), all_scopes.end(),
                       [](const std::weak_ptr<scope_t> &w) {
                           return w.expired();
                       }),
        all_scopes.end());
}

static void register_scope(const std::shared_ptr<scope_t> &s) {
    if (all_scopes.size() > 4096)
        prune_scopes();
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
    s->is_global = false;
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

void pushscope_isolated() {
    push_scope_impl(false, nullptr);
}

void pushscope_closure(const std::shared_ptr<scope_t> &parent) {
    push_scope_impl(true, parent ? parent : global_scope);
}

void initscope() {
    if (current) return;
    std::shared_ptr<scope_t> s = std::make_shared<scope_t>();
    s->parent.reset();
    s->is_function = false;
    s->is_global = true;
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

namespace {

/*
 * Cycle collection works at scope granularity.  Every cycle that can occur in
 * the current object model passes through a function closure, and therefore
 * through a scope.  Clearing symbol tables of scopes that are not reachable
 * from the active roots breaks those cycles.
 */
struct cycle_collector {
    std::unordered_set<scope_t *> live_scopes;
    std::unordered_set<value_t *> seen_values;
    std::unordered_set<list_payload *> seen_lists;
    std::unordered_set<dict_payload *> seen_dicts;
    std::vector<scope_t *> scope_work;
    std::vector<value_t *> value_work;

    void mark_scope(scope_t *s) {
        if (s)
            scope_work.push_back(s);
    }

    void mark_value(value_t *v) {
        if (v)
            value_work.push_back(v);
    }

    void drain() {
        for (;;) {
            while (!scope_work.empty()) {
                scope_t *s = scope_work.back();
                scope_work.pop_back();
                if (!live_scopes.insert(s).second)
                    continue;

                if (s->parent)
                    mark_scope(s->parent.get());
                for (struct symbol *sym : s->syms.small) {
                    if (sym)
                        mark_value(sym->val);
                }
                mark_value(s->ret);
            }

            if (value_work.empty())
                break;

            while (!value_work.empty()) {
                value_t *v = value_work.back();
                value_work.pop_back();
                if (!seen_values.insert(v).second)
                    continue;

                switch (v->type) {
                    case T_func: {
                        struct func_value_t *fv = (struct func_value_t *)v;
                        if (fv->def && fv->def->closure)
                            mark_scope(fv->def->closure.get());
                        break;
                    }
                    case T_method: {
                        struct method_value_t *method =
                            (struct method_value_t *)v;
                        if (method->method && method->method->closure)
                            mark_scope(method->method->closure.get());
                        if (method->receiver)
                            for (size_t i = 0;
                                 i < method->receiver->fields.size(); ++i)
                                mark_value(method->receiver->fields[i]);
                        break;
                    }
                    case T_list: {
                        struct list_value_t *lv = (struct list_value_t *)v;
                        if (lv->payload &&
                            seen_lists.insert(lv->payload.get()).second) {
                            for (value_t *item : lv->payload->items)
                                mark_value(item);
                        }
                        break;
                    }
                    case T_dict: {
                        struct dict_value_t *dv = (struct dict_value_t *)v;
                        if (dv->payload &&
                            seen_dicts.insert(dv->payload.get()).second) {
                            for (auto &entry : dv->payload->items) {
                                mark_value(entry.first);
                                mark_value(entry.second);
                            }
                        }
                        break;
                    }
                    case T_struct: {
                        struct struct_value_t *sv =
                            (struct struct_value_t *)v;
                        if (sv->payload)
                            for (size_t i = 0;
                                 i < sv->payload->fields.size(); ++i)
                                mark_value(sv->payload->fields[i]);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
};

void collect_cycles() {
    prune_scopes();

    std::vector<std::shared_ptr<scope_t>> scopes;
    scopes.reserve(all_scopes.size());
    for (auto &w : all_scopes) {
        if (std::shared_ptr<scope_t> s = w.lock())
            scopes.push_back(std::move(s));
    }

    cycle_collector collector;
    collector.mark_scope(global_scope.get());
    collector.mark_scope(current.get());
    for (auto &s : restore_stack)
        collector.mark_scope(s.get());
    collector.drain();

    for (auto &s : scopes) {
        if (collector.live_scopes.count(s.get()))
            continue;
        clear_scope(s.get());
        if (s->ret) {
            freeval(s->ret);
            s->ret = NULL;
        }
    }

    scopes.clear();
    prune_scopes();
}

} // namespace

void maybe_collect_cycles() {
    static int statements_since_collect = 0;
    static size_t scopes_at_last_collect = 0;
    static constexpr int COLLECT_INTERVAL = 64;
    static constexpr size_t SCOPE_GROWTH_THRESHOLD = 1024;

    ++statements_since_collect;
    prune_scopes();

    bool due = statements_since_collect >= COLLECT_INTERVAL;
    if (!due && all_scopes.size() >
                    scopes_at_last_collect + SCOPE_GROWTH_THRESHOLD)
        due = true;
    if (!due)
        return;

    statements_since_collect = 0;
    collect_cycles();
    prune_scopes();
    scopes_at_last_collect = all_scopes.size();
}

void save_global_symbol(const char *name, value_t *v) {
    if (!global_scope || !name)
        return;
    std::string_view key(name);
    struct symbol *existing = global_scope->syms.find(key);
    if (existing) {
        freeval(existing->val);
        existing->val = v;
        return;
    }
    struct symbol *sym = newsym(name);
    sym->val = v;
    global_scope->syms.insert(sym);
}

value_t *lookup_global_symbol(const char *name) {
    if (!global_scope || !name)
        return NULL;
    struct symbol *sym = global_scope->syms.find(std::string_view(name));
    return sym ? sym->val : NULL;
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
