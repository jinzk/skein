#include "scopestack.h"
#include "symbol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

static std::shared_ptr<scope_t> current;
static std::shared_ptr<scope_t> global_scope;
static std::vector<std::shared_ptr<scope_t>> restore_stack;
static std::vector<std::weak_ptr<scope_t>> all_scopes;

static void clear_scope(scope_t *s) {
    if (!s) return;
    struct symlist *list = s->syms;
    s->syms = NULL;
    while (list) {
        struct symlist *next = list->next;
        struct symbol *sym = list->sym;
        if (sym) {
            freeval(sym->val);
            std::free(sym->name);
            std::free(sym);
        }
        std::free(list);
        list = next;
    }
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
    s->syms = NULL;
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
    s->syms = NULL;
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

struct symbol *lookupsym(char *sym) {
    for (std::shared_ptr<scope_t> s = current; s; s = s->parent) {
        for (struct symlist *sl = s->syms; sl; sl = sl->next) {
            if (sl->sym->name && !std::strcmp(sl->sym->name, sym))
                return sl->sym;
        }
    }
    return NULL;
}

bool isrepeated(char *sym) {
    if (!current) return false;
    for (struct symlist *sl = current->syms; sl; sl = sl->next) {
        if (sl->sym->name && !std::strcmp(sl->sym->name, sym))
            return true;
    }
    return false;
}

void savesym(struct symbol *sp) {
    if (!current) return;
    struct symlist *sl = (struct symlist *)std::malloc(sizeof(struct symlist));
    if (!sl) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    sl->sym = sp;
    sl->next = current->syms;
    current->syms = sl;
}

bool retval(struct value_t *r) {
    for (std::shared_ptr<scope_t> s = current; s; s = s->parent) {
        if (s->is_function) {
            s->ret = r;
            return true;
        }
    }
    return false;
}
