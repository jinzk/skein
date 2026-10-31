#include "eval_context.h"

#include "valuetype.h"

namespace {

eval_context &default_context() {
    static eval_context context;
    return context;
}

eval_context *current_context = NULL;

} // namespace

eval_context::~eval_context() {
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
