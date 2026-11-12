#ifndef _STRUCTEVAL_H_
#define _STRUCTEVAL_H_

#include "symbol.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct scope_t;
struct upvalue_cell;
struct struct_node;
struct impl_node;
struct trait_node;
struct struct_lit_node;
struct value_t;
struct function_code;

struct field_meta {
    std::string name;
    type_constraint type;
    std::shared_ptr<function_code> default_code;
};

struct variant_meta {
    std::string name;
    std::vector<field_meta> fields;
};

bool validate_type_constraint(const struct type_constraint *type);
struct type_constraint any_type_constraint();
struct type_constraint copy_type_constraint(
    const struct type_constraint *type);

void eval_struct_decl(struct struct_node *s);
bool eval_impl_decl(struct impl_node *impl);
void eval_trait_decl(struct trait_node *t);
struct value_t *eval_struct_lit(struct struct_lit_node *lit);
void eval_enum_decl(struct enum_node *e);
struct value_t *eval_enum_lit(struct enum_lit_node *lit);

/*
 * Value-based constructors for the bytecode VM.  `fields` is consumed: the
 * helper takes ownership of every value and frees them on error.
 */
struct value_t *build_struct_value(
    const char *source, const char *name,
    std::vector<std::pair<std::string, struct value_t *>> &fields,
    const std::vector<bool> &spread);
struct value_t *build_enum_value(
    const char *source, const char *name, const char *variant,
    std::vector<std::pair<std::string, struct value_t *>> &fields,
    const std::vector<bool> &spread);

bool declare_struct_meta(
    const char *source, const char *package, const char *module, const char *name,
    bool exported, const std::vector<field_meta> &fields,
    const std::shared_ptr<struct scope_t> &closure, std::string &error);

bool declare_enum_meta(
    const char *source, const char *package, const char *module, const char *name,
    bool exported, const std::vector<variant_meta> &variants,
    const std::shared_ptr<struct scope_t> &closure, std::string &error);

struct trait_param_meta {
    std::string name;
    type_constraint type;
    bool variadic = false;
    bool has_default = false;
};

struct trait_method_meta {
    std::string name;
    std::vector<trait_param_meta> params;
    std::shared_ptr<struct function_code> code;
};

bool declare_trait_meta(
    const char *source, const char *package, const char *module, const char *name,
    bool exported, const std::vector<trait_method_meta> &methods,
    const std::shared_ptr<struct scope_t> &closure, std::string &error,
    const std::function<std::shared_ptr<struct upvalue_cell>(
        const std::string &)> &capture_upvalue);

bool declare_impl_meta(
    const char *source, const char *package, const char *type_name,
    bool has_trait, const char *trait_name,
    const std::vector<std::pair<
        std::string, std::shared_ptr<struct function_code>>> &methods,
    const std::shared_ptr<struct scope_t> &closure, std::string &error,
    const std::function<std::shared_ptr<struct upvalue_cell>(
        const std::string &)> &capture_upvalue = {});

#endif
