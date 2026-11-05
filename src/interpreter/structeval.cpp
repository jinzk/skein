#include "structeval.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval.h"
#include "structtype.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

bool validate_type_constraint(const struct type_constraint *type) {
    if (!type) return true;
    if (type->kind == TYPE_NAMED) {
        if (!type->name ||
            !structtype_type_exists(type->module, type->name)) {
            yyerror("unknown type name %s",
                    type->name ? type->name : "invalid");
            return false;
        }
        return true;
    }
    if (type->kind == TYPE_INVALID) {
        yyerror("invalid type");
        return false;
    }
    return true;
}

struct type_constraint any_type_constraint() {
    struct type_constraint type;
    type.kind = TYPE_ANY;
    type.name = NULL;
    type.module = NULL;
    type.nullable = false;
    return type;
}

struct type_constraint copy_type_constraint(
    const struct type_constraint *type) {
    if (!type) return any_type_constraint();
    return *type;
}

static bool method_self_ok(struct symlist *sl, const char *owner,
                           std::string &error) {
    if (!sl || !sl->sym) {
        error = "method requires self";
        return false;
    }
    if (std::strcmp(sl->sym->name, "self") != 0 || sl->variadic) {
        error = "method first parameter must be self";
        return false;
    }
    if (sl->has_default) {
        error = "method self cannot have a default value";
        return false;
    }
    if (sl->type && sl->type->kind != TYPE_ANY) {
        if (sl->type->kind != TYPE_NAMED || !owner ||
            !sl->type->name || std::strcmp(sl->type->name, owner) != 0) {
            error = "method self has an invalid type";
            return false;
        }
    }
    return true;
}

static bool evaluate_method(struct struct_member *member, type_def *owner,
                            std::shared_ptr<function_def> &out,
                            std::string &error) {
    out.reset();
    struct value_t *value = eval(member->func);
    if (!value)
        return false;
    if (value->type != T_func) {
        error = "invalid method function";
        freeval(value);
        return false;
    }
    struct func_value_t *fv = (struct func_value_t *)value;
    out = fv->def;
    freeval(value);
    if (!out || !out->code) {
        error = "invalid method function";
        out.reset();
        return false;
    }
    if (!method_self_ok(out->code->sl, owner ? owner->name.c_str() : NULL,
                        error)) {
        out.reset();
        return false;
    }
    out->code->is_method = true;
    out->code->name = member->name;
    return true;
}

static const struct method_sig *find_trait_method(const trait_def *trait,
                                                  const char *name) {
    if (!trait || !name) return NULL;
    for (size_t i = 0; i < trait->methods.size(); ++i)
        if (trait->methods[i].name == name)
            return &trait->methods[i];
    return NULL;
}

static bool method_signature_matches(const method_sig *sig,
                                     const struct function_code *code,
                                     std::string &error) {
    if (!sig || !code) {
        error = "invalid method signature";
        return false;
    }
    if (code->variadic) {
        error = "trait methods cannot be variadic";
        return false;
    }
    if ((int)sig->params.size() != code->nparams) {
        error = "method " + sig->name + " has the wrong number of parameters";
        return false;
    }
    int i = 0;
    for (struct symlist *sl = code->sl; sl; sl = sl->next, ++i) {
        struct type_constraint actual =
            sl->type ? *sl->type : any_type_constraint();
        struct type_constraint expected = sig->params[(size_t)i];
        /* The receiver type is the implementing type; do not compare it. */
        if (i == 0)
            continue;
        if (!type_constraint_equal(&actual, &expected)) {
            error = "method " + sig->name +
                    " does not match the trait signature";
            return false;
        }
    }
    return true;
}

void eval_struct_decl(struct struct_node *s) {
    for (struct struct_member *m = s->members; m; m = m->next) {
        if (m->nodetype == 'm') {
            yyerror("methods must be declared in impl");
            return;
        }
    }

    std::string error;
    struct_def *def = NULL;
    if (!structtype_prepare_struct(
            s->module, diag_current_module_name().c_str(), s->name,
            s->exported != 0, def, error)) {
        yyerror("%s", error.c_str());
        return;
    }

    std::vector<field_def> fields;
    for (struct struct_member *m = s->members; m; m = m->next) {
        if (m->nodetype != 'f')
            continue;
        if (!validate_type_constraint(m->type)) {
            structtype_rollback_struct(def);
            return;
        }
        field_def field;
        field.name = m->name;
        field.type = copy_type_constraint(m->type);
        fields.push_back(field);
    }

    if (!structtype_finalize_struct(def, std::move(fields), error)) {
        yyerror("%s", error.c_str());
        structtype_rollback_struct(def);
        return;
    }
}

