#include "eval_context.h"

#include "gc_roots.h"
#include "valuetype.h"

namespace {

eval_context &default_context() {
    static eval_context context;
    return context;
}

eval_context *current_context = NULL;

void visit_eval_context_roots(void *ctx, gc_mark_fn mark,
                              void *mark_ctx) {
    eval_context *context = static_cast<eval_context *>(ctx);
    if (context && context->control_value)
        mark(mark_ctx, context->control_value);
}

} // namespace

eval_context::eval_context() {
    gc_root_mark = gc_roots_push_visitor(
        scopes, this, &visit_eval_context_roots);
}

eval_context::~eval_context() {
    gc_roots_pop(scopes, gc_root_mark);
    if (control_value) {
        freeval(control_value);
        control_value = NULL;
    }
    donescope(scopes);
}

eval_context &active_eval_context() {
    if (!current_context)
        set_active_eval_context(&default_context());
    return *current_context;
}

void set_active_eval_context(eval_context *context) {
    current_context = context ? context : &default_context();
    set_active_scope_context(&current_context->scopes);
}
