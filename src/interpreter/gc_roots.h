#ifndef SKEIN_GC_ROOTS_H
#define SKEIN_GC_ROOTS_H

#include "valuetype.h"

#include <cstddef>
#include <vector>

struct scope_context;

/*
 * Called by the collector to mark one rooted value.  `ctx` is collector
 * private state, not the root frame's context.
 */
using gc_mark_fn = void (*)(void *ctx, struct value_t *value);

/*
 * Called for a root frame that needs custom traversal (for example a VM
 * executor whose vectors can reallocate).  `ctx` is the frame's context;
 * `mark_ctx` is opaque collector state passed back to `mark`.
 */
using gc_root_visit_fn = void (*)(void *ctx, gc_mark_fn mark,
                                  void *mark_ctx);

/*
 * One registered frame.  Array roots are used by JIT frames, whose value
 * slots live in a stable native-stack array.  Visitor roots are used by VM
 * frames, whose vector storage may move.
 */
struct gc_root_frame {
    enum class Kind : unsigned char {
        Array,
        Visitor,
    };

    Kind kind = Kind::Array;
    void *ctx = nullptr;
    size_t count = 0;
    gc_root_visit_fn visit = nullptr;
};

/* Register an array of value slots and return the frame mark for pop. */
size_t gc_roots_push_array(struct scope_context &context,
                           struct value_t **values, size_t count);

/* Register a custom root visitor and return the frame mark for pop. */
size_t gc_roots_push_visitor(struct scope_context &context, void *ctx,
                             gc_root_visit_fn visit);

/*
 * Remove every root frame registered after `mark`.  Out-of-order marks are
 * clamped defensively; frame teardown must still be LIFO.
 */
void gc_roots_pop(struct scope_context &context, size_t mark);

/* Visit every registered root in registration order. */
void gc_roots_visit(struct scope_context &context, gc_mark_fn mark,
                    void *mark_ctx);

/*
 * RAII root set for values that only live in C++ locals while user code
 * runs.  The collector cannot discover those values by itself, so builtins
 * that accumulate results across callbacks (map/reduce/sort keys, ...) must
 * register their C++ holders here.
 *
 * Marking re-reads the stored vector, so entries may be added or dropped
 * while the guard is alive.  The guard must be destroyed before the values
 * it references are freed.
 */
class gc_root_scope {
public:
    explicit gc_root_scope(struct scope_context &context);
    ~gc_root_scope();

    gc_root_scope(const gc_root_scope &) = delete;
    gc_root_scope &operator=(const gc_root_scope &) = delete;

    void add(struct value_t *value) {
        if (value)
            values_.push_back(value);
    }
    void add_all(const std::vector<struct value_t *> &values) {
        for (struct value_t *value : values)
            add(value);
    }
    /* Mark the live contents of a C++ vector that grows while user code
     * runs; the vector itself must outlive the guard. */
    void add_vector(std::vector<struct value_t *> *values) {
        if (values)
            vectors_.push_back(values);
    }
    void reset() {
        values_.clear();
        vectors_.clear();
    }

private:
    static void visit(void *ctx, gc_mark_fn mark, void *mark_ctx);

    struct scope_context *context_ = nullptr;
    size_t mark_ = 0;
    std::vector<struct value_t *> values_;
    std::vector<std::vector<struct value_t *> *> vectors_;
};

#endif