bool eval_impl_decl(struct impl_node *impl) {
    type_def *owner = structtype_find_type(impl->module, impl->type_name);
    if (!owner) {
        yyerror("unknown type %s", impl->type_name);
        return false;
    }

    trait_def *trait = NULL;
    if (impl->trait_name) {
        std::string trait_error;
        trait = structtype_resolve_trait(impl->module, impl->trait_name,
                                         true, trait_error);
        if (!trait) {
            yyerror("%s", trait_error.c_str());
            return false;
        }
    }

    bool ok = true;
    for (struct struct_member *m = impl->methods; m; m = m->next) {
        if (m->nodetype != 'm')
            continue;
        if (trait) {
            const struct method_sig *sig = find_trait_method(trait, m->name);
            if (!sig) {
                yyerror("method %s is not declared by trait %s", m->name,
                        trait->name.c_str());
                ok = false;
                continue;
            }
            std::shared_ptr<function_def> code;
            std::string error;
            if (!evaluate_method(m, owner, code, error)) {
                if (!error.empty())
                    yyerror("%s", error.c_str());
                ok = false;
                continue;
            }
            if (!method_signature_matches(sig, code->code.get(), error)) {
                yyerror("%s", error.c_str());
                ok = false;
                continue;
            }
            if (!structtype_add_method(owner, m->name, code, error)) {
                yyerror("%s", error.c_str());
                ok = false;
            }
        } else {
            std::shared_ptr<function_def> code;
            std::string error;
            if (!evaluate_method(m, owner, code, error)) {
                if (!error.empty())
                    yyerror("%s", error.c_str());
                ok = false;
                continue;
            }
            if (!structtype_add_method(owner, m->name, code, error)) {
                yyerror("%s", error.c_str());
                ok = false;
            }
        }
    }

    if (!trait)
        return ok;

    for (size_t i = 0; i < trait->methods.size(); ++i) {
        auto method = owner->methods.find(trait->methods[i].name);
        if (method == owner->methods.end()) {
            yyerror("type %s does not implement method %s required by trait %s",
                    owner->name.c_str(), trait->methods[i].name.c_str(),
                    trait->name.c_str());
            ok = false;
            continue;
        }
        std::string error;
        if (method->second && method->second->code &&
            !method_signature_matches(&trait->methods[i],
                                      method->second->code.get(), error)) {
            yyerror("%s", error.c_str());
            ok = false;
        }
    }

    if (ok) {
        std::string error;
        if (!structtype_add_trait(owner, trait, error)) {
            yyerror("%s", error.c_str());
            ok = false;
        }
    }
    return ok;
}

void eval_trait_decl(struct trait_node *t) {
    std::vector<method_sig> methods;
    for (struct trait_method *m = t->methods; m; m = m->next) {
        if (!m->params || std::strcmp(m->params->sym->name, "self") != 0) {
            yyerror("trait method %s must start with self", m->name);
            return;
        }
        if (m->params->type && m->params->type->kind != TYPE_ANY) {
            yyerror("trait method %s self must be untyped", m->name);
            return;
        }
        method_sig sig;
        sig.name = m->name;
        for (struct symlist *p = m->params; p; p = p->next) {
            if (p->variadic) {
                yyerror("trait methods cannot be variadic");
                return;
            }
            if (p->has_default) {
                yyerror("trait method %s cannot have default parameter values",
                        m->name);
                return;
            }
            if (!validate_type_constraint(p->type))
                return;
            sig.params.push_back(copy_type_constraint(p->type));
        }
        methods.push_back(sig);
    }

    std::string error;
    trait_def *def = NULL;
    if (!structtype_create_trait(
            t->module, diag_current_module_name().c_str(), t->name,
            t->exported != 0, std::move(methods), def, error))
        yyerror("%s", error.c_str());
}

bool declare_struct_meta(
    const char *module, const char *module_name, const char *name,
    bool exported,
    const std::vector<std::pair<std::string, type_constraint>> &fields,
    std::string &error) {
    struct_def *def = NULL;
    if (!structtype_prepare_struct(module, module_name, name, exported, def,
                                   error)) {
        yyerror("%s", error.c_str());
        return false;
    }

    std::vector<field_def> declared_fields;
    for (const auto &field : fields) {
        if (!validate_type_constraint(&field.second)) {
            structtype_rollback_struct(def);
            return false;
        }
        field_def declared;
        declared.name = field.first;
        declared.type = copy_type_constraint(&field.second);
        declared_fields.push_back(std::move(declared));
    }

    if (!structtype_finalize_struct(def, std::move(declared_fields),
                                    error)) {
        yyerror("%s", error.c_str());
        structtype_rollback_struct(def);
        return false;
    }
    return true;
}

