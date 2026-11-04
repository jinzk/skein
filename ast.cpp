#include "ast.h"
#include "diagnostics.h"
#include "package.h"
#include "scopestack.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

static char *copy_string(const char *text) {
    if (!text) text = "";
    size_t len = std::strlen(text) + 1;
    char *copy = (char *)std::malloc(len);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text, len);
    return copy;
}

template <typename T>
static void set_loc(T *node, int line, int column) {
    node->line = line > 0 ? line : g_token_line;
    node->column = column > 0 ? column : g_token_column;
}

template <typename T>
static void set_loc_from(T *node, const struct ast *anchor) {
    set_loc(node, anchor ? anchor->line : 0,
            anchor ? anchor->column : 0);
}

static std::vector<std::pair<int, int>> func_locations;

void push_func_location(int line, int column) {
    func_locations.push_back(std::make_pair(line, column));
}

void clear_func_locations() {
    func_locations.clear();
}

struct ast *newast(int nodetype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)std::malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    set_loc_from(a, l);
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newint(long long i) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newintval(i);
    return (struct ast *)a;
}

struct ast *newfloat(double d) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newfloatval(d);
    return (struct ast *)a;
}

struct ast *newbool(bool b) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newboolval(b);
    return (struct ast *)a;
}

struct ast *newstr(const char *s) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newstrval(s);
    return (struct ast *)a;
}

struct ast *newchar(long long c) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newcharval((unsigned int)c);
    return (struct ast *)a;
}

struct ast *newbuiltin(int functype) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newbuiltinval(functype);
    return (struct ast *)a;
}

struct ast *newnullast() {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newnull();
    return (struct ast *)a;
}

struct ast *newcmp(int cmptype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)std::malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = '0' + cmptype;
    set_loc_from(a, l);
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newcall(struct ast *callee, struct ast *l) {
    struct ufncall *a = (struct ufncall *)std::malloc(sizeof(struct ufncall));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'C';
    set_loc_from(a, callee);
    a->callee = callee;
    a->args = l;
    return (struct ast *)a;
}

struct ast *newref(char *s) {
    struct symref *a = (struct symref *)std::malloc(sizeof(struct symref));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'N';
    set_loc(a, 0, 0);
    a->name = s;
    return (struct ast *)a;
}

