#ifndef SKEIN_GC_H
#define SKEIN_GC_H

#include "gc_roots.h"

struct scope_context;

/*
 * Thread-local root provider.  Fibers keep values and scopes on their own
 * stacks while suspended; the collector asks the provider to mark them as
 * roots before sweeping.  `mark_value` is the usual value callback and
 * `mark_scope` keeps a suspended scope tree alive.
 */
struct scope_t;
using gc_scope_mark_fn = void (*)(void *ctx, struct scope_t *scope);
/*
 * `collector_ctx` is opaque collector state and must be passed through to the
 * two mark callbacks.
 */
using gc_thread_roots_fn = void (*)(void *collector_ctx,
                                    gc_scope_mark_fn mark_scope,
                                    gc_mark_fn mark_value);
void gc_set_thread_root_visitor(gc_thread_roots_fn visit);

/*
 * Reference counting plus cycle collection over scopes and explicit frame
 * roots.  Values are reference counted; the collector clears scopes that are
 * no longer reachable from the active roots, which breaks closure cycles.
 * Frame roots cover VM/JIT value slots, upvalue cells, defers and iterators.
 */
void collect_cycles(struct scope_context &context);
void maybe_collect_cycles();

/*
 * Upvalue-cell reachability.  Cells live in a per-thread registry and carry
 * a collection epoch; a reachable cell is stamped while marking.  Cells left
 * behind by closure cycles are broken after scope sweeping.
 */
struct upvalue_cell;
void gc_mark_upvalue_cell(struct upvalue_cell *cell);
/* Tear down every closed cell owned by `context` (context shutdown). */
void gc_sweep_all_cells(struct scope_context &context);

/* Break unreachable Message/Channel/duplex userdata cycles.  Runs only when
 * no worker threads are active.  The builtins layer registers the concrete
 * collector because the runtime must not link against builtins. */
void gc_set_thread_cycle_collector(void (*fn)());
void collect_thread_cycles();

/* True when SKEIN_GC_STRESS requests collection at VM safepoints. */
bool gc_stress_enabled();

#endif
