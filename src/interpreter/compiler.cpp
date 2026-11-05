#include "compiler.h"

#include "ast.h"
#include "eval_context.h"
#include "eval_internal.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using skein::bc::Constant;
using skein::bc::ConstantKind;
using skein::bc::Instr;
using skein::bc::Op;
using skein::bc::Proto;
using skein::bc::ProtoPtr;
using skein::bc::SourceLoc;

struct value_deleter {
    void operator()(value_t *value) const { freeval(value); }
};

std::shared_ptr<value_t> own_value(value_t *value) {
    return std::shared_ptr<value_t>(value, value_deleter());
}

struct LoopContext {
    std::string label;
    std::vector<size_t> break_patches;
    std::vector<size_t> continue_patches;
    int scope_depth = 0;
    int iter_depth = 0;
    int stack_cleanup = 0;
    bool accepts_continue = true;
};

struct Compiler {
    Proto proto;
    std::string error;
    std::vector<LoopContext> loops;
    std::vector<int> iter_stack;
    std::string pending_label;
    int scope_depth = 0;
    bool ok = true;

    explicit Compiler(const std::string &module) {
        proto.module = module;
    }

    SourceLoc loc_of(const ast *node) const {
        SourceLoc loc;
        if (node) {
            loc.line = node->line > 0 ? node->line : 1;
            loc.column = node->column > 0 ? node->column : 1;
        }
        return loc;
    }

    size_t emit(Op op, uint8_t flags, uint16_t a, uint32_t b,
                SourceLoc loc) {
        Instr ins;
        ins.op = static_cast<uint8_t>(op);
        ins.flags = flags;
        ins.a = a;
        ins.b = b;
        proto.code.push_back(ins);
        proto.locs.push_back(loc);
        return proto.code.size() - 1;
    }

    size_t emit_jump(Op op, SourceLoc loc) {
        return emit(op, 0, 0, 0, loc);
    }

    void patch(size_t at, size_t target) {
        if (at < proto.code.size())
            proto.code[at].b = static_cast<uint32_t>(target);
    }

