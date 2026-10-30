#ifndef _SCOPESTACK_H_
#define _SCOPESTACK_H_

#include <memory>

#include "valuetype.h"

struct symbol;

struct scope_t {
    std::shared_ptr<scope_t> parent;
    bool is_function;
    value_t *ret;
    struct symlist *syms;

    ~scope_t();
};

struct scope_t *topscope();
std::shared_ptr<scope_t> current_scope();
void pushscope();
void pushscope_closure(const std::shared_ptr<struct scope_t> &);
void initscope();
void popscope();
void donescope();
struct symbol *lookupsym(char *);
bool isrepeated(char *);
void savesym(struct symbol *);
bool retval(struct value_t *);

#endif