bool declare_enum_meta(
    const char *module, const char *module_name, const char *name,
    bool exported,
    const std::vector<std::pair<
        std::string,
        std::vector<std::pair<std::string, type_constraint>>>> &variants,
    std::string &error) {
    enum_def *def = NULL;
    if (!structtype_prepare_enum(module, module_name, name, exported, def,
                                 error)) {
        yyerror("%s", error.c_str());
        return false;
    }

    std::vector<variant_def> declared_variants;
    for (const auto &variant : variants) {
        variant_def declared;
        declared.name = variant.first;
        for (const auto &field : variant.second) {
            if (!validate_type_constraint(&field.second)) {
                structtype_rollback_enum(def);
                return false;
            }
            field_def declared_field;
            declared_field.name = field.first;
            declared_field.type = copy_type_constraint(&field.second);
            declared.fields.push_back(std::move(declared_field));
        }
        declared_variants.push_back(std::move(declared));
    }

    if (!structtype_finalize_enum(def, std::move(declared_variants),
                                  error)) {
        yyerror("%s", error.c_str());
        structtype_rollback_enum(def);
        return false;
    }
    return true;
}

bool declare_trait_meta(
    const char *module, const char *module_name, const char *name,
    bool exported, const std::vector<trait_method_meta> &methods,
    std::string &error) {
    std::vector<method_sig> signatures;
    for (const trait_method_meta &method : methods) {
        if (method.params.empty() ||
            method.params.front().name != "self") {
            yyerror("trait method %s must start with self",
                    method.name.c_str());
            return false;
        }
        if (method.params.front().type.kind != TYPE_ANY) {
            yyerror("trait method %s self must be untyped",
                    method.name.c_str());
            return false;
        }
        method_sig signature;
        signature.name = method.name;
        for (const trait_param_meta &param : method.params) {
            if (param.variadic) {
                yyerror("trait methods cannot be variadic");
                return false;
            }
            if (param.has_default) {
                yyerror("trait method %s cannot have default parameter "
                        "values", method.name.c_str());
                return false;
            }
            if (!validate_type_constraint(&param.type))
                return false;
            signature.params.push_back(
                copy_type_constraint(&param.type));
        }
        signatures.push_back(std::move(signature));
    }

    trait_def *def = NULL;
    if (!structtype_create_trait(module, module_name, name, exported,
                                 std::move(signatures), def, error)) {
        yyerror("%s", error.c_str());
        return false;
    }
    return true;
}

bool declare_impl_meta(
    const char *module, const char *module_name, const char *type_name,
    bool has_trait, const char *trait_name,
    const std::vector<
        std::pair<std::string, std::shared_ptr<function_code>>> &methods,
    const std::shared_ptr<scope_t> &closure, std::string &error) {
    (void)module_name;
    (void)error;
    type_def *owner = structtype_find_type(module, type_name);
    if (!owner) {
        yyerror("unknown type %s", type_name ? type_name : "");
        return false;
    }

    trait_def *trait = NULL;
    if (has_trait) {
        std::string trait_error;
        trait = structtype_resolve_trait(module, trait_name, true,
                                         trait_error);
        if (!trait) {
            yyerror("%s", trait_error.c_str());
            return false;
        }
    }

    bool ok = true;
    for (const auto &method : methods) {
        std::shared_ptr<function_def> definition =
            std::make_shared<function_def>();
        definition->code = method.second;
        definition->closure = closure;
        std::string method_error;
        if (!method.second ||
            !method_self_ok(method.second->sl, owner->name.c_str(),
                            method_error)) {
            yyerror("%s", method_error.c_str());
            ok = false;
            continue;
        }
        method.second->is_method = true;
        method.second->name = method.first;

        if (trait) {
            const method_sig *sig =
                find_trait_method(trait, method.first.c_str());
            if (!sig) {
                yyerror("method %s is not declared by trait %s",
                        method.first.c_str(), trait->name.c_str());
                ok = false;
                continue;
            }
            if (!method_signature_matches(sig, method.second.get(),
                                          method_error)) {
                yyerror("%s", method_error.c_str());
                ok = false;
                continue;
            }
        }
        if (!structtype_add_method(owner, method.first, definition,
                                   method_error)) {
            yyerror("%s", method_error.c_str());
            ok = false;
        }
    }

    if (!trait)
        return ok;

    for (size_t i = 0; i < trait->methods.size(); ++i) {
        auto method = owner->methods.find(trait->methods[i].name);
        if (method == owner->methods.end()) {
            yyerror("type %s does not implement method %s required by "
                    "trait %s", owner->name.c_str(),
                    trait->methods[i].name.c_str(),
                    trait->name.c_str());
            ok = false;
            continue;
        }
        std::string signature_error;
        if (method->second && method->second->code &&
            !method_signature_matches(&trait->methods[i],
                                      method->second->code.get(),
                                      signature_error)) {
            yyerror("%s", signature_error.c_str());
            ok = false;
        }
    }

    if (ok) {
        std::string trait_error;
        if (!structtype_add_trait(owner, trait, trait_error)) {
            yyerror("%s", trait_error.c_str());
            ok = false;
        }
    }
    return ok;
}

