#ifndef SKEIN_EVAL_INTERNAL_H
#define SKEIN_EVAL_INTERNAL_H

#include "ast.h"
#include "diagnostics.h"
#include "eval_context.h"
#include "valuetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct type_constraint;

struct eval_location_guard {
    int old_line;
    int old_column;

    eval_location_guard(int line, int column)
        : old_line(g_error_line), old_column(g_error_column) {
        if (line > 0) {
            g_error_line = line;
            g_error_column = column > 0 ? column : old_column;
        }
    }

    ~eval_location_guard() {
        g_error_line = old_line;
        g_error_column = old_column;
    }
};

struct call_stack_guard {
    call_stack_guard(const std::string &name, int line, int column) {
        diag_push_call(name, line, column);
    }
    ~call_stack_guard() {
        diag_pop_call();
    }
};

void free_args(std::vector<struct value_t *> &args);

eval_context &eval_ctx();
const char *take_pending_label();
bool label_directly_applies(const struct ast *node);
bool control_matches_label(struct value_t *value, const char *label);
void set_eval_control(enum eval_control_kind kind, struct value_t *value);
struct value_t *take_eval_control(enum eval_control_kind kind);
bool switch_value_supported(const struct value_t *v);
bool case_values_have_condition(struct ast *values);

struct value_t *do_compare(int op, struct value_t *l, struct value_t *r);
struct value_t *apply_binary_value(int op, struct value_t *l,
                                   struct value_t *r);

const char *import_binding_name(const char *path);
struct value_t *callvalue(struct value_t *callee, struct ast *args_ast);
struct value_t *call_method_value(struct value_t *receiver,
                                  std::shared_ptr<function_def> def,
                                  struct ast *args_ast);

struct value_t *compound_name(struct symref *ref, int op,
                              struct ast *value_ast);
struct value_t *compound_index(struct ast *target, int op,
                               struct ast *value_ast);
struct value_t *assign_index(struct ast *target, struct ast *value_ast);
struct value_t *increment_index(struct ast *target, int delta, bool prefix);
struct value_t *read_member_value(struct value_t *container,
                                  struct value_t *index,
                                  bool field_access = false);
bool read_index_chain(struct ast *a, struct value_t *&out);
struct value_t *default_value_for_type(const struct type_constraint *type);
bool validate_function_params(const char *name, struct symlist *params);
struct value_t *assign_name(struct symref *ref, struct ast *value_ast);
struct value_t *increment_name(struct symref *ref, int delta, bool prefix);

/*
 * Value-based variants used by the bytecode VM.  They consume `value` and
 * return an owned result (or a control value, or NULL after reporting an
 * error), matching the corresponding AST helpers.
 */
struct value_t *assign_symbol_value(const char *name, struct value_t *value);
struct value_t *compound_symbol_value(const char *name, int op,
                                      struct value_t *value);
struct value_t *increment_symbol_value(const char *name, int delta,
                                       bool prefix);

/* Direct variants for cached symbol lookups in the VM. */
struct value_t *assign_symbol_direct(struct symbol *sym,
                                     struct value_t *value);
struct value_t *compound_symbol_direct(struct symbol *sym, int op,
                                       struct value_t *value);
struct value_t *increment_symbol_direct(struct symbol *sym, int delta,
                                        bool prefix);

/*
 * Value-based index helpers for the VM.  `indices` is consumed (freed) on
 * every path; `value` is consumed for assignment/compound assignment.
 */
struct value_t *assign_index_values(const char *base_name,
                                    std::vector<struct value_t *> &indices,
                                    const std::vector<bool> &fields,
                                    struct value_t *value);
struct value_t *compound_index_values(const char *base_name, int op,
                                      std::vector<struct value_t *> &indices,
                                      const std::vector<bool> &fields,
                                      struct value_t *value);
struct value_t *increment_index_values(const char *base_name, int delta,
                                       bool prefix,
                                       std::vector<struct value_t *> &indices,
                                       const std::vector<bool> &fields);

#endif