    uint32_t add_value(value_t *value) {
        Constant c;
        c.kind = ConstantKind::Value;
        if (value)
            c.value = own_value(dupval(value));
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_name(const std::string &name) {
        Constant c;
        c.kind = ConstantKind::Name;
        c.name = name;
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_ast(struct ast *node) {
        Constant c;
        c.kind = ConstantKind::Ast;
        c.ast = node;
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_decl(struct symdef *decl) {
        Constant c;
        c.kind = ConstantKind::Decl;
        c.decl = decl;
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_type(const struct type_constraint *type) {
        Constant c;
        c.kind = ConstantKind::Type;
        c.type = type;
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_iter(const char *key_name, const char *value_name) {
        skein::bc::IterDesc desc;
        desc.key_name = key_name ? key_name : "";
        desc.value_name = value_name ? value_name : "";
        desc.two_vars = value_name != NULL;
        proto.iters.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.iters.size() - 1);
    }

    uint32_t add_index(const char *base_name,
                       const std::vector<bool> &fields) {
        skein::bc::IndexDesc desc;
        desc.base_name = base_name ? base_name : "";
        desc.fields = fields;
        proto.index_ops.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.index_ops.size() - 1);
    }

    uint32_t add_import(const struct import_node *node) {
        skein::bc::ImportDesc desc;
        desc.path = node && node->path ? node->path : "";
        desc.alias = node && node->alias ? node->alias : "";
        desc.base = node && node->base ? node->base : "";
        proto.imports.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.imports.size() - 1);
    }

    static bool collect_index_chain(ast *target,
                                    std::vector<ast *> &indices,
                                    std::vector<bool> &fields,
                                    ast *&base) {
        if (!target)
            return false;
        if (target->nodetype == 'G' || target->nodetype == 'j') {
            if (!collect_index_chain(target->l, indices, fields, base))
                return false;
            indices.push_back(target->r);
            fields.push_back(target->nodetype == 'j');
            return true;
        }
        base = target;
        return target->nodetype == 'N';
    }

    uint32_t add_label(const char *label) {
        if (!label || !*label)
            return UINT32_MAX;
        return add_name(label);
    }

    uint32_t add_bool(bool value) {
        Constant c;
        c.kind = ConstantKind::Value;
        c.value = std::shared_ptr<value_t>(newboolval(value), value_deleter());
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    void fail(const char *message) {
        if (ok)
            error = message ? message : "cannot compile";
        ok = false;
    }

    /*
     * Fallback safety: an Op::EvalAst subtree is executed by the AST
     * interpreter in the current scope.  If that subtree contains a break or
     * continue that is meant for an enclosing bytecode loop, the AST
     * interpreter would return a control value which the VM cannot route to
     * the loop.  In that case we decline the whole statement/function.
     */
    static bool escapes_control(const ast *node, int loop_depth,
                                int switch_depth) {
        if (!node)
            return false;

        switch (node->nodetype) {
            case 'V':
            case 'N':
            case 'J': // import executes in its own package/scope
            case 'K': // package body is a separate compilation unit
            case 'Z': // function body is not executed now
            case 'Y': // struct declaration; methods are not executed now
            case 'H': // impl declaration
            case 't': // trait declaration
            case '<':
            case '>':
            case 'n': // defer registration; body runs later with its own scope
                return false;

            case 'B': {
                struct nbreak *brk = (struct nbreak *)node;
                if (brk->label)
                    return true;
                return loop_depth == 0 && switch_depth == 0;
            }

            case 'O': {
                struct ncontinue *ctn = (struct ncontinue *)node;
                if (ctn->label)
                    return true;
                return loop_depth == 0;
            }

            case 'W': {
                struct flow *f = (struct flow *)node;
                return escapes_control(f->cond, loop_depth, switch_depth) ||
                       escapes_control(f->tl, loop_depth + 1, switch_depth);
            }

            case 'E': {
                struct flow *f = (struct flow *)node;
                return escapes_control(f->tl, loop_depth + 1,
                                       switch_depth) ||
                       escapes_control(f->cond, loop_depth, switch_depth);
            }

            case 'P': {
                struct forloop *f = (struct forloop *)node;
                return escapes_control(f->init, loop_depth, switch_depth) ||
                       escapes_control(f->cond, loop_depth, switch_depth) ||
                       escapes_control(f->step, loop_depth, switch_depth) ||
                       escapes_control(f->body, loop_depth + 1,
                                       switch_depth);
            }

            case 'F': {
                struct forinloop *f = (struct forinloop *)node;
                return escapes_control(f->iterable, loop_depth,
                                       switch_depth) ||
                       escapes_control(f->body, loop_depth + 1,
                                       switch_depth);
            }

            case 'S': {
                struct switch_node *s = (struct switch_node *)node;
                if (escapes_control(s->expr, loop_depth, switch_depth))
                    return true;
                for (struct case_node *c = (struct case_node *)s->cases; c;
                     c = (struct case_node *)c->next) {
                    if (escapes_control(c->values, loop_depth,
                                        switch_depth) ||
                        escapes_control(c->body, loop_depth,
                                        switch_depth + 1))
                        return true;
                }
                return false;
            }

            case '7': {
                struct match_node *m = (struct match_node *)node;
                if (escapes_control(m->expr, loop_depth, switch_depth))
                    return true;
                for (struct match_arm *arm = (struct match_arm *)m->arms;
                     arm; arm = (struct match_arm *)arm->next) {
                    if (escapes_control(arm->guard, loop_depth,
                                        switch_depth) ||
                        escapes_control(arm->body, loop_depth,
                                        switch_depth))
                        return true;
                }
                return false;
            }

            case 'o': {
                struct on_error_node *handler = (struct on_error_node *)node;
                return escapes_control(handler->call, loop_depth,
                                       switch_depth) ||
                       escapes_control(handler->body, loop_depth,
                                       switch_depth);
            }

            case 'I':
            case 'T': {
                struct flow *f = (struct flow *)node;
                return escapes_control(f->cond, loop_depth, switch_depth) ||
                       escapes_control(f->tl, loop_depth, switch_depth) ||
                       escapes_control(f->el, loop_depth, switch_depth);
            }

            case 'X':
                return escapes_control(((struct nscope *)node)->ast,
                                       loop_depth, switch_depth);

            case 'L':
                return escapes_control(node->l, loop_depth, switch_depth) ||
                       escapes_control(node->r, loop_depth, switch_depth);

            case 'D':
                return escapes_control(((struct symdef *)node)->v,
                                       loop_depth, switch_depth);

            case '=': {
                struct symasgn *asgn = (struct symasgn *)node;
                return escapes_control(asgn->target, loop_depth,
                                       switch_depth) ||
                       escapes_control(asgn->v, loop_depth, switch_depth);
            }

            case 'a': {
                struct compound_asgn *asgn = (struct compound_asgn *)node;
                return escapes_control(asgn->target, loop_depth,
                                       switch_depth) ||
                       escapes_control(asgn->v, loop_depth, switch_depth);
            }

            case 'U':
                return escapes_control(((struct incdec_node *)node)->target,
                                       loop_depth, switch_depth);

            case 'R':
                return escapes_control(((struct fnret *)node)->v,
                                       loop_depth, switch_depth);

            case 'u':
            case 'v':
            case 'p':
                return escapes_control(((struct control_stmt *)node)->value,
                                       loop_depth, switch_depth);

            case 'b':
            case 'w':
                return escapes_control(node->l, loop_depth, switch_depth);

            case 'C': {
                struct ufncall *call = (struct ufncall *)node;
                return escapes_control(call->callee, loop_depth,
                                       switch_depth) ||
                       escapes_control(call->args, loop_depth, switch_depth);
            }

            case 'k':
                return escapes_control(((struct is_node *)node)->value,
                                       loop_depth, switch_depth);

            case 'x':
                return escapes_control(((struct spread_node *)node)->value,
                                       loop_depth, switch_depth);

            case 'g':
                return escapes_control(((struct label_node *)node)->stmt,
                                       loop_depth, switch_depth);

            case '`':
                return escapes_control(
                    ((struct destructure_node *)node)->value, loop_depth,
                    switch_depth);

            case 'l': {
                for (struct list_node *item = (struct list_node *)node; item;
                     item = (struct list_node *)item->next) {
                    if (escapes_control(item->value, loop_depth,
                                        switch_depth))
                        return true;
                }
                return false;
            }

            case 'd': {
                for (struct dict_node *entry = (struct dict_node *)node;
                     entry; entry = (struct dict_node *)entry->next) {
                    if (escapes_control(entry->key, loop_depth,
                                        switch_depth) ||
                        escapes_control(entry->value, loop_depth,
                                        switch_depth))
                        return true;
                }
                return false;
            }

            case 'y':
            case '?': {
                struct struct_init *init =
                    node->nodetype == 'y'
                        ? ((struct struct_lit_node *)node)->fields
                        : ((struct enum_lit_node *)node)->fields;
                for (; init; init = init->next) {
                    if (escapes_control(init->value, loop_depth,
                                        switch_depth))
                        return true;
                }
                return false;
            }

            default:
                return escapes_control(node->l, loop_depth, switch_depth) ||
                       escapes_control(node->r, loop_depth, switch_depth);
        }
    }

    bool fallback_safe(const ast *node) const {
        if (loops.empty())
            return true;
        return !escapes_control(node, 0, 0);
    }

    void compile_fallback(ast *node) {
        if (!fallback_safe(node)) {
            fail("unsupported control flow in fallback expression");
            return;
        }
        SourceLoc loc = loc_of(node);
        uint32_t index = add_ast(node);
        emit(Op::EvalAst, 0, 0, index, loc);
    }

    void emit_null(SourceLoc loc) {
        emit(Op::LoadNull, 0, 0, 0, loc);
    }

    void compile(ast *node) {
        if (!ok)
            return;
        if (!node) {
            emit(Op::LoadNull, 0, 0, 0, SourceLoc());
            return;
        }

        SourceLoc here = loc_of(node);
        switch (node->nodetype) {
            case 'V': {
                struct val *literal = (struct val *)node;
                if (!literal->val) {
                    emit_null(here);
                } else {
                    uint32_t index = add_value(literal->val);
                    emit(Op::LoadConst, 0, 0, index, here);
                }
                return;
            }

            case 'N': {
                struct symref *ref = (struct symref *)node;
                uint32_t index = add_name(ref->name ? ref->name : "");
                emit(Op::LoadName, 0, 0, index, here);
                return;
            }

            case 'D': {
                struct symdef *def = (struct symdef *)node;
                if (def->v) {
                    compile(def->v);
                    if (!ok) return;
                }
                uint32_t index = add_decl(def);
                emit(Op::DefineName, def->v ? 1 : 0, 0, index, here);
                return;
            }

            case '=': {
                struct symasgn *asgn = (struct symasgn *)node;
                if (asgn->target && asgn->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)asgn->target;
                    compile(asgn->v);
                    if (!ok) return;
                    uint32_t index = add_name(ref->name ? ref->name : "");
                    emit(Op::StoreName, 0, 0, index, here);
                    return;
                }
                if (asgn->target &&
                    (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(asgn->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_fallback(node);
                        return;
                    }
                    compile(asgn->v);
                    if (!ok) return;
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index(
                        ((struct symref *)base)->name, fields);
                    emit(Op::AssignIndex, 0, 0, desc, here);
                    return;
                }
                compile_fallback(node);
                return;
            }

            case 'a': {
                struct compound_asgn *asgn = (struct compound_asgn *)node;
                if (asgn->target && asgn->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)asgn->target;
                    compile(asgn->v);
                    if (!ok) return;
                    uint32_t index = add_name(ref->name ? ref->name : "");
                    emit(Op::CompoundName, static_cast<uint8_t>(asgn->op),
                          0, index, here);
                    return;
                }
                if (asgn->target &&
                    (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(asgn->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_fallback(node);
                        return;
                    }
                    compile(asgn->v);
                    if (!ok) return;
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index(
                        ((struct symref *)base)->name, fields);
                    emit(Op::CompoundIndex,
                         static_cast<uint8_t>(asgn->op), 0, desc, here);
                    return;
                }
                compile_fallback(node);
                return;
            }

            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)node;
                if (inc->target && inc->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)inc->target;
                    uint32_t index = add_name(ref->name ? ref->name : "");
                    emit(Op::IncDecName, inc->prefix ? 1 : 0,
                          inc->delta > 0 ? 1 : 0, index, here);
                    return;
                }
                if (inc->target &&
                    (inc->target->nodetype == 'G' ||
                     inc->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(inc->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_fallback(node);
                        return;
                    }
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index(
                        ((struct symref *)base)->name, fields);
                    emit(Op::IncDecIndex, inc->prefix ? 1 : 0,
                         inc->delta > 0 ? 1 : 0, desc, here);
                    return;
                }
                compile_fallback(node);
                return;
            }

            case 'M':
                compile(node->l);
                if (!ok) return;
                emit(Op::Neg, 0, 0, 0, here);
                return;

            case '!':
                compile(node->l);
                if (!ok) return;
                emit(Op::Not, 0, 0, 0, here);
                return;

            case '~':
                compile(node->l);
                if (!ok) return;
                emit(Op::BitNot, 0, 0, 0, here);
                return;

            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '&':
            case '|':
            case '^':
            case 's':
            case 'r': {
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                Op op = Op::Nop;
                switch (node->nodetype) {
                    case '+': op = Op::Add; break;
                    case '-': op = Op::Sub; break;
                    case '*': op = Op::Mul; break;
                    case '/': op = Op::Div; break;
                    case '%': op = Op::Mod; break;
                    case '&': op = Op::BitAnd; break;
                    case '|': op = Op::BitOr; break;
                    case '^': op = Op::BitXor; break;
                    case 's': op = Op::Shl; break;
                    case 'r': op = Op::Shr; break;
                }
                emit(op, 0, 0, 0, here);
                return;
            }

            case 'A':
                compile_logical(node, true);
                return;

            case 'Q':
                compile_logical(node, false);
                return;

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6': {
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                Op op = Op::CmpEq;
                switch (node->nodetype) {
                    case '1': op = Op::CmpGt; break;
                    case '2': op = Op::CmpLt; break;
                    case '3': op = Op::CmpNe; break;
                    case '4': op = Op::CmpEq; break;
                    case '5': op = Op::CmpGe; break;
                    case '6': op = Op::CmpLe; break;
                }
                emit(op, 0, 0, 0, here);
                return;
            }

            case 'T': {
                struct flow *f = (struct flow *)node;
                compile(f->cond);
                if (!ok) return;
                size_t jf = emit_jump(Op::JumpIfFalse, here);
                compile(f->tl);
                if (!ok) return;
                size_t jend = emit_jump(Op::Jump, here);
                patch(jf, proto.code.size());
                compile(f->el);
                if (!ok) return;
                patch(jend, proto.code.size());
                return;
            }

            case 'I': {
                struct flow *f = (struct flow *)node;
                compile(f->cond);
                if (!ok) return;
                size_t jf = emit_jump(Op::JumpIfFalse, here);
                compile(f->tl);
                if (!ok) return;
                size_t jend = emit_jump(Op::Jump, here);
                patch(jf, proto.code.size());
                if (f->el)
                    compile(f->el);
                else
                    emit_null(here);
                if (!ok) return;
                patch(jend, proto.code.size());
                return;
            }

            case 'W':
                compile_while(node);
                return;

            case 'E':
                compile_do(node);
                return;

            case 'P':
                compile_for(node);
                return;

            case 'B': {
                struct nbreak *brk = (struct nbreak *)node;
                compile_break_continue(true, brk->label, here);
                return;
            }

            case 'O': {
                struct ncontinue *ctn = (struct ncontinue *)node;
                compile_break_continue(false, ctn->label, here);
                return;
            }

            case 'R': {
                struct fnret *ret = (struct fnret *)node;
                if (ret->v)
                    compile(ret->v);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Return, 0, 0, 0, here);
                return;
            }

            case 'g': {
                struct label_node *lab = (struct label_node *)node;
                ast *child = lab->stmt;
                if (child && (child->nodetype == 'W' || child->nodetype == 'E' ||
                              child->nodetype == 'P' || child->nodetype == 'F' ||
                              child->nodetype == 'S')) {
                    std::string saved = pending_label;
                    pending_label = lab->name ? lab->name : "";
                    compile(child);
                    pending_label = saved;
                } else {
                    compile_fallback(node);
                }
                return;
            }

            case 'l':
                compile_list(node);
                return;

            case 'd':
                compile_dict(node);
                return;

            case 'G':
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                emit(Op::GetIndex, 0, 0, 0, here);
                return;

            case 'j':
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                emit(Op::GetField, 0, 0, 0, here);
                return;

            case 'C':
                compile_call(node);
                return;

            case 'X': {
                struct nscope *scope = (struct nscope *)node;
                emit(Op::PushScope, 0, 0, 0, here);
                ++scope_depth;
                compile(scope->ast);
                if (!ok) return;
                emit(Op::PopScope, 0, 0, 0, here);
                --scope_depth;
                return;
            }

            case 'L':
                compile_statement_list(node);
                return;

            case 'k': {
                struct is_node *is = (struct is_node *)node;
                compile(is->value);
                if (!ok) return;
                uint32_t index = add_type(is->type);
                emit(Op::IsType, 0, 0, index, here);
                return;
            }

            case 'b':
                compile(node->l);
                if (!ok) return;
                emit(Op::Await, 0, 0, 0, here);
                return;

            case 'w':
                compile(node->l);
                if (!ok) return;
                emit(Op::Yield, 0, 0, 0, here);
                return;

            case 'u': {
                struct control_stmt *stmt = (struct control_stmt *)node;
                if (stmt->value)
                    compile(stmt->value);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Fail, 0, 0, 0, here);
                return;
            }

