#include "vm_eval.h"

#include "control_runtime.h"
#include "eval_context.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"
#include "value_runtime.h"

#include <memory>
#include <vector>

void skein_vm_eval_capture_begin(struct eval_capture_snapshot &snapshot) {
    eval_capture_begin(snapshot);
}

struct eval_capture_result skein_vm_eval_capture_end(
    const struct eval_capture_snapshot &snapshot) {
    return eval_capture_end(snapshot);
}

void skein_vm_eval_raise(struct value_t *error) {
    eval_raise(error);
}

struct value_t *skein_vm_take_eval_control(enum eval_control_kind kind) {
    return take_eval_control(kind);
}

void skein_vm_eval_pushscope() {
    pushscope();
}

void skein_vm_eval_push_isolated_scope() {
    pushscope_isolated();
}

void skein_vm_eval_push_closure_scope(
    const std::shared_ptr<struct scope_t> &parent) {
    pushscope_closure(parent);
}

void skein_vm_eval_popscope() {
    popscope();
}

struct value_t *skein_vm_read_member_value(
    struct value_t *container, struct value_t *index, bool field_access) {
    return read_member_value(container, index, field_access);
}

struct value_t *skein_vm_assign_symbol_value(
    const char *name, struct value_t *value) {
    return assign_symbol_value(name, value);
}

struct value_t *skein_vm_compound_symbol_value(
    const char *name, int op, struct value_t *value) {
    return compound_symbol_value(name, op, value);
}

struct value_t *skein_vm_increment_symbol_value(
    const char *name, int delta, bool prefix) {
    return increment_symbol_value(name, delta, prefix);
}

struct value_t *skein_vm_assign_symbol_direct(
    struct symbol *sym, struct value_t *value) {
    return assign_symbol_direct(sym, value);
}

struct value_t *skein_vm_compound_symbol_direct(
    struct symbol *sym, int op, struct value_t *value) {
    return compound_symbol_direct(sym, op, value);
}

struct value_t *skein_vm_increment_symbol_direct(
    struct symbol *sym, int delta, bool prefix) {
    return increment_symbol_direct(sym, delta, prefix);
}

struct value_t *skein_vm_assign_index_symbol(
    struct symbol *base, const char *base_name,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value) {
    return assign_index_symbol(base, base_name, indices, fields, value);
}

struct value_t *skein_vm_compound_index_symbol(
    struct symbol *base, const char *base_name, int op,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value) {
    return compound_index_symbol(base, base_name, op, indices, fields,
                                 value);
}

struct value_t *skein_vm_increment_index_symbol(
    struct symbol *base, const char *base_name, int delta, bool prefix,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields) {
    return increment_index_symbol(base, base_name, delta, prefix, indices,
                                  fields);
}

struct value_t *skein_vm_default_value_for_type(
    const struct type_constraint *type) {
    return default_value_for_type(type);
}

bool skein_vm_switch_value_supported(const struct value_t *value) {
    return switch_value_supported(value);
}

struct value_t *skein_vm_do_compare(
    int op, struct value_t *left, struct value_t *right) {
    return do_compare(op, left, right);
}

struct value_t *skein_vm_apply_binary_value(
    int op, struct value_t *left, struct value_t *right) {
    return apply_binary_value(op, left, right);
}

const char *skein_vm_import_binding_name(const char *path) {
    return import_binding_name(path);
}

bool skein_vm_validate_function_params(
    const char *name, struct symlist *params) {
    return validate_function_params(name, params);
}
