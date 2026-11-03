#ifndef SKEIN_GC_H
#define SKEIN_GC_H

struct scope_context;

/*
 * Scope-granularity cycle collection.  Values are reference counted; the
 * collector clears scopes that are no longer reachable from the active roots,
 * which breaks closure cycles.  Values must not contain cycles themselves.
 */
void collect_cycles(struct scope_context &context);
void maybe_collect_cycles();

#endif
