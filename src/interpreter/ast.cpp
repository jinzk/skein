#include "ast.h"
#include "compiler.h"
#include "diagnostics.h"
#include "package.h"
#include "scopestack.h"
#include "vm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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

/*
 * AST nodes are numerous and short-lived, and every node is released through
 * treefree() or one of the two helpers below.  malloc/free per node is a
 * measurable part of parsing large packages, so node structs come from a
 * size-class arena.  Names, type constraints and symbol lists keep using
 * malloc because they are owned by other subsystems.
 *
 * Blocks are carved from process-wide 64 KiB chunks and recycled through
 * per-thread free lists.  Chunks stay alive until process exit: an AST
 * allocated while parsing one thread may be freed by another when a function
 * code object is released, and keeping the chunks reachable makes that safe
 * without a lock on every free.
 */
namespace {

constexpr size_t kAstAlign = alignof(std::max_align_t);
constexpr size_t kAstMaxPayload = 512;
constexpr size_t kAstClassCount = kAstMaxPayload / kAstAlign;
constexpr size_t kAstChunkSize = 64 * 1024;
constexpr uint32_t kAstLargeClass = 0xffffffffu;

struct ast_free_block {
    ast_free_block *next;
};

constexpr uint32_t kAstMagic = 0x41535442u; /* "ASTB" */

struct ast_block_header {
    uint32_t magic;
    uint32_t class_index;
    uint64_t reserved;
};
static_assert(sizeof(ast_block_header) == kAstAlign,
              "AST block header must match the arena alignment");

struct ast_chunk {
    ast_chunk *next;
};

struct ast_thread_pool {
    ast_free_block *free_lists[kAstClassCount] = {};
    unsigned char *cursor = nullptr;
    unsigned char *limit = nullptr;
};

std::mutex g_ast_chunk_mutex;
ast_chunk *g_ast_chunks = nullptr;
thread_local ast_thread_pool g_ast_pool;

bool ast_arena_enabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("SKEIN_AST_ARENA");
        if (!value || !*value)
            return true;
        return std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0;
    }();
    return enabled;
}

void ast_add_chunk(size_t need) {
    const size_t capacity = need > kAstChunkSize ? need : kAstChunkSize;
    ast_chunk *chunk = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ast_chunk_mutex);
        chunk = static_cast<ast_chunk *>(
            std::malloc(sizeof(ast_chunk) + capacity + kAstAlign));
        if (!chunk) {
            yyerror("out of space");
            std::exit(0);
        }
        chunk->next = g_ast_chunks;
        g_ast_chunks = chunk;
    }
    unsigned char *data = reinterpret_cast<unsigned char *>(chunk + 1);
    uintptr_t address = reinterpret_cast<uintptr_t>(data);
    address = (address + kAstAlign - 1) & ~(uintptr_t)(kAstAlign - 1);
    g_ast_pool.cursor = reinterpret_cast<unsigned char *>(address);
    g_ast_pool.limit = g_ast_pool.cursor + capacity;
}

} // namespace

void *ast_node_alloc(size_t size) {
    if (!ast_arena_enabled())
        return std::malloc(size);
    const size_t payload = (size + kAstAlign - 1) & ~(kAstAlign - 1);
    if (payload <= kAstMaxPayload) {
        const size_t index = payload / kAstAlign - 1;
        ast_free_block *block = g_ast_pool.free_lists[index];
        if (block) {
            g_ast_pool.free_lists[index] = block->next;
            return block;
        }
        const size_t block_size = sizeof(ast_block_header) + payload;
        if (!g_ast_pool.cursor ||
            static_cast<size_t>(g_ast_pool.limit - g_ast_pool.cursor) <
                block_size) {
            ast_add_chunk(block_size);
        }
        ast_block_header *header =
            reinterpret_cast<ast_block_header *>(g_ast_pool.cursor);
        header->magic = kAstMagic;
        header->class_index = static_cast<uint32_t>(index);
        header->reserved = 0;
        g_ast_pool.cursor += block_size;
        return header + 1;
    }
    ast_block_header *header = static_cast<ast_block_header *>(
        std::malloc(sizeof(ast_block_header) + payload));
    if (!header) {
        yyerror("out of space");
        std::exit(0);
    }
    header->magic = kAstMagic;
    header->class_index = kAstLargeClass;
    header->reserved = 0;
    return header + 1;
}

