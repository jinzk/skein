#ifndef _STRUCTEVAL_H_
#define _STRUCTEVAL_H_

#include "symbol.h"

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

#endif
