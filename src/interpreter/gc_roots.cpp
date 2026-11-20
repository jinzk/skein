#include "gc_roots.h"

#include "scopestack.h"

#include <vector>

size_t gc_roots_push_array(struct scope_context &context,
                           struct value_t **values, size_t count) {
    const size_t mark = context.gc_roots.size();
    gc_root_frame frame;
    frame.kind = gc_root_frame::Kind::Array;
    frame.ctx = values;
    frame.count = count;
    context.gc_roots.push_back(frame);
    return mark;
}

size_t gc_roots_push_visitor(struct scope_context &context, void *ctx,
                             gc_root_visit_fn visit) {
    const size_t mark = context.gc_roots.size();
    gc_root_frame frame;
    frame.kind = gc_root_frame::Kind::Visitor;
    frame.ctx = ctx;
    frame.visit = visit;
    context.gc_roots.push_back(frame);
    return mark;
}

void gc_roots_pop(struct scope_context &context, size_t mark) {
    if (mark < context.gc_roots.size())
        context.gc_roots.resize(mark);
    else if (mark > context.gc_roots.size())
        context.gc_roots.clear();
}

void gc_roots_visit(struct scope_context &context, gc_mark_fn mark,
                    void *mark_ctx) {
    if (!mark)
        return;
    for (const gc_root_frame &frame : context.gc_roots) {
        if (frame.kind == gc_root_frame::Kind::Array) {
            struct value_t **values =
                static_cast<struct value_t **>(frame.ctx);
            if (!values)
                continue;
            for (size_t i = 0; i < frame.count; ++i) {
                if (values[i])
                    mark(mark_ctx, values[i]);
            }
            continue;
        }
        if (frame.visit)
            frame.visit(frame.ctx, mark, mark_ctx);
    }
}

gc_root_scope::gc_root_scope(struct scope_context &context)
    : context_(&context) {
    mark_ = gc_roots_push_visitor(context, this, &gc_root_scope::visit);
}

gc_root_scope::~gc_root_scope() {
    if (context_)
        gc_roots_pop(*context_, mark_);
}

void gc_root_scope::visit(void *ctx, gc_mark_fn mark, void *mark_ctx) {
    if (!ctx || !mark)
        return;
    gc_root_scope *self = static_cast<gc_root_scope *>(ctx);
    for (struct value_t *value : self->values_) {
        if (value)
            mark(mark_ctx, value);
    }
    for (std::vector<struct value_t *> *values : self->vectors_) {
        if (!values)
            continue;
        for (struct value_t *value : *values) {
            if (value)
                mark(mark_ctx, value);
        }
    }
}
