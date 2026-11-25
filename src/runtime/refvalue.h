#ifndef _REFVALUE_H_
#define _REFVALUE_H_

#include "valuetype.h"

#include <cstdint>
#include <functional>
#include <memory>

struct struct_def;
struct struct_payload;
struct type_constraint;

/*
 * A GC-managed object created by `ref T { ... }`.  Handles are ordinary
 * T_userdata values; the object itself lives in a thread-local registry that
 * the existing cycle collector marks and sweeps.
 */
struct ref_object {
    uint64_t id = 0;
    uint64_t generation = 0;
    struct_def *def = nullptr;
    std::shared_ptr<struct_payload> payload;
    uint64_t gc_epoch = 0;
    bool swept = false;
};

const struct native_type *ref_native_type();
const struct native_type *weak_native_type();

bool is_ref_value(const struct value_t *value);
bool is_weak_value(const struct value_t *value);
bool is_ref_like_value(const struct value_t *value);
const char *ref_transfer_guidance();

struct ref_object *ref_target(struct value_t *value);
struct_def *ref_value_def(const struct value_t *value);

struct value_t *new_ref_value(struct_def *def,
                              const std::shared_ptr<struct_payload> &payload);
struct value_t *new_weak_from_ref(struct value_t *value);
struct value_t *ref_upgrade(struct value_t *value);
struct value_t *ref_snapshot(struct value_t *value);
/* Replaces the whole target payload with a value-semantic copy. */
bool ref_replace_target(struct value_t *ref, struct value_t *value);

/* Called by runtime type checks when a weak field receives a strong ref. */
struct value_t *coerce_to_ref_type(const struct type_constraint *type,
                                   struct value_t *value);

/* GC integration.  The collector stamps objects with its epoch and calls
 * `mark` for every ordinary value field reachable through a strong ref. */
void gc_mark_ref_value(struct value_t *value, uint64_t epoch,
                       const std::function<void(struct value_t *)> &mark);
void gc_sweep_unmarked_refs(uint64_t epoch);
void gc_note_ref_allocation();
bool gc_ref_allocation_due(std::size_t threshold);
void gc_reset_ref_allocation_counter();
/* Release any ref objects left alive at interpreter/thread shutdown. */
void ref_shutdown();

#endif