            case 'J': {
                uint32_t desc = add_import((struct import_node *)node);
                emit(Op::Import, 0, static_cast<uint16_t>(desc), 0, here);
                return;
            }

            case 'F':
                compile_forin(node);
                return;

            case 'S':
                compile_switch(node);
                return;

            case '7': // match
            case 'o': // on error
            case 'v': // recover
            case 'p': // pass
            case 'n': // defer
            case 'K': // package
            case 'Y':
            case 'H':
            case 't':
            case 'y':
            case '?':
            case '<':
            case '>':
            case '`':
            case 'x':
            default:
                compile_fallback(node);
                return;
        }
    }

    void compile_logical(ast *node, bool is_and) {
        SourceLoc here = loc_of(node);
        compile(node->l);
        if (!ok) return;
        emit(Op::Dup, 0, 0, 0, here);
        size_t branch =
            emit_jump(is_and ? Op::JumpIfFalse : Op::JumpIfTrue, here);
        emit(Op::Pop, 0, 0, 0, here);
        compile(node->r);
        if (!ok) return;
        emit(Op::Not, 0, 0, 0, here);
        emit(Op::Not, 0, 0, 0, here); // normalize to bool
        size_t jend = emit_jump(Op::Jump, here);
        patch(branch, proto.code.size());
        emit(Op::Pop, 0, 0, 0, here);
        uint32_t bool_index = add_bool(!is_and);
        emit(Op::LoadConst, 0, 0, bool_index, here);
        patch(jend, proto.code.size());
    }

    void compile_statement_list(ast *node) {
        std::vector<ast *> items;
        collect_chain(node, items);
        if (items.empty()) {
            emit(Op::LoadNull, 0, 0, 0, loc_of(node));
            return;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (!ok) return;
            compile(items[i]);
            if (!ok) return;
            if (i + 1 < items.size())
                emit(Op::Pop, 0, 0, 0, loc_of(items[i]));
        }
    }

    static void collect_chain(ast *node, std::vector<ast *> &out) {
        if (!node) return;
        if (node->nodetype == 'L') {
            collect_chain(node->l, out);
            collect_chain(node->r, out);
        } else {
            out.push_back(node);
        }
    }

    void compile_while(ast *node) {
        struct flow *f = (struct flow *)node;
        SourceLoc here = loc_of(node);
        size_t start = proto.code.size();

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        size_t jf = static_cast<size_t>(-1);
        if (f->cond) {
            compile(f->cond);
            if (!ok) return;
            jf = emit_jump(Op::JumpIfFalse, here);
        }

        compile(f->tl);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, start);

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(start), here);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        if (jf != static_cast<size_t>(-1))
            patch(jf, end);
        loops.pop_back();
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_do(ast *node) {
        struct flow *f = (struct flow *)node;
        SourceLoc here = loc_of(node);
        size_t start = proto.code.size();

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        compile(f->tl);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        size_t cond_start = proto.code.size();
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, cond_start);

        compile(f->cond);
        if (!ok) return;
        size_t jt = emit_jump(Op::JumpIfTrue, here);
        patch(jt, start);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_for(ast *node) {
        struct forloop *f = (struct forloop *)node;
        SourceLoc here = loc_of(node);

        emit(Op::PushScope, 0, 0, 0, here);
        ++scope_depth;
        if (f->init) {
            compile(f->init);
            if (!ok) return;
            emit(Op::Pop, 0, 0, 0, here);
        }

        size_t start = proto.code.size();
        size_t jf = static_cast<size_t>(-1);
        if (f->cond) {
            compile(f->cond);
            if (!ok) return;
            jf = emit_jump(Op::JumpIfFalse, here);
        }

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        compile(f->body);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        size_t step_start = proto.code.size();
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, step_start);

        if (f->step) {
            compile(f->step);
            if (!ok) return;
            emit(Op::Pop, 0, 0, 0, here);
        }

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(start), here);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        if (jf != static_cast<size_t>(-1))
            patch(jf, end);

        emit(Op::PopScope, 0, 0, 0, here);
        --scope_depth;
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_forin(ast *node) {
        struct forinloop *f = (struct forinloop *)node;
        SourceLoc here = loc_of(node);

        compile(f->iterable);
        if (!ok) return;

        emit(Op::PushScope, 0, 0, 0, here);
        ++scope_depth;

        uint32_t desc = add_iter(f->name, f->value_name);
        emit(Op::IterNew, 0, static_cast<uint16_t>(desc), 0, here);

        size_t next_pc = proto.code.size();
        size_t jend = emit(Op::IterNext, 0, static_cast<uint16_t>(desc),
                           0, here);

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        loops.push_back(ctx);
        iter_stack.push_back(static_cast<int>(desc));
        loops.back().iter_depth = static_cast<int>(iter_stack.size());
        size_t ctx_index = loops.size() - 1;

        compile(f->body);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(next_pc), here);

        size_t end = proto.code.size();
        patch(jend, end);
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, next_pc);
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        iter_stack.pop_back();

        emit(Op::IterEnd, 0, static_cast<uint16_t>(desc), 0, here);
        emit(Op::PopScope, 0, 0, 0, here);
        --scope_depth;
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_switch(ast *node) {
        struct switch_node *sw = (struct switch_node *)node;
        SourceLoc here = loc_of(node);

        compile(sw->expr);
        if (!ok) return;
        emit(Op::SwitchCheck, 0, 0, 0, here);

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.stack_cleanup = 1; // the subject stays on the operand stack
        ctx.accepts_continue = false;
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        std::vector<size_t> end_jumps;
        struct case_node *default_case = NULL;

        for (struct case_node *c = (struct case_node *)sw->cases; c;
             c = (struct case_node *)c->next) {
            if (!c->values) {
                default_case = c;
                continue;
            }
            if (case_values_have_condition(c->values)) {
                fail("switch case values contain conditions");
                return;
            }

            SourceLoc case_loc = loc_of((struct ast *)c);
            compile(c->values);
            if (!ok) return;
            emit(Op::CaseMatch, 0, 0, 0, here);
            size_t jf = emit_jump(Op::JumpIfFalse, case_loc);

            emit(Op::PushScope, 0, 0, 0, case_loc);
            ++scope_depth;
            compile(c->body);
            if (!ok) return;
            emit(Op::PopScope, 0, 0, 0, case_loc);
            --scope_depth;
            emit(Op::Pop, 0, 0, 0, case_loc);
            end_jumps.push_back(emit_jump(Op::Jump, case_loc));
            patch(jf, proto.code.size());
        }

        if (default_case) {
            SourceLoc case_loc = loc_of((struct ast *)default_case);
            emit(Op::PushScope, 0, 0, 0, case_loc);
            ++scope_depth;
            compile(default_case->body);
            if (!ok) return;
            emit(Op::PopScope, 0, 0, 0, case_loc);
            --scope_depth;
            emit(Op::Pop, 0, 0, 0, case_loc);
        }

        size_t end_cleanup = proto.code.size();
        for (size_t patch_at : end_jumps)
            patch(patch_at, end_cleanup);
        for (size_t patch_at : loops[ctx_index].break_patches)
            patch(patch_at, end_cleanup);
        loops.pop_back();

        emit(Op::Pop, 0, 0, 0, here); // discard the switch subject
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    std::string take_pending_label() {
        std::string label = pending_label;
        pending_label.clear();
        return label;
    }

    void compile_break_continue(bool is_break, const char *label,
                                SourceLoc loc) {
        LoopContext *target = NULL;
        if (label && *label) {
            for (size_t i = loops.size(); i > 0; --i) {
                if (loops[i - 1].label != label)
                    continue;
                if (!is_break && !loops[i - 1].accepts_continue)
                    continue;
                target = &loops[i - 1];
                break;
            }
        } else if (is_break) {
            if (!loops.empty())
                target = &loops.back();
        } else {
            for (size_t i = loops.size(); i > 0; --i) {
                if (loops[i - 1].accepts_continue) {
                    target = &loops[i - 1];
                    break;
                }
            }
        }

        if (!target) {
            uint32_t index = add_label(label);
            emit(is_break ? Op::ControlBreak : Op::ControlContinue, 0, 0,
                 index, loc);
            return;
        }

        size_t target_index =
            static_cast<size_t>(target - loops.data());

        /*
         * A break/continue may leave lexical blocks inside the loop body.
         * Pop those scopes before jumping so defers run and symbols do not
         * leak past their block.
         *
         * If the jump leaves one or more for-in loops, release their
         * iterator state as well.  The target loop's own iterator (if any)
         * is not included; its normal exit path emits the matching IterEnd.
         */
        for (size_t i = iter_stack.size();
             i > static_cast<size_t>(target->iter_depth); --i) {
            emit(Op::IterEnd, 0,
                 static_cast<uint16_t>(iter_stack[i - 1]), 0, loc);
        }

        /*
         * A switch keeps its subject value on the operand stack.  When a
         * break/continue jumps past a switch (for example to an outer loop),
         * discard that subject first.
         */
        for (size_t i = loops.size(); i > target_index + 1; --i) {
            for (int n = 0; n < loops[i - 1].stack_cleanup; ++n)
                emit(Op::Pop, 0, 0, 0, loc);
        }

        for (int depth = scope_depth; depth > target->scope_depth; --depth)
            emit(Op::PopScope, 0, 0, 0, loc);

        size_t patch_at = emit_jump(Op::Jump, loc);
        if (is_break)
            target->break_patches.push_back(patch_at);
        else
            target->continue_patches.push_back(patch_at);
    }

    void compile_list(ast *node) {
        struct list_node *item = (struct list_node *)node;
        std::vector<ast *> values;
        for (; item; item = (struct list_node *)item->next) {
            if (!item->value) continue;
            if (item->value->nodetype == 'x') {
                compile_fallback(node);
                return;
            }
            values.push_back(item->value);
        }
        for (ast *value : values) {
            compile(value);
            if (!ok) return;
        }
        emit(Op::NewList, 0, static_cast<uint16_t>(values.size()),
             static_cast<uint32_t>(values.size()), loc_of(node));
    }

    void compile_dict(ast *node) {
        struct dict_node *entry = (struct dict_node *)node;
        std::vector<ast *> keys;
        std::vector<ast *> values;
        for (; entry; entry = (struct dict_node *)entry->next) {
            if (!entry->key || !entry->value) continue;
            keys.push_back(entry->key);
            values.push_back(entry->value);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            compile(keys[i]);
            compile(values[i]);
            if (!ok) return;
        }
        emit(Op::NewDict, 0, static_cast<uint16_t>(keys.size()),
             static_cast<uint32_t>(keys.size()), loc_of(node));
    }

    void compile_call(ast *node) {
        struct ufncall *call = (struct ufncall *)node;
        std::vector<ast *> args;
        collect_chain(call->args, args);
        for (ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                compile_fallback(node);
                return;
            }
        }

        compile(call->callee);
        for (ast *arg : args) {
            compile(arg);
            if (!ok) return;
        }
        if (!ok) return;
        emit(Op::Call, 0, static_cast<uint16_t>(args.size()),
             static_cast<uint32_t>(args.size()), loc_of(node));
    }
};

} // namespace