void ast_node_free(void *ptr) {
    if (!ptr)
        return;
    if (!ast_arena_enabled()) {
        std::free(ptr);
        return;
    }
    ast_block_header *header =
        reinterpret_cast<ast_block_header *>(ptr) - 1;
    if (header->magic != kAstMagic) {
#ifndef NDEBUG
        std::fprintf(stderr,
                     "internal error: ast_node_free on non-arena pointer %p\n",
                     ptr);
        std::abort();
#else
        /* Defensive fallback for a node allocated outside the arena. */
        std::free(ptr);
        return;
#endif
    }
    if (header->class_index == kAstLargeClass) {
        std::free(header);
        return;
    }
    ast_free_block *block = static_cast<ast_free_block *>(ptr);
    block->next = g_ast_pool.free_lists[header->class_index];
    g_ast_pool.free_lists[header->class_index] = block;
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

static thread_local std::vector<std::pair<int, int>> func_locations;

void push_func_location(int line, int column) {
    func_locations.push_back(std::make_pair(line, column));
}

void clear_func_locations() {
    func_locations.clear();
}

struct ast *newast(int nodetype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)ast_node_alloc(sizeof(struct ast));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = newintval(i);
    return (struct ast *)a;
}

struct ast *newvalnode(value_t *value) {
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    set_loc(a, 0, 0);
    a->val = value;
    return (struct ast *)a;
}

struct ast *newfloat(double d) {
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct val *a = (struct val *)ast_node_alloc(sizeof(struct val));
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
    struct ast *a = (struct ast *)ast_node_alloc(sizeof(struct ast));
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
    struct ufncall *a =
        (struct ufncall *)ast_node_alloc(sizeof(struct ufncall));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'C';
    set_loc_from(a, callee);
    a->callee = callee;
    a->args = l;
    a->launch = 0;
    a->awaited = 0;
    a->wait_sugar = 0;
    return (struct ast *)a;
}

