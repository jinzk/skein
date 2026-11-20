#ifndef SKEIN_VALUE_RUNTIME_H
#define SKEIN_VALUE_RUNTIME_H

#include "symbol.h"
#include "valuetype.h"

#include <string>
#include <vector>

struct type_constraint;

struct value_t *read_member_value(struct value_t *container,
                                  struct value_t *index,
                                  bool field_access = false);
struct value_t *default_value_for_type(const struct type_constraint *type);
struct value_t *assign_symbol_value(const char *name,
                                    struct value_t *value);struct value_t *compound_symbol_value(const char *name, int op,
                                      struct value_t *value);
struct value_t *increment_symbol_value(const char *name, int delta,
                                       bool prefix);
struct value_t *assign_symbol_direct(struct symbol *sym,
                                     struct value_t *value);
struct value_t *compound_symbol_direct(struct symbol *sym, int op,
                                       struct value_t *value);
struct value_t *increment_symbol_direct(struct symbol *sym, int delta,
                                        bool prefix);
struct value_t *assign_index_symbol(
    struct symbol *base, const char *base_name,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value);
struct value_t *compound_index_symbol(
    struct symbol *base, const char *base_name, int op,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value);
struct value_t *increment_index_symbol(
    struct symbol *base, const char *base_name, int delta, bool prefix,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields);

struct value_t *do_compare(int op, struct value_t *left,
                           struct value_t *right);
struct value_t *apply_binary_value(int op, struct value_t *left,
                                   struct value_t *right);
struct value_t *new_int_checked(__int128 n);

bool validate_type_constraint(const struct type_constraint *type);

/*
 * Upvalue cell access shared by the VM and the JIT.  Cells either point at a
 * live frame slot (`slot`) or own a closed value (`value`).
 */
struct value_t *upvalue_cell_value(const struct upvalue_cell *cell);
struct value_t *upvalue_load(const struct upvalue_cell *cell);
struct value_t *upvalue_store(struct upvalue_cell *cell,
                              struct value_t *value);
struct value_t *upvalue_compound(struct upvalue_cell *cell, int op,
                                 struct value_t *value);
struct value_t *upvalue_incdec(struct upvalue_cell *cell, int delta,
                               bool prefix);
/* Detach an open cell from its frame slot, moving the value into the cell. */
void close_upvalue_cell(struct upvalue_cell &cell);

bool switch_value_supported(const struct value_t *value);
bool validate_function_params(const char *name, struct symlist *params);
const char *import_binding_name(const char *path);

#endif