struct value_t *eval_struct_lit(struct struct_lit_node *lit) {
    struct_def *def = structtype_find_struct(lit->module, lit->name);
    if (!def) {
        yyerror("unknown struct type %s", lit->name);
        return NULL;
    }

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(def->fields.size(), NULL);
    std::vector<bool> seen(def->fields.size(), false);
    bool ok = true;

    for (struct struct_init *init = lit->fields; init; init = init->next) {
        int index = structtype_field_index(def, init->name);
        if (index < 0) {
            yyerror("unknown field %s in %s", init->name, def->name.c_str());
            ok = false;
            continue;
        }
        if (seen[(size_t)index]) {
            yyerror("field %s repeated in %s", init->name, def->name.c_str());
            ok = false;
            continue;
        }
        struct value_t *value = eval(init->value);
        if (!value) {
            ok = false;
            continue;
        }
        if (!type_constraint_matches(&def->fields[(size_t)index].type,
                                     value)) {
            yyerror("field %s of %s expects %s, got %s", init->name,
                    def->name.c_str(),
                    type_constraint_name(&def->fields[(size_t)index].type),
                    typename_of(value));
            freeval(value);
            ok = false;
            continue;
        }
        payload->fields[(size_t)index] = value;
        seen[(size_t)index] = true;
    }

    for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            yyerror("field %s of %s is required", def->fields[i].name.c_str(),
                    def->name.c_str());
            ok = false;
        }
    }

    if (!ok)
        return NULL;
    return newstructval(def, payload);
}

static void free_constructed_fields(
    std::vector<std::pair<std::string, value_t *>> &fields) {
    for (auto &field : fields)
        freeval(field.second);
    fields.clear();
}