ProtoPtr skein_compile_statement(struct ast *statement,
                                 const std::string &module,
                                 std::string &error) {
    Compiler compiler(module);
    compiler.proto.name = "<top>";
    compiler.compile(statement);
    if (!compiler.ok) {
        error = compiler.error.empty() ? "cannot compile statement"
                                       : compiler.error;
        return nullptr;
    }
    compiler.proto.max_stack = 0;
    return std::make_shared<const Proto>(std::move(compiler.proto));
}

ProtoPtr skein_compile_function_body(struct function_code *code,
                                     const std::string &module,
                                     std::string &error) {
    if (!code || !code->ast) {
        error = "function has no body";
        return nullptr;
    }

    Compiler compiler(module);
    compiler.proto.name = code->name;
    compiler.proto.nparams = static_cast<uint32_t>(code->nparams);
    compiler.proto.required_params =
        static_cast<uint32_t>(code->required_params);
    compiler.proto.variadic = code->variadic;
    compiler.proto.is_async = code->is_async;
    compiler.proto.is_generator = code->is_generator;
    compiler.proto.is_method = code->is_method;
    compiler.proto.def_line = code->ast->line;
    compiler.proto.def_column = code->ast->column;

    compiler.compile(code->ast);
    if (!compiler.ok) {
        error = compiler.error.empty() ? "cannot compile function"
                                       : compiler.error;
        return nullptr;
    }
    return std::make_shared<const Proto>(std::move(compiler.proto));
}
