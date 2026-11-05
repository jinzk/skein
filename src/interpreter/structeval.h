#ifndef _STRUCTEVAL_H_
#define _STRUCTEVAL_H_

#include "symbol.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct scope_t;
struct struct_node;
struct impl_node;
struct trait_node;
struct struct_lit_node;
struct value_t;

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
    const char *module, const char *name,
    std::vector<std::pair<std::string, struct value_t *>> &fields);
struct value_t *build_enum_value(
    const char *module, const char *name, const char *variant,
    std::vector<std::pair<std::string, struct value_t *>> &fields);

bool declare_struct_meta(
    const char *module, const char *module_name, const char *name,
    bool exported,
    const std::vector<std::pair<std::string, struct type_constraint>>
        &fields,
    const std::vector<std::pair<
        std::string, std::shared_ptr<struct function_code>>> &methods,
    const std::shared_ptr<struct scope_t> &closure, std::string &error);

bool declare_enum_meta(
    const char *module, const char *module_name, const char *name,
    bool exported,
    const std::vector<std::pair<
        std::string,
        std::vector<std::pair<std::string, struct type_constraint>>>>
        &variants,
    std::string &error);

struct trait_param_meta {
    std::string name;
    type_constraint type;
    bool variadic = false;
    bool has_default = false;
};

struct trait_method_meta {
    std::string name;
    std::vector<trait_param_meta> params;
};

bool declare_trait_meta(
    const char *module, const char *module_name, const char *name,
    bool exported, const std::vector<trait_method_meta> &methods,
    std::string &error);

bool declare_impl_meta(
    const char *module, const char *module_name, const char *type_name,
    bool has_trait, const char *trait_name,
    const std::vector<std::pair<
        std::string, std::shared_ptr<struct function_code>>> &methods,
    const std::shared_ptr<struct scope_t> &closure, std::string &error);

#endif