value_t *build_struct_value(
    const char *module, const char *name,
    std::vector<std::pair<std::string, value_t *>> &fields) {
    struct_def *def = structtype_find_struct(module, name);
    if (!def) {
        yyerror("unknown struct type %s", name ? name : "");
        free_constructed_fields(fields);
        return NULL;
    }

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(def->fields.size(), NULL);
    std::vector<bool> seen(def->fields.size(), false);
    bool ok = true;

    for (auto &field : fields) {
        int index = structtype_field_index(def, field.first.c_str());
        if (index < 0) {
            yyerror("unknown field %s in %s", field.first.c_str(),
                    def->name.c_str());
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        if (seen[(size_t)index]) {
            yyerror("field %s repeated in %s", field.first.c_str(),
                    def->name.c_str());
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        if (!type_constraint_matches(&def->fields[(size_t)index].type,
                                     field.second)) {
            yyerror("field %s of %s expects %s, got %s",
                    field.first.c_str(), def->name.c_str(),
                    type_constraint_name(
                        &def->fields[(size_t)index].type),
                    typename_of(field.second));
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        payload->fields[(size_t)index] = field.second;
        field.second = NULL;
        seen[(size_t)index] = true;
    }

    for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            yyerror("field %s of %s is required",
                    def->fields[i].name.c_str(), def->name.c_str());
            ok = false;
        }
    }

    if (!ok) {
        free_constructed_fields(fields);
        return NULL;
    }
    fields.clear();
    return newstructval(def, payload);
}

value_t *build_enum_value(
    const char *module, const char *name, const char *variant_name,
    std::vector<std::pair<std::string, value_t *>> &fields) {
    enum_def *def = structtype_find_enum(module, name);
    if (!def) {
        yyerror("unknown enum type %s", name ? name : "");
        free_constructed_fields(fields);
        return NULL;
    }

    int variant_index =
        structtype_variant_index(def, variant_name ? variant_name : "");
    if (variant_index < 0) {
        yyerror("unknown enum variant %s::%s", def->name.c_str(),
                variant_name ? variant_name : "");
        free_constructed_fields(fields);
        return NULL;
    }
    const variant_def &variant =
        def->variants[(size_t)variant_index];

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(variant.fields.size(), NULL);
    std::vector<bool> seen(variant.fields.size(), false);
    bool ok = true;

    for (auto &field : fields) {
        int index =
            structtype_variant_field_index(&variant, field.first.c_str());
        if (index < 0) {
            yyerror("unknown field %s in %s::%s", field.first.c_str(),
                    def->name.c_str(), variant.name.c_str());
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        if (seen[(size_t)index]) {
            yyerror("field %s repeated in %s::%s", field.first.c_str(),
                    def->name.c_str(), variant.name.c_str());
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        if (!type_constraint_matches(&variant.fields[(size_t)index].type,
                                     field.second)) {
            yyerror("field %s of %s::%s expects %s, got %s",
                    field.first.c_str(), def->name.c_str(),
                    variant.name.c_str(),
                    type_constraint_name(
                        &variant.fields[(size_t)index].type),
                    typename_of(field.second));
            freeval(field.second);
            field.second = NULL;
            ok = false;
            continue;
        }
        payload->fields[(size_t)index] = field.second;
        field.second = NULL;
        seen[(size_t)index] = true;
    }

    for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            yyerror("field %s of %s::%s is required",
                    variant.fields[i].name.c_str(), def->name.c_str(),
                    variant.name.c_str());
            ok = false;
        }
    }

    if (!ok) {
        free_constructed_fields(fields);
        return NULL;
    }
    fields.clear();
    return newenumval(def, variant_index, payload);
}

void eval_enum_decl(struct enum_node *e) {
    std::string error;
    enum_def *def = NULL;
    if (!structtype_prepare_enum(
            e->module, diag_current_module_name().c_str(), e->name,
            e->exported != 0, def, error)) {
        yyerror("%s", error.c_str());
        return;
    }

    std::vector<variant_def> variants;
    for (struct enum_variant *v = e->variants; v; v = v->next) {
        variant_def variant;
        variant.name = v->name;
        for (struct struct_member *m = v->fields; m; m = m->next) {
            if (m->nodetype != 'f')
                continue;
            if (!validate_type_constraint(m->type)) {
                structtype_rollback_enum(def);
                return;
            }
            field_def field;
            field.name = m->name;
            field.type = copy_type_constraint(m->type);
            variant.fields.push_back(std::move(field));
        }
        variants.push_back(std::move(variant));
    }

    if (!structtype_finalize_enum(def, std::move(variants), error)) {
        yyerror("%s", error.c_str());
        structtype_rollback_enum(def);
    }
}

struct value_t *eval_enum_lit(struct enum_lit_node *lit) {
    enum_def *def = structtype_find_enum(lit->module, lit->name);
    if (!def) {
        yyerror("unknown enum type %s", lit->name);
        return NULL;
    }

    int variant_index = structtype_variant_index(def, lit->variant);
    if (variant_index < 0) {
        yyerror("unknown enum variant %s::%s", def->name.c_str(),
                lit->variant);
        return NULL;
    }
    const variant_def &variant = def->variants[(size_t)variant_index];

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(variant.fields.size(), NULL);
    std::vector<bool> seen(variant.fields.size(), false);
    bool ok = true;

    for (struct struct_init *init = lit->fields; init; init = init->next) {
        int index = structtype_variant_field_index(&variant, init->name);
        if (index < 0) {
            yyerror("unknown field %s in %s::%s", init->name,
                    def->name.c_str(), variant.name.c_str());
            ok = false;
            continue;
        }
        if (seen[(size_t)index]) {
            yyerror("field %s repeated in %s::%s", init->name,
                    def->name.c_str(), variant.name.c_str());
            ok = false;
            continue;
        }
        struct value_t *value = eval(init->value);
        if (!value) {
            ok = false;
            continue;
        }
        if (!type_constraint_matches(&variant.fields[(size_t)index].type,
                                     value)) {
            yyerror("field %s of %s::%s expects %s, got %s", init->name,
                    def->name.c_str(), variant.name.c_str(),
                    type_constraint_name(
                        &variant.fields[(size_t)index].type),
                    typename_of(value));
            freeval(value);
            ok = false;
            continue;
        }
        payload->fields[(size_t)index] = value;
        seen[(size_t)index] = true;
    }

    if (!ok)
        return NULL;

    for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            yyerror("field %s of %s::%s is required",
                    variant.fields[i].name.c_str(), def->name.c_str(),
                    variant.name.c_str());
            ok = false;
        }
    }

    if (!ok)
        return NULL;
    return newenumval(def, variant_index, payload);
}