struct ast *newref(char *s) {
    struct symref *a = (struct symref *)ast_node_alloc(sizeof(struct symref));
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
    struct symasgn *a =
        (struct symasgn *)ast_node_alloc(sizeof(struct symasgn));
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
        (struct compound_asgn *)ast_node_alloc(sizeof(struct compound_asgn));
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
        (struct incdec_node *)ast_node_alloc(sizeof(struct incdec_node));
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
    struct nscope *a = (struct nscope *)ast_node_alloc(sizeof(struct nscope));
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
    struct symdef *a = (struct symdef *)ast_node_alloc(sizeof(struct symdef));
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
    struct destructure_node *a = (struct destructure_node *)
        ast_node_alloc(sizeof(struct destructure_node));
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
    struct fnret *a = (struct fnret *)ast_node_alloc(sizeof(struct fnret));
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
    struct nbreak *a = (struct nbreak *)ast_node_alloc(sizeof(struct nbreak));
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
    struct ncontinue *a =
        (struct ncontinue *)ast_node_alloc(sizeof(struct ncontinue));
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
        (struct label_node *)ast_node_alloc(sizeof(struct label_node));
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

struct ast *newgoto(char *name, int line, int column) {
    struct goto_node *a =
        (struct goto_node *)ast_node_alloc(sizeof(struct goto_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_GOTO;
    set_loc(a, line, column);
    a->name = name;
    return (struct ast *)a;
}

struct ast *newfail(struct ast *v) {
    struct control_stmt *a =
        (struct control_stmt *)ast_node_alloc(sizeof(struct control_stmt));
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
        (struct control_stmt *)ast_node_alloc(sizeof(struct control_stmt));
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
        (struct control_stmt *)ast_node_alloc(sizeof(struct control_stmt));
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
        (struct on_error_node *)ast_node_alloc(sizeof(struct on_error_node));
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
    struct is_node *a =
        (struct is_node *)ast_node_alloc(sizeof(struct is_node));
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

struct ast *newcast(struct ast *value, struct type_constraint *type) {
    struct cast_node *a =
        (struct cast_node *)ast_node_alloc(sizeof(struct cast_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_CAST;
    set_loc_from(a, value);
    a->value = value;
    a->type = type;
    return (struct ast *)a;
}

struct ast *newspread(struct ast *value) {
    struct spread_node *a =
        (struct spread_node *)ast_node_alloc(sizeof(struct spread_node));
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
    struct flow *a = (struct flow *)ast_node_alloc(sizeof(struct flow));
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
    struct forloop *a =
        (struct forloop *)ast_node_alloc(sizeof(struct forloop));
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
        (struct forinloop *)ast_node_alloc(sizeof(struct forinloop));
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
        (struct switch_node *)ast_node_alloc(sizeof(struct switch_node));
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
        (struct case_node *)ast_node_alloc(sizeof(struct case_node));
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
        (struct match_node *)ast_node_alloc(sizeof(struct match_node));
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
        (struct match_arm *)ast_node_alloc(sizeof(struct match_arm));
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
        (struct pattern_node *)ast_node_alloc(sizeof(struct pattern_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    set_loc(a, 0, 0);
    a->name = NULL;
    a->source = NULL;
    a->variant = NULL;
    a->literal = NULL;
    a->type = NULL;
    a->range_start = 0;
    a->range_end = 0;
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

struct ast *newpatterntype(struct type_constraint *type) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_TYPE);
    a->type = type;
    return (struct ast *)a;
}

struct ast *newpatternor(struct ast *left, struct ast *right) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_OR);
    a->items = left;
    if (left) {
        struct pattern_node *current = (struct pattern_node *)left;
        while (current->next) current = (struct pattern_node *)current->next;
        current->next = right;
    } else {
        a->items = right;
    }
    return (struct ast *)a;
}

struct ast *newpatternrange(long long start, long long end) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_RANGE);
    a->range_start = start;
    a->range_end = end;
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

struct ast *newpatterntuple(struct ast *items) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_TUPLE);
    a->items = items;
    return (struct ast *)a;
}

struct ast *newpatternstruct(char *name, char *source,
                             struct pattern_field *fields) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_CONTAINER);
    a->name = name;
    a->source = source;
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newpatternvariant(char *name, char *source, char *variant,
                              struct pattern_field *fields) {
    struct pattern_node *a = new_pattern_node(NODE_PATTERN_CONTAINER);
    a->name = name;
    a->source = source;
    a->variant = variant;
    a->fields = fields;
    return (struct ast *)a;
}

struct pattern_field *newpatternfield(char *key, struct ast *pattern,
                                      struct pattern_field *next) {
    struct pattern_field *f =
        (struct pattern_field *)ast_node_alloc(sizeof(struct pattern_field));
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
        (struct pattern_field *)ast_node_alloc(sizeof(struct pattern_field));
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
                    bool exported, bool is_generator,
                    struct type_constraint *ret_type) {
    struct funcdef *a =
        (struct funcdef *)ast_node_alloc(sizeof(struct funcdef));
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
    a->generic_params = NULL;
    a->sl = sl;
    if (ast && ast->nodetype != 'R') {
        struct ast *body = newast(NODE_FUNCTION_BODY, ast, NULL);
        body->line = ast->line;
        body->column = ast->column;
        ast = body;
    }
    a->ast = ast;
    a->ret_type = ret_type;
    a->moved = 0;
    a->exported = exported;
    a->is_generator = is_generator;
    a->is_method = 0;
    a->decorated_generator_source = NULL;
    a->method_self_type = NULL;
    return (struct ast *)a;
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
        (struct package_node *)ast_node_alloc(sizeof(struct package_node));
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
        (struct import_node *)ast_node_alloc(sizeof(struct import_node));
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

struct ast *newinclude(char *path) {
    struct include_node *a =
        (struct include_node *)ast_node_alloc(sizeof(struct include_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_INCLUDE;
    set_loc(a, 0, 0);
    a->path = path;
    return (struct ast *)a;
}

struct symlist_metadata {
    struct ast *default_value = NULL;
    struct ast *pattern = NULL;
};

static thread_local std::unordered_map<struct symlist *, struct symlist_metadata>
    symlist_metadata_map;

struct ast *symlist_default_ast(struct symlist *sl) {
    if (!sl)
        return NULL;
    auto it = symlist_metadata_map.find(sl);
    return it == symlist_metadata_map.end() ? NULL : it->second.default_value;
}

struct ast *take_symlist_default_ast(struct symlist *sl) {
    if (!sl)
        return NULL;
    auto it = symlist_metadata_map.find(sl);
    if (it == symlist_metadata_map.end()) {
        sl->has_default = false;
        return NULL;
    }
    struct ast *value = it->second.default_value;
    it->second.default_value = NULL;
    sl->has_default = false;
    if (!it->second.pattern)
        symlist_metadata_map.erase(it);
    return value;
}

void set_symlist_default_ast(struct symlist *sl, struct ast *body) {
    if (!sl)
        return;
    symlist_metadata &meta = symlist_metadata_map[sl];
    if (meta.default_value && meta.default_value != body)
        treefree(meta.default_value);
    meta.default_value = body;
    /*
     * has_default records the syntactic presence of a default expression.
     * The VM may release the AST after compiling it, but the parameter still
     * has a default and must keep taking the default-value path.
     */
    if (body)
        sl->has_default = true;
    if (!meta.default_value && !meta.pattern)
        symlist_metadata_map.erase(sl);
}

struct ast *symlist_pattern_ast(struct symlist *sl) {
    if (!sl)
        return NULL;
    auto it = symlist_metadata_map.find(sl);
    return it == symlist_metadata_map.end() ? NULL : it->second.pattern;
}

void set_symlist_pattern_ast(struct symlist *sl, struct ast *pattern) {
    if (!sl)
        return;
    symlist_metadata &meta = symlist_metadata_map[sl];
    if (meta.pattern && meta.pattern != pattern)
        treefree(meta.pattern);
    meta.pattern = pattern;
    if (pattern)
        sl->has_pattern = true;
    if (!meta.default_value && !meta.pattern)
        symlist_metadata_map.erase(sl);
}

void clear_symlist_metadata(struct symlist *sl) {
    for (struct symlist *p = sl; p; p = p->next) {
        auto it = symlist_metadata_map.find(p);
        if (it == symlist_metadata_map.end())
            continue;
        treefree(it->second.default_value);
        treefree(it->second.pattern);
        symlist_metadata_map.erase(it);
    }
}

void symlistfree_full(struct symlist *sl) {
    clear_symlist_metadata(sl);
    symlistfree(sl);
}

static thread_local std::unordered_map<function_code *, struct ast *> function_bodies;

struct ast *function_body_ast(struct function_code *code) {
    if (!code)
        return NULL;
    auto it = function_bodies.find(code);
    return it == function_bodies.end() ? NULL : it->second;
}

void set_function_body_ast(struct function_code *code, struct ast *body) {
    if (!code)
        return;
    if (body)
        function_bodies[code] = body;
    else
        function_bodies.erase(code);
}

void clear_function_body_ast(struct function_code *code) {
    if (!code)
        return;
    auto it = function_bodies.find(code);
    if (it == function_bodies.end())
        return;
    treefree(it->second);
    function_bodies.erase(it);
}

void free_function_code(struct function_code *code) {
    if (!code) return;
    skein_vm_forget_function_code(code);
    clear_function_body_ast(code);
    if (code->sl) symlistfree_full(code->sl);
    delete code;
}

/*
 * A single `func` AST node can be executed more than once (a nested function
 * defined inside another function).  All executions share the same code
 * (body + parameter list), but each gets its own closure environment.
 */
static thread_local std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>
    function_codes;

std::shared_ptr<function_code> get_function_code(struct funcdef *node) {
    std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>::iterator
        it = function_codes.find(node);
    if (it != function_codes.end())
        return it->second;

    int nparams = 0;
    int required_params = 0;
    bool variadic = false;
    for (struct symlist *sl = node->sl; sl; sl = sl->next) {
        if (sl->variadic) {
            variadic = true;
        } else {
            ++nparams;
            if (!sl->has_default)
                ++required_params;
        }
    }
    std::shared_ptr<function_code> code(
        new function_code(), free_function_code);
    code->sl = node->sl;
    code->nparams = nparams;
    code->required_params = required_params;
    code->name = node->name ? node->name : "";
    code->is_method = node->is_method != 0;
    code->variadic = variadic;
    code->is_generator = node->is_generator != 0;
    code->def_line = node->ast ? node->ast->line : 0;
    code->def_column = node->ast ? node->ast->column : 0;
    code->package = diag_current_package_name();
    code->module = diag_current_module_name();
    code->source = diag_current_source_name();
    if (node->ret_type) {
        code->ret_type =
            std::make_shared<type_constraint>(*node->ret_type);
    }
    struct ast *body = node->ast;
    if (!body)
        body = newret(newnullast());
    set_function_body_ast(code.get(), body);
    if (!skein_vm_enabled())
        skein_check_function_labels(code.get(), body);
    function_codes[node] = code;
    return code;
}

void release_function_code(struct funcdef *node) {
    function_codes.erase(node);
}

void ast_thread_shutdown() {
    function_codes.clear();
    for (auto &entry : function_bodies)
        treefree(entry.second);
    function_bodies.clear();
    for (auto &entry : symlist_metadata_map) {
        treefree(entry.second.default_value);
        treefree(entry.second.pattern);
    }
    symlist_metadata_map.clear();
    func_locations.clear();
}

struct ast *newlist(struct ast *v, struct ast *l) {
    struct list_node *p =
        (struct list_node *)ast_node_alloc(sizeof(struct list_node));
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

struct ast *newtuple(struct ast *v, struct ast *l) {
    struct ast *tuple = newlist(v, l);
    tuple->nodetype = NODE_TUPLE;
    return tuple;
}

struct ast *newdict(struct ast *k, struct ast *v, struct ast *l) {
    struct dict_node *p =
        (struct dict_node *)ast_node_alloc(sizeof(struct dict_node));
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

static struct generic_constraint_item *new_generic_constraint_item(
    struct type_constraint *type) {
    struct generic_constraint_item *item =
        (struct generic_constraint_item *)
            ast_node_alloc(sizeof(struct generic_constraint_item));
    if (!item) {
        yyerror("out of space");
        std::exit(0);
    }
    item->type = type;
    item->next = NULL;
    return item;
}

struct generic_constraint *newgenericconstraint(
    enum generic_constraint_kind kind, struct type_constraint *type) {
    struct generic_constraint *constraint =
        (struct generic_constraint *)
            ast_node_alloc(sizeof(struct generic_constraint));
    if (!constraint) {
        yyerror("out of space");
        std::exit(0);
    }
    constraint->kind = kind;
    constraint->items = new_generic_constraint_item(type);
    return constraint;
}

struct generic_constraint *combine_genericconstraint(
    struct generic_constraint *left, int op,
    struct generic_constraint *right) {
    if (!left)
        return right;
    if (!right)
        return left;
    enum generic_constraint_kind wanted =
        op == '&' ? GENERIC_CONSTRAINT_AND : GENERIC_CONSTRAINT_OR;
    enum generic_constraint_kind forbidden =
        op == '&' ? GENERIC_CONSTRAINT_OR : GENERIC_CONSTRAINT_AND;
    if (left->kind == forbidden || right->kind == forbidden) {
        yyerror("generic constraints cannot mix '&' and '|'");
        return left;
    }
    if (left->kind == GENERIC_CONSTRAINT_SINGLE)
        left->kind = wanted;
    if (left->kind != wanted) {
        yyerror("generic constraints cannot mix '&' and '|'");
        return left;
    }
    struct generic_constraint_item *tail = left->items;
    while (tail && tail->next)
        tail = tail->next;
    if (tail)
        tail->next = right->items;
    else
        left->items = right->items;
    return left;
}

struct generic_type_arg *newgenerictypearg(
    struct type_constraint *type) {
    struct generic_type_arg *arg =
        (struct generic_type_arg *)ast_node_alloc(
            sizeof(struct generic_type_arg));
    if (!arg) {
        yyerror("out of space");
        std::exit(0);
    }
    arg->type = type;
    arg->next = NULL;
    return arg;
}

struct generic_type_arg *append_generictypearg(
    struct generic_type_arg *head, struct generic_type_arg *arg) {
    if (!head)
        return arg;
    struct generic_type_arg *current = head;
    while (current->next)
        current = current->next;
    current->next = arg;
    return head;
}

struct generic_param *newgenericparam(char *name,
                                      struct generic_constraint *constraint) {
    struct generic_param *param =
        (struct generic_param *)ast_node_alloc(sizeof(struct generic_param));
    if (!param) {
        yyerror("out of space");
        std::exit(0);
    }
    param->name = name;
    param->constraint = constraint;
    param->default_type = NULL;
    param->next = NULL;
    return param;
}

struct generic_param *append_genericparam(struct generic_param *head,
                                          struct generic_param *param) {
    if (!head)
        return param;
    struct generic_param *current = head;
    while (current->next)
        current = current->next;
    current->next = param;
    return head;
}

struct ast *newstructdecl(char *name, char *source,
                          struct generic_param *generic_params,
                          struct struct_member *members, bool exported) {
    struct struct_node *a =
        (struct struct_node *)ast_node_alloc(sizeof(struct struct_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'Y';
    set_loc(a, 0, 0);
    a->name = name;
    a->source = source;
    a->module = copy_string(diag_current_module_name().c_str());
    a->generic_params = generic_params;
    a->members = members;
    a->exported = exported;
    return (struct ast *)a;
}

struct ast *newstructfield(char *name, struct type_constraint *type) {
    struct struct_member *a =
        (struct struct_member *)ast_node_alloc(sizeof(struct struct_member));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'f';
    set_loc(a, 0, 0);
    a->name = name;
    a->type = type;
    a->func = NULL;
    a->default_value = NULL;
    a->decorators = NULL;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *newstructmethod(char *name, struct ast *func) {
    struct struct_member *a =
        (struct struct_member *)ast_node_alloc(sizeof(struct struct_member));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'm';
    set_loc_from(a, func);
    a->name = name;
    a->type = NULL;
    a->func = func;
    a->default_value = NULL;
    a->decorators = NULL;
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

struct ast *newimpl(char *type_name, char *source,
                    struct struct_member *methods) {
    struct impl_node *a =
        (struct impl_node *)ast_node_alloc(sizeof(struct impl_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'H';
    set_loc(a, 0, 0);
    a->type_name = type_name;
    a->source = source;
    a->methods = methods;
    return (struct ast *)a;
}

struct ast *newprotocoldecl(char *name, char *source,
                         struct generic_param *generic_params,
                         struct protocol_method *methods, bool exported) {
    struct protocol_node *a =
        (struct protocol_node *)ast_node_alloc(sizeof(struct protocol_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 't';
    set_loc(a, 0, 0);
    a->name = name;
    a->source = source;
    a->module = copy_string(diag_current_module_name().c_str());
    a->generic_params = generic_params;
    a->methods = methods;
    a->exported = exported;
    return (struct ast *)a;
}

struct ast *newprotocolmethod(char *name, struct symlist *params,
                           struct type_constraint *ret_type, struct ast *func,
                           bool has_default) {
    struct protocol_method *a =
        (struct protocol_method *)ast_node_alloc(sizeof(struct protocol_method));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'q';
    set_loc(a, 0, 0);
    a->name = name;
    a->params = params;
    a->ret_type = ret_type;
    a->func = func;
    a->has_default = has_default;
    a->next = NULL;
    return (struct ast *)a;
}

struct ast *append_protocol_method(struct ast *head, struct ast *method) {
    if (!head) return method;
    struct protocol_method *current = (struct protocol_method *)head;
    while (current->next) current = current->next;
    current->next = (struct protocol_method *)method;
    return head;
}

struct ast *newbracketapply(struct ast *base, int start_line,
                            int start_column, int end_line,
                            int end_column, int suffix,
                            struct ast *suffix_value) {
    struct bracket_apply_node *a =
        (struct bracket_apply_node *)
            ast_node_alloc(sizeof(struct bracket_apply_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_BRACKET_APPLY;
    set_loc(a, base ? base->line : start_line,
            base ? base->column : start_column);
    a->base = base;
    a->span.start_line = start_line;
    a->span.start_column = start_column;
    a->span.end_line = end_line;
    a->span.end_column = end_column;
    a->suffix = suffix;
    a->suffix_value = suffix_value;
    return (struct ast *)a;
}

struct ast *newoptionalchain(struct ast *base, char *name) {
    struct optional_chain_node *a =
        (struct optional_chain_node *)
            ast_node_alloc(sizeof(struct optional_chain_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_OPTIONAL_CHAIN;
    set_loc(a, base ? base->line : 0, base ? base->column : 0);
    a->l = base;
    a->r = newstr(name ? name : "");
    return (struct ast *)a;
}

struct ast *newstructlit(char *name, char *source,
                         struct generic_type_arg *type_args,
                         struct struct_init *fields, bool is_ref) {
    struct struct_lit_node *a =
        (struct struct_lit_node *)
            ast_node_alloc(sizeof(struct struct_lit_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'y';
    set_loc(a, 0, 0);
    a->name = name;
    a->source = source;
    a->type_args = type_args;
    a->fields = fields;
    a->is_ref = is_ref;
    return (struct ast *)a;
}

struct ast *newenumdecl(char *name, char *source,
                        struct generic_param *generic_params,
                        struct enum_variant *variants, bool exported) {
    struct enum_node *a =
        (struct enum_node *)ast_node_alloc(sizeof(struct enum_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_ENUM_DECL;
    set_loc(a, 0, 0);
    a->name = name;
    a->source = source;
    a->module = copy_string(diag_current_module_name().c_str());
    a->generic_params = generic_params;
    a->variants = variants;
    a->exported = exported ? 1 : 0;
    return (struct ast *)a;
}

struct ast *newenumvariant(char *name, struct struct_member *fields,
                           struct enum_variant *next) {
    struct enum_variant *a =
        (struct enum_variant *)ast_node_alloc(sizeof(struct enum_variant));
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

struct ast *newenumlit(char *name, char *source, char *variant,
                       struct generic_type_arg *type_args,
                       struct struct_init *fields) {
    struct enum_lit_node *a =
        (struct enum_lit_node *)ast_node_alloc(sizeof(struct enum_lit_node));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = NODE_ENUM_LITERAL;
    set_loc(a, 0, 0);
    a->name = name;
    a->source = source;
    a->variant = variant;
    a->type_args = type_args;
    a->fields = fields;
    return (struct ast *)a;
}

struct ast *newstructinit(char *name, struct ast *value) {
    struct struct_init *a =
        (struct struct_init *)ast_node_alloc(sizeof(struct struct_init));
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
        ast_node_free(field);
        field = next;
    }
}

static void free_generic_constraint(struct generic_constraint *constraint) {
    if (!constraint)
        return;
    struct generic_constraint_item *item = constraint->items;
    while (item) {
        struct generic_constraint_item *next = item->next;
        free_type_constraint(item->type);
        ast_node_free(item);
        item = next;
    }
    ast_node_free(constraint);
}

static void free_generic_type_args(struct generic_type_arg *args) {
    while (args) {
        struct generic_type_arg *next = args->next;
        free_type_constraint(args->type);
        ast_node_free(args);
        args = next;
    }
}

static void free_generic_params(struct generic_param *params) {
    while (params) {
        struct generic_param *next = params->next;
        free_generic_constraint(params->constraint);
        free_type_constraint(params->default_type);
        std::free(params->name);
        ast_node_free(params);
        params = next;
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
        case NODE_FUNCTION_BODY:
        case NODE_OPTIONAL_CHAIN:
        case NODE_REF_TARGET:
            treefree(a->r);
            treefree(a->l);
            break;
        case NODE_BRACKET_APPLY: {
            struct bracket_apply_node *b = (struct bracket_apply_node *)a;
            treefree(b->base);
            treefree(b->suffix_value);
            break;
        }
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
        case NODE_GOTO:
            std::free(((struct goto_node *)a)->name);
            break;
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
        case NODE_CAST: {
            struct cast_node *n = (struct cast_node *)a;
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
                /*
                 * Static checking can materialize a function_code before
                 * evaluation (for example while inferring generator types).
                 * That code owns the body and parameter list, so release it
                 * instead of freeing the same storage twice.
                 */
                auto code = function_codes.find(f);
                if (code != function_codes.end()) {
                    function_codes.erase(code);
                    f->ast = NULL;
                    f->sl = NULL;
                } else {
                    treefree(f->ast);
                    if (f->sl) symlistfree_full(f->sl);
                }
            }
            free_type_constraint(f->ret_type);
            free_type_constraint(f->method_self_type);
            free_generic_params(f->generic_params);
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
        case NODE_INCLUDE: {
            struct include_node *inc = (struct include_node *)a;
            std::free(inc->path);
            break;
        }
        case 'X':
            treefree(((struct nscope *)a)->ast);
            break;
        case 'l':
        case NODE_TUPLE: {
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
            free_generic_params(s->generic_params);
            treefree((struct ast *)s->members);
            std::free(s->name);
            std::free(s->source);
            std::free(s->module);
            break;
        }
        case 'H': {
            struct impl_node *i = (struct impl_node *)a;
            treefree((struct ast *)i->methods);
            std::free(i->type_name);
            std::free(i->source);
            break;
        }
        case 't': {
            struct protocol_node *t = (struct protocol_node *)a;
            free_generic_params(t->generic_params);
            treefree((struct ast *)t->methods);
            std::free(t->name);
            std::free(t->source);
            std::free(t->module);
            break;
        }
        case 'f': {
            struct struct_member *m = (struct struct_member *)a;
            treefree((struct ast *)m->next);
            treefree(m->default_value);
            free_type_constraint(m->type);
            std::free(m->name);
            break;
        }
        case 'm': {
            struct struct_member *m = (struct struct_member *)a;
            treefree((struct ast *)m->next);
            treefree(m->func);
            treefree(m->decorators);
            std::free(m->name);
            break;
        }
        case 'q': {
            struct protocol_method *m = (struct protocol_method *)a;
            treefree((struct ast *)m->next);
            treefree(m->func);
            symlistfree_full(m->params);
            free_type_constraint(m->ret_type);
            std::free(m->name);
            break;
        }
        case 'y': {
            struct struct_lit_node *s = (struct struct_lit_node *)a;
            free_generic_type_args(s->type_args);
            treefree((struct ast *)s->fields);
            std::free(s->name);
            std::free(s->source);
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
        case NODE_PATTERN_TYPE: {
            struct pattern_node *p = (struct pattern_node *)a;
            free_type_constraint(p->type);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_OR: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->items);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_RANGE: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_CONTAINER: {
            struct pattern_node *p = (struct pattern_node *)a;
            std::free(p->name);
            std::free(p->source);
            std::free(p->variant);
            freepatternfields(p->fields);
            treefree(p->next);
            break;
        }
        case NODE_PATTERN_LIST:
        case NODE_PATTERN_TUPLE: {
            struct pattern_node *p = (struct pattern_node *)a;
            treefree(p->items);
            treefree(p->next);
            break;
        }
        case NODE_ENUM_DECL: {
            struct enum_node *e = (struct enum_node *)a;
            free_generic_params(e->generic_params);
            std::free(e->name);
            std::free(e->source);
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
            std::free(e->source);
            std::free(e->variant);
            free_generic_type_args(e->type_args);
            treefree((struct ast *)e->fields);
            break;
        }
        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    ast_node_free(a);
}
