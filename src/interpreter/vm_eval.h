#ifndef SKEIN_VM_EVAL_H
#define SKEIN_VM_EVAL_H

#include "eval_context.h"
#include "valuetype.h"

#include <memory>
#include <string>
#include <vector>

struct scope_t;
struct symlist;

/*
 * Narrow evaluator interface used by the bytecode executor.  These wrappers
 * are implemented by vm_bridge.cpp, which is the only VM file that directly
 * include the AST/evaluator headers.
 */

struct eval_capture_snapshot {
    bool capture_errors = false;
    bool has_captured_error = false;
    std::string captured_error;
    int runtime_error_count = 0;
};

struct eval_capture_result {
    bool failed = false;
    struct value_t *error = NULL;
};

void skein_vm_eval_capture_begin(struct eval_capture_snapshot &snapshot);
struct eval_capture_result skein_vm_eval_capture_end(
    const struct eval_capture_snapshot &snapshot);
void skein_vm_eval_raise(struct value_t *error);
void skein_vm_eval_pushscope();
void skein_vm_eval_push_isolated_scope();
void skein_vm_eval_push_closure_scope(
    const std::shared_ptr<struct scope_t> &parent);
void skein_vm_eval_popscope();

struct value_t *skein_vm_call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args);
struct value_t *skein_vm_read_member_value(
    struct value_t *container, struct value_t *index,
    bool field_access = false);

struct value_t *skein_vm_assign_symbol_value(
    const char *name, struct value_t *value);
struct value_t *skein_vm_compound_symbol_value(
    const char *name, int op, struct value_t *value);
struct value_t *skein_vm_increment_symbol_value(
    const char *name, int delta, bool prefix);
struct value_t *skein_vm_assign_symbol_direct(
    struct symbol *sym, struct value_t *value);
struct value_t *skein_vm_compound_symbol_direct(
    struct symbol *sym, int op, struct value_t *value);
struct value_t *skein_vm_increment_symbol_direct(
    struct symbol *sym, int delta, bool prefix);

struct value_t *skein_vm_assign_index_symbol(
    struct symbol *base, const char *base_name,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value);
struct value_t *skein_vm_compound_index_symbol(
    struct symbol *base, const char *base_name, int op,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value);
struct value_t *skein_vm_increment_index_symbol(
    struct symbol *base, const char *base_name, int delta, bool prefix,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields);

struct value_t *skein_vm_take_eval_control(enum eval_control_kind kind);
bool skein_vm_switch_value_supported(const struct value_t *value);
struct value_t *skein_vm_do_compare(
    int op, struct value_t *left, struct value_t *right);
struct value_t *skein_vm_apply_binary_value(
    int op, struct value_t *left, struct value_t *right);
const char *skein_vm_import_binding_name(const char *path);

struct value_t *skein_vm_default_value_for_type(
    const struct type_constraint *type);
bool skein_vm_validate_function_params(
    const char *name, struct symlist *params);

#endif