struct ast *newasgn(struct ast *target, struct ast *v) {
    struct symasgn *a = (struct symasgn *)std::malloc(sizeof(struct symasgn));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = '=';
    set_loc_from(a, target);
    a->target = target;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newcompound(struct ast *target, int op, struct ast *v) {
    struct compound_asgn *a =
        (struct compound_asgn *)std::malloc(sizeof(struct compound_asgn));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'a';
    set_loc_from(a, target);
    a->target = target;
    a->op = op;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newincdec(struct ast *target, int delta, bool prefix) {
    struct incdec_node *a =
        (struct incdec_node *)std::malloc(sizeof(struct incdec_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'U';
    set_loc_from(a, target);
    a->target = target;
    a->delta = delta;
    a->prefix = prefix;
    return (struct ast *)a;
}

struct ast *newscope(struct ast *v) {
    struct nscope *a = (struct nscope *)std::malloc(sizeof(struct nscope));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'X';
    set_loc_from(a, v);
    a->ast = v;
    return (struct ast *)a;
}

struct ast *newvar(char *s, struct ast *v, bool exported, bool is_const,
                   struct type_constraint *type) {
    struct symdef *a = (struct symdef *)std::malloc(sizeof(struct symdef));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'D';
    set_loc_from(a, v);
    a->name = s;
    a->v = v;
    a->type = type;
    a->exported = exported;
    a->is_const = is_const;
    return (struct ast *)a;
}

struct ast *newdestructure(struct ast *pattern, struct ast *value,
                           bool is_const) {
    struct destructure_node *a =
        (struct destructure_node *)std::malloc(
            sizeof(struct destructure_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_DESTRUCTURE;
    set_loc_from(a, value ? value : pattern);
    a->pattern = pattern;
    a->value = value;
    a->is_const = is_const ? 1 : 0;
    return (struct ast *)a;
}

struct ast *newret(struct ast *v) {
    struct fnret *a = (struct fnret *)std::malloc(sizeof(struct fnret));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'R';
    set_loc_from(a, v);
    a->v = v;
    return (struct ast *)a;
}

struct ast *newbrk(char *label, int line, int column) {
    struct nbreak *a = (struct nbreak *)std::malloc(sizeof(struct nbreak));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'B';
    set_loc(a, line, column);
    a->label = label;
    return (struct ast *)a;
}

struct ast *newctn(char *label, int line, int column) {
    struct ncontinue *a = (struct ncontinue *)std::malloc(sizeof(struct ncontinue));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'O';
    set_loc(a, line, column);
    a->label = label;
    return (struct ast *)a;
}

struct ast *newlabel(char *name, struct ast *stmt) {
    struct label_node *a =
        (struct label_node *)std::malloc(sizeof(struct label_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'g';
    set_loc_from(a, stmt);
    a->name = name;
    a->stmt = stmt;
    return (struct ast *)a;
}

struct ast *newfail(struct ast *v) {
    struct control_stmt *a =
        (struct control_stmt *)std::malloc(sizeof(struct control_stmt));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'u';
    set_loc_from(a, v);
    a->value = v;
    return (struct ast *)a;
}

struct ast *newrecover(struct ast *v) {
    struct control_stmt *a =
        (struct control_stmt *)std::malloc(sizeof(struct control_stmt));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'v';
    set_loc_from(a, v);
    a->value = v;
    return (struct ast *)a;
}

struct ast *newpass(struct ast *v) {
    struct control_stmt *a =
        (struct control_stmt *)std::malloc(sizeof(struct control_stmt));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'p';
    set_loc_from(a, v);
    a->value = v;
    return (struct ast *)a;
}

struct ast *newonerror(struct ast *call, char *name, struct ast *body) {
    struct on_error_node *a =
        (struct on_error_node *)std::malloc(sizeof(struct on_error_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'o';
    set_loc_from(a, call);
    a->call = call;
    a->name = name;
    a->body = body;
    return (struct ast *)a;
}

struct ast *newis(struct ast *value, struct type_constraint *type) {
    struct is_node *a = (struct is_node *)std::malloc(sizeof(struct is_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'k';
    set_loc_from(a, value);
    a->value = value;
    a->type = type;
    return (struct ast *)a;
}

struct ast *newspread(struct ast *value) {
    struct spread_node *a =
        (struct spread_node *)std::malloc(sizeof(struct spread_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'x';
    set_loc_from(a, value);
    a->value = value;
    return (struct ast *)a;
}

struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *el) {
    struct flow *a = (struct flow *)std::malloc(sizeof(struct flow));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    set_loc_from(a, cond);
    a->cond = cond;
    a->tl = tl;
    a->el = el;
    return (struct ast *)a;
}

struct ast *newfor(struct ast *init, struct ast *cond, struct ast *step, struct ast *body) {
    struct forloop *a = (struct forloop *)std::malloc(sizeof(struct forloop));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'P';
    set_loc_from(a, init ? init : (cond ? cond : body));
    a->init = init;
    a->cond = cond;
    a->step = step;
    a->body = body;
    return (struct ast *)a;
}

struct ast *newforin(char *name, char *value_name, struct ast *iterable,
                     struct ast *body) {
    struct forinloop *a =
        (struct forinloop *)std::malloc(sizeof(struct forinloop));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'F';
    set_loc_from(a, iterable ? iterable : body);
    a->name = name;
    a->value_name = value_name;
    a->iterable = iterable;
    a->body = body;
    return (struct ast *)a;
}

struct ast *newswitch(struct ast *expr, struct ast *cases) {
    struct switch_node *a =
        (struct switch_node *)std::malloc(sizeof(struct switch_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'S';
    set_loc_from(a, expr);
    a->expr = expr;
    a->cases = cases;
    return (struct ast *)a;
}

struct ast *newcase(struct ast *values, struct ast *body, struct ast *next) {
    struct case_node *a =
        (struct case_node *)std::malloc(sizeof(struct case_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'c';
    set_loc_from(a, body ? body : values);
    a->values = values;
    a->body = body;
    a->next = next;
    return (struct ast *)a;
}

struct ast *append_case(struct ast *head, struct ast *node) {
    if (!head) return node;
    struct ast *current = head;
    while (((struct case_node *)current)->next)
        current = ((struct case_node *)current)->next;
    ((struct case_node *)current)->next = node;
    return head;
}

struct ast *newmatch(struct ast *expr, struct ast *arms) {
    struct match_node *a =
        (struct match_node *)std::malloc(sizeof(struct match_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_MATCH;
    set_loc_from(a, expr);
    a->expr = expr;
    a->arms = arms;
    return (struct ast *)a;
}

struct ast *newmatcharm(struct ast *pattern, struct ast *guard,
                        struct ast *body, struct ast *next) {
    struct match_arm *a =
        (struct match_arm *)std::malloc(sizeof(struct match_arm));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_MATCH_ARM;
    set_loc_from(a, body ? body : pattern);
    a->pattern = pattern;
    a->guard = guard;
    a->body = body;
    a->next = next;
    return (struct ast *)a;
}

struct ast *append_matcharm(struct ast *head, struct ast *node) {
    if (!head) return node;
    struct ast *current = head;
    while (((struct match_arm *)current)->next) {
        if (!((struct match_arm *)current)->pattern) {
            yyerror("default must be the last match arm");
            treefree(node);
            return head;
        }
        current = ((struct match_arm *)current)->next;
    }
    if (!((struct match_arm *)current)->pattern) {
        yyerror("default must be the last match arm");
        treefree(node);
        return head;
    }
    ((struct match_arm *)current)->next = node;
    return head;
}

static struct pattern_node *new_pattern_node(int nodetype) {
    struct pattern_node *a =
        (struct pattern_node *)std::malloc(sizeof(struct pattern_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    set_loc(a, 0, 0);
    a->name = NULL;
    a->module = NULL;
    a->variant = NULL;
    a->literal = NULL;
    a->fields = NULL;
    a->items = NULL;
    a->next = NULL;
    return a;
}

struct ast *newpatternlit(struct ast *literal) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_LITERAL);
    set_loc_from(a, literal);
    a->literal = literal;
    return (struct ast *)a;
}

struct ast *newpatternname(char *name) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_BINDING);
    if (name && std::strcmp(name, "_") == 0) {
        std::free(name);
        name = NULL; /* wildcard */
    }
    a->name = name;
    return (struct ast *)a;
}

struct ast *newpatternrest(char *name) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_REST);
    a->name = name;
    return (struct ast *)a;
}

struct ast *newpatternas(struct ast *pattern, char *name) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_AS);
    a->items = pattern;
    a->name = name;
    return (struct ast *)a;
}

struct ast *newpatternnever() {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_NEVER);
    return (struct ast *)a;
}

struct ast *newpatterndict(struct pattern_field *fields) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_CONTAINER);
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newpatternlist(struct ast *items) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_LIST);
    a->items = items;
    return (struct ast *)a;
}

struct ast *newpatternstruct(char *name, char *module,
                             struct pattern_field *fields) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_CONTAINER);
    a->name = name;
    a->module = module;
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newpatternvariant(char *name, char *module, char *variant,
                              struct pattern_field *fields) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_CONTAINER);
    a->name = name;
    a->module = module;
    a->variant = variant;
    a->fields = fields;
    return (struct ast *)a;
}

struct pattern_field *newpatternfield(char *key, struct ast *pattern,
                                      struct pattern_field *next) {
    struct pattern_field *f =
        (struct pattern_field *)std::malloc(sizeof(struct pattern_field));
    if (!f) {
        yyerror("out of space");
        std::exit(0);
    }
    f->key = key;
    f->rest_name = NULL;
    f->pattern = pattern;
    f->next = next;
    return f;
}

struct pattern_field *newpatternrestfield(char *name,
                                          struct pattern_field *next) {
    struct pattern_field *f =
        (struct pattern_field *)std::malloc(sizeof(struct pattern_field));
    if (!f) {
        yyerror("out of space");
        std::exit(0);
    }
    f->key = NULL;
    f->rest_name = name;
    f->pattern = NULL;
    f->next = next;
    return f;
}

struct pattern_field *append_patternfield(struct pattern_field *head,
                                          struct pattern_field *node) {
    if (!head) return node;
    struct pattern_field *current = head;
    while (current->next)
        current = current->next;
    current->next = node;
    return head;
}

struct ast *append_patternitem(struct ast *head, struct ast *node) {
    if (!head) return node;
    struct ast *current = head;
    while (((struct pattern_node *)current)->next)
        current = ((struct pattern_node *)current)->next;
    ((struct pattern_node *)current)->next = node;
    return head;
}

struct ast *newfunc(char *s, struct symlist *sl, struct ast *ast,
                    bool exported, bool is_async, bool is_generator) {
    struct funcdef *a = (struct funcdef *)std::malloc(sizeof(struct funcdef));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'Z';
    set_loc_from(a, ast);
    if (!func_locations.empty()) {
        set_loc(a, func_locations.back().first,
                func_locations.back().second);
        func_locations.pop_back();
    }
    a->name = s;
    a->sl = sl;
    a->ast = ast;
    a->moved = 0;
    if (is_async && is_generator) {
        yyerror("async generators are not supported");
        is_generator = false;
    }
    a->exported = exported;
    a->is_async = is_async;
    a->is_generator = is_generator;
    return (struct ast *)a;
}

struct ast *newawait(struct ast *value) {
    struct ast *a = newast('b', value, NULL);
    return a;
}

struct ast *newyield(struct ast *value) {
    struct ast *a = newast('w', value, NULL);
    return a;
}

struct ast *newdefer(struct ast *value) {
    struct ast *a = newast('n', value, NULL);
    return a;
}

struct ast *newpackage(char *name, struct ast *body) {
    struct package_node *a =
        (struct package_node *)std::malloc(sizeof(struct package_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'K';
    set_loc_from(a, body);
    a->name = name;
    a->body = body;
    return (struct ast *)a;
}

struct ast *newimport(char *path, char *alias) {
    struct import_node *a =
        (struct import_node *)std::malloc(sizeof(struct import_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'J';
    set_loc(a, 0, 0);
    a->path = path;
    a->alias = alias;
    a->base = copy_string(package_current_import_base().c_str());
    return (struct ast *)a;
}

void free_function_code(struct function_code *code) {
    if (!code) return;
    if (code->ast) treefree(code->ast);
    if (code->sl) symlistfree(code->sl);
    delete code;
}

/*
 * A single `func` AST node can be executed more than once (a nested function
 * defined inside another function).  All executions share the same code
 * (body + parameter list), but each gets its own closure environment.
 */
static std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>
    function_codes;

std::shared_ptr<function_code> get_function_code(struct funcdef *node) {
    std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>::iterator
        it = function_codes.find(node);
    if (it != function_codes.end())
        return it->second;

    int nparams = 0;
    bool variadic = false;
    for (struct symlist *sl = node->sl; sl; sl = sl->next) {
        if (sl->variadic)
            variadic = true;
        else
            ++nparams;
    }
    std::shared_ptr<function_code> code(
        new function_code{node->ast, node->sl, nparams,
                          node->name ? node->name : "", false, variadic,
                          node->is_async != 0, node->is_generator != 0},
        free_function_code);
    function_codes[node] = code;
    return code;
}

void release_function_code(struct funcdef *node) {
    function_codes.erase(node);
}

struct ast *newlist(struct ast *v, struct ast *l) {
    struct list_node *p = (struct list_node *)std::malloc(sizeof(struct list_node));
    if (!p) {
        yyerror("out of space");
        std::exit(0);
    }
    p->nodetype = 'l';
    set_loc_from(p, v);
    p->value = v;
    p->next = l;
    return (struct ast *)p;
}

struct ast *newdict(struct ast *k, struct ast *v, struct ast *l) {
    struct dict_node *p = (struct dict_node *)std::malloc(sizeof(struct dict_node));
    if (!p) {
        yyerror("out of space");
        std::exit(0);
    }
    p->nodetype = 'd';
    set_loc_from(p, k);
    p->value = v;
    p->key = k;
    p->next = l;
    return (struct ast *)p;
}

struct ast *newstructdecl(char *name, char *module,
                          struct struct_member *members, bool exported) {
    struct struct_node *a =
        (struct struct_node *)std::malloc(sizeof(struct struct_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'Y';
    set_loc(a, 0, 0);
    a->name = name;
    a->module = module;
    a->members = members;
    a->exported = exported;
    return (struct ast *)a;
}

struct ast *newstructfield(char *name, struct type_constraint *type) {
    struct struct_member *a =
        (struct struct_member *)std::malloc(sizeof(struct struct_member));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'f';
    set_loc(a, 0, 0);
    a->name = name;
    a->type = type;
    a->func = NULL;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *newstructmethod(char *name, struct ast *func) {
    struct struct_member *a =
        (struct struct_member *)std::malloc(sizeof(struct struct_member));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'm';
    set_loc_from(a, func);
    a->name = name;
    a->type = NULL;
    a->func = func;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *append_struct_member(struct ast *head, struct ast *member) {
    if (!head) return member;
    struct struct_member *current = (struct struct_member *)head;
    while (current->next) current = current->next;
    current->next = (struct struct_member *)member;
    return head;
}

struct ast *newimpl(char *type_name, char *trait_name, char *module,
                    struct struct_member *methods) {
    struct impl_node *a =
        (struct impl_node *)std::malloc(sizeof(struct impl_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'H';
    set_loc(a, 0, 0);
    a->type_name = type_name;
    a->trait_name = trait_name;
    a->module = module;
    a->methods = methods;
    return (struct ast *)a;
}

struct ast *newtraitdecl(char *name, char *module,
                         struct trait_method *methods, bool exported) {
    struct trait_node *a =
        (struct trait_node *)std::malloc(sizeof(struct trait_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 't';
    set_loc(a, 0, 0);
    a->name = name;
    a->module = module;
    a->methods = methods;
    a->exported = exported;
    return (struct ast *)a;
}

struct ast *newtraitmethod(char *name, struct symlist *params) {
    struct trait_method *a =
        (struct trait_method *)std::malloc(sizeof(struct trait_method));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'q';
    set_loc(a, 0, 0);
    a->name = name;
    a->params = params;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *append_trait_method(struct ast *head, struct ast *method) {
    if (!head) return method;
    struct trait_method *current = (struct trait_method *)head;
    while (current->next) current = current->next;
    current->next = (struct trait_method *)method;
    return head;
}

struct ast *newstructlit(char *name, char *module,
                         struct struct_init *fields) {
    struct struct_lit_node *a =
        (struct struct_lit_node *)std::malloc(sizeof(struct struct_lit_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'y';
    set_loc(a, 0, 0);
    a->name = name;
    a->module = module;
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newenumdecl(char *name, char *module,
                        struct enum_variant *variants, bool exported) {
    struct enum_node *a =
        (struct enum_node *)std::malloc(sizeof(struct enum_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_ENUM_DECL;
    set_loc(a, 0, 0);
    a->name = name;
    a->module = module;
    a->variants = variants;
    a->exported = exported ? 1 : 0;
    return (struct ast *)a;
}

struct ast *newenumvariant(char *name, struct struct_member *fields,
                           struct enum_variant *next) {
    struct enum_variant *a =
        (struct enum_variant *)std::malloc(sizeof(struct enum_variant));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_ENUM_VARIANT;
    set_loc(a, 0, 0);
    a->name = name;
    a->fields = fields;
    a->next = next;
    return (struct ast *)a;
}

struct enum_variant *append_enumvariant(struct enum_variant *head,
                                        struct enum_variant *node) {
    if (!head) return node;
    struct enum_variant *current = head;
    while (current->next)
        current = current->next;
    current->next = node;
    return head;
}

struct ast *newenumlit(char *name, char *module, char *variant,
                       struct struct_init *fields) {
    struct enum_lit_node *a =
        (struct enum_lit_node *)std::malloc(sizeof(struct enum_lit_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_ENUM_LITERAL;
    set_loc(a, 0, 0);
    a->name = name;
    a->module = module;
    a->variant = variant;
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newstructinit(char *name, struct ast *value) {
    struct struct_init *a =
        (struct struct_init *)std::malloc(sizeof(struct struct_init));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'i';
    set_loc_from(a, value);
    a->name = name;
    a->value = value;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *append_struct_init(struct ast *head, struct ast *init) {
    if (!head) return init;
    struct struct_init *current = (struct struct_init *)head;
    while (current->next) current = current->next;
    current->next = (struct struct_init *)init;
    return head;
}

static void freepatternfields(struct pattern_field *field) {
    while (field) {
        struct pattern_field *next = field->next;
        std::free(field->key);
        std::free(field->rest_name);
        treefree(field->pattern);
        std::free(field);
        field = next;
    }
}

void treefree(struct ast *a) {
    if (!a) return;
    switch (a->nodetype) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'G':
        case 'j':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case 'L':
            treefree(a->r);
            treefree(a->l);
            break;
        case 'M':
        case '!':
        case '~':
        case 'b':
        case 'w':
        case 'n':
            treefree(a->l);
            break;
        case 'V':
            freeval(((struct val *)a)->val);
            break;
        case 'N':
            std::free(((struct symref *)a)->name);
            break;
        case 'B':
            std::free(((struct nbreak *)a)->label);
            break;
        case 'O':
            std::free(((struct ncontinue *)a)->label);
            break;
        case 'g': {
            struct label_node *n = (struct label_node *)a;
            treefree(n->stmt);
            std::free(n->name);
            break;
        }
        case 'u':
        case 'v':
        case 'p':
            treefree(((struct control_stmt *)a)->value);
            break;
        case 'o': {
            struct on_error_node *o = (struct on_error_node *)a;
            treefree(o->call);
            treefree(o->body);
            std::free(o->name);
            break;
        }
        case 'k': {
            struct is_node *n = (struct is_node *)a;
            treefree(n->value);
            free_type_constraint(n->type);
            break;
        }
        case 'x':
            treefree(((struct spread_node *)a)->value);
            break;
        case '=':
            treefree(((struct symasgn *)a)->target);
            treefree(((struct symasgn *)a)->v);
            break;
        case 'U':
            treefree(((struct incdec_node *)a)->target);
            break;
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)a;
            treefree(asgn->target);
            treefree(asgn->v);
            break;
        }
        case 'D':
            treefree(((struct symdef *)a)->v);
            free_type_constraint(((struct symdef *)a)->type);
            std::free(((struct symdef *)a)->name);
            break;
        case NODE_DESTRUCTURE: {
            struct destructure_node *d = (struct destructure_node *)a;
            treefree(d->pattern);
            treefree(d->value);
            break;
        }
        case 'R':
            treefree(((struct fnret *)a)->v);
            break;
        case 'Z': {
            /*
             * If the definition was evaluated, the body and parameter list
             * are owned by the runtime function value.  Otherwise (for
             * example, a duplicate definition error) release them here.
             */
            struct funcdef *f = (struct funcdef *)a;
            if (f->moved) {
                release_function_code(f);
            } else {
                treefree(f->ast);
                if (f->sl) symlistfree(f->sl);
            }
            std::free(f->name);
            break;
        }
        case 'I':
        case 'W':
        case 'E':
        case 'T': {
            struct flow *f = (struct flow *)a;
            treefree(f->cond);
            treefree(f->tl);
            treefree(f->el);
            break;
        }
        case 'P': {
            struct forloop *f = (struct forloop *)a;
            treefree(f->init);
            treefree(f->cond);
            treefree(f->step);
            treefree(f->body);
            break;
        }
        case 'F': {
            struct forinloop *f = (struct forinloop *)a;
            treefree(f->iterable);
            treefree(f->body);
            std::free(f->name);
            std::free(f->value_name);
            break;
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)a;
            treefree(s->expr);
            treefree(s->cases);
            break;
        }
        case 'c': {
            struct case_node *c = (struct case_node *)a;
            treefree(c->values);
            treefree(c->body);
            treefree(c->next);
            break;
        }
        case 'C':
            treefree(((struct ufncall *)a)->callee);
            treefree(((struct ufncall *)a)->args);
            break;
        case 'K': {
            struct package_node *p = (struct package_node *)a;
            treefree(p->body);
            std::free(p->name);
            break;
        }
        case 'J': {
            struct import_node *imp = (struct import_node *)a;
            std::free(imp->path);
            std::free(imp->alias);
            std::free(imp->base);
            break;
        }
        case 'X':
            treefree(((struct nscope *)a)->ast);
            break;
        case 'l': {
            struct list_node *p = (struct list_node *)a;
            treefree(p->value);
            treefree(p->next);
            break;
        }
        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            treefree(p->key);
            treefree(p->value);
            treefree(p->next);
            break;
        }
        case 'Y': {
            struct struct_node *s = (struct struct_node *)a;
            treefree((struct ast *)s->members);
            std::free(s->name);
            std::free(s->module);
            break;
        }
        case 'H': {
            struct impl_node *i = (struct impl_node *)a;
            treefree((struct ast *)i->methods);
            std::free(i->type_name);
            std::free(i->trait_name);
            std::free(i->module);
            break;
        }
        case 't': {
            struct trait_node *t = (struct trait_node *)a;
            treefree((struct ast *)t->methods);
            std::free(t->name);
            std::free(t->module);
            break;
        }
        case 'f': {
            struct struct_member *m = (struct struct_member *)a;
            treefree((struct ast *)m->next);
            free_type_constraint(m->type);
            std::free(m->name);
            break;
        }
        case 'm': {
            struct struct_member *m = (struct struct_member *)a;
            treefree((struct ast *)m->next);
            treefree(m->func);
            std::free(m->name);
            break;
        }
        case 'q': {
            struct trait_method *m = (struct trait_method *)a;
            treefree((struct ast *)m->next);
            symlistfree(m->params);
            std::free(m->name);
            break;
        }
        case 'y': {
            struct struct_lit_node *s = (struct struct_lit_node *)a;
            treefree((struct ast *)s->fields);
            std::free(s->name);
            std::free(s->module);
            break;
        }
        case 'i': {
            struct struct_init *i = (struct struct_init *)a;
            treefree((struct ast *)i->next);
            treefree(i->value);
            std::free(i->name);
            break;
        }
        case NODE_MATCH: {
            struct match_node *m = (struct match_node *)a;
            treefree(m->expr);
            treefree(m->arms);
            break;
        }
        case NODE_MATCH_ARM: {
            struct match_arm *m = (struct match_arm *)a;
            treefree(m->pattern);
            treefree(m->guard);
            treefree(m->body);
            treefree(m->next);
            break;
        }
        case NODE_PATTERN_LITERAL: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->literal);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_BINDING: {
            struct pattern_node *p = (struct pattern_node *)a;
            std::free(p->name);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_REST: {
            struct pattern_node *p = (struct pattern_node *)a;
            std::free(p->name);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_AS: {
            struct pattern_node *p = (struct pattern_node *)a;
            std::free(p->name);
            treefree(p->items);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_NEVER: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_CONTAINER: {
            struct pattern_node *p = (struct pattern_node *)a;
            std::free(p->name);
            std::free(p->module);
            std::free(p->variant);
            freepatternfields(p->fields);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_LIST: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->items);
            treefree(p->next);
            break;
        }
        case NODE_ENUM_DECL: {
            struct enum_node *e = (struct enum_node *)a;
            std::free(e->name);
            std::free(e->module);
            treefree((struct ast *)e->variants);
            break;
        }
        case NODE_ENUM_VARIANT: {
            struct enum_variant *v = (struct enum_variant *)a;
            std::free(v->name);
            treefree((struct ast *)v->fields);
            treefree((struct ast *)v->next);
            break;
        }
        case NODE_ENUM_LITERAL: {
            struct enum_lit_node *e = (struct enum_lit_node *)a;
            std::free(e->name);
            std::free(e->module);
            std::free(e->variant);
            treefree((struct ast *)e->fields);
            break;
        }
        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    std::free(a);
}
