#include "structeval.h"

#include "ast.h"
#include "compiler.h"
#include "diagnostics.h"
#include "eval.h"
#include "scopestack.h"
#include "structtype.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

static std::shared_ptr<function_def> field_default_from_code(
    const std::shared_ptr<function_code> &code,
    const std::shared_ptr<scope_t> &closure) {
    if (!code)
        return NULL;
    std::shared_ptr<function_def> definition =
        std::make_shared<function_def>();
    definition->code = code;
    definition->closure = closure;
    if (definition->closure && definition->closure->is_global)
        definition->closure.reset();
    return definition;
}

static std::shared_ptr<function_def> make_field_default(
    struct ast *expr, const char *owner, const char *field,
    const std::shared_ptr<scope_t> &closure) {
    if (!expr)
        return NULL;

    std::shared_ptr<function_code> code(
        new function_code(), free_function_code);
    code->sl = NULL;
    code->nparams = 0;
    code->required_params = 0;
    code->name = std::string("<default ") +
                 (owner ? owner : "?") + "." +
                 (field ? field : "?") + ">";
    code->is_method = false;
    code->variadic = false;
    code->is_async = false;
    code->is_generator = false;
    code->def_line = expr->line;
    code->def_column = expr->column;
    code->module = diag_current_module_name();
    code->source = diag_current_source_name();
    set_function_body_ast(code.get(), newret(expr));
    return field_default_from_code(code, closure);
}

static bool check_field_default_names(
    struct ast *expr, const char *owner, const char *field,
    const std::vector<std::string> &forbidden) {
    if (!expr || forbidden.empty())
        return true;

    struct function_code code {};
    code.sl = NULL;
    code.nparams = 0;
    code.required_params = 0;
    code.name = std::string("<default ") +
                (owner ? owner : "?") + "." +
                (field ? field : "?") + ">";
    code.module = diag_current_module_name();

    std::string error;
    if (!skein_check_default_references(&code, expr, forbidden,
                                        code.module, error)) {
        yyerror("field %s default: %s",
                field ? field : "?", error.c_str());
        return false;
    }
    return true;
}

static struct value_t *eval_field_default(
    const field_def &field, const char *owner, const char *field_name) {    if (!field.default_value || !field.default_value->code) {
        yyerror("field %s of %s has no default value",
                field_name ? field_name : "?",
                owner ? owner : "?");
        return NULL;
    }

    struct func_value_t callee;
    callee.type = T_func;
    callee.def = field.default_value;
    struct value_t *value =
        call_value_args((struct value_t *)&callee, {});
    if (!value)
        return NULL;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue) {
        yyerror("default value for field %s of %s cannot return control flow",
                field_name ? field_name : "?",
                owner ? owner : "?");
        freeval(value);
        return NULL;
    }
    return value;
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
    if (sig->is_async != code->is_async) {
        error = "method " + sig->name + " async marker does not match " +
                "the trait signature";
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

    std::vector<std::string> forbidden;
    forbidden.push_back("self");
    for (struct struct_member *m = s->members; m; m = m->next) {
        if (m->nodetype == 'f' && m->name)
            forbidden.push_back(m->name);
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
        if (m->default_value) {
            if (!check_field_default_names(m->default_value, s->name,
                                           m->name, forbidden)) {
                structtype_rollback_struct(def);
                return;
            }
            field.has_default = true;
            field.default_value = make_field_default(
                m->default_value, s->name, m->name, current_scope());
            m->default_value = NULL;
        }
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
        sig.is_async = m->is_async != 0;
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
    bool exported, const std::vector<field_meta> &fields,
    const std::shared_ptr<scope_t> &closure, std::string &error) {
    struct_def *def = NULL;
    if (!structtype_prepare_struct(module, module_name, name, exported, def,
                                   error)) {
        yyerror("%s", error.c_str());
        return false;
    }

    std::vector<field_def> declared_fields;
    for (const auto &field : fields) {
        if (!validate_type_constraint(&field.type)) {
            structtype_rollback_struct(def);
            return false;
        }
        field_def declared;
        declared.name = field.name;
        declared.type = copy_type_constraint(&field.type);
        if (field.default_code) {
            declared.has_default = true;
            declared.default_value =
                field_default_from_code(field.default_code, closure);
        }
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
    bool exported, const std::vector<variant_meta> &variants,
    const std::shared_ptr<scope_t> &closure, std::string &error) {
    enum_def *def = NULL;
    if (!structtype_prepare_enum(module, module_name, name, exported, def,
                                 error)) {
        yyerror("%s", error.c_str());
        return false;
    }

    std::vector<variant_def> declared_variants;
    for (const auto &variant : variants) {
        variant_def declared;
        declared.name = variant.name;
        for (const auto &field : variant.fields) {
            if (!validate_type_constraint(&field.type)) {
                structtype_rollback_enum(def);
                return false;
            }
            field_def declared_field;
            declared_field.name = field.name;
            declared_field.type = copy_type_constraint(&field.type);
            if (field.default_code) {
                declared_field.has_default = true;
                declared_field.default_value =
                    field_default_from_code(field.default_code, closure);
            }
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
        signature.is_async = method.is_async;
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

struct struct_init_entry {
    std::string name;
    value_t *value;
    bool spread;
};

static void free_struct_init_entries(
    std::vector<struct_init_entry> &entries) {
    for (auto &entry : entries)
        freeval(entry.value);
    entries.clear();
}

static value_t *build_struct_from_entries(
    struct_def *def, std::vector<struct_init_entry> &entries) {
    if (!def) {
        free_struct_init_entries(entries);
        return NULL;
    }

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(def->fields.size(), NULL);
    std::vector<bool> filled(def->fields.size(), false);
    std::vector<bool> explicit_seen(def->fields.size(), false);
    struct value_t *spread_value = NULL;
    bool ok = true;

    for (size_t i = 0; i < entries.size(); ++i) {
        struct_init_entry &entry = entries[i];
        if (entry.spread) {
            if (i != 0) {
                yyerror("struct spread must come first");
                ok = false;
                continue;
            }
            if (spread_value) {
                yyerror("struct literal has multiple spread values");
                ok = false;
                continue;
            }
            struct value_t *value = entry.value;
            entry.value = NULL;
            if (!value) {
                ok = false;
                continue;
            }
            if (value->type != T_struct) {
                yyerror("struct spread expects a struct, got %s",
                        typename_of(value));
                freeval(value);
                ok = false;
                continue;
            }
            struct struct_value_t *source = (struct struct_value_t *)value;
            if (source->def != def) {
                yyerror("struct spread expects %s, got %s",
                        def->name.c_str(),
                        source->def ? source->def->name.c_str()
                                    : typename_of(value));
                freeval(value);
                ok = false;
                continue;
            }
            if (!source->payload ||
                source->payload->fields.size() != def->fields.size()) {
                yyerror("invalid struct spread value");
                freeval(value);
                ok = false;
                continue;
            }
            spread_value = value;
            for (size_t k = 0; k < def->fields.size(); ++k) {
                if (payload->fields[k])
                    freeval(payload->fields[k]);
                payload->fields[k] = dupval(source->payload->fields[k]);
                filled[k] = true;
            }
            continue;
        }

        int index = structtype_field_index(def, entry.name.c_str());
        if (index < 0) {
            yyerror("unknown field %s in %s", entry.name.c_str(),
                    def->name.c_str());
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        if (explicit_seen[(size_t)index]) {
            yyerror("field %s repeated in %s", entry.name.c_str(),
                    def->name.c_str());
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        explicit_seen[(size_t)index] = true;
        if (!entry.value ||
            !type_constraint_matches(&def->fields[(size_t)index].type,
                                     entry.value)) {
            if (entry.value) {
                yyerror("field %s of %s expects %s, got %s",
                        entry.name.c_str(), def->name.c_str(),
                        type_constraint_name(
                            &def->fields[(size_t)index].type),
                        typename_of(entry.value));
            }
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        if (payload->fields[(size_t)index])
            freeval(payload->fields[(size_t)index]);
        payload->fields[(size_t)index] = entry.value;
        entry.value = NULL;
        filled[(size_t)index] = true;
    }

    if (spread_value)
        freeval(spread_value);

    if (ok) {
        for (size_t i = 0; i < filled.size(); ++i) {
            if (filled[i] || explicit_seen[i])
                continue;
            const field_def &field = def->fields[i];
            if (!field.has_default) {
                yyerror("field %s of %s is required",
                        field.name.c_str(), def->name.c_str());
                ok = false;
                continue;
            }
            struct value_t *value = eval_field_default(
                field, def->name.c_str(), field.name.c_str());
            if (!value) {
                ok = false;
                break;
            }
            if (!type_constraint_matches(&field.type, value)) {
                yyerror("field %s of %s expects %s, got %s",
                        field.name.c_str(), def->name.c_str(),
                        type_constraint_name(&field.type),
                        typename_of(value));
                freeval(value);
                ok = false;
                break;
            }
            payload->fields[i] = value;
            filled[i] = true;
        }
    }

    if (!ok) {
        free_struct_init_entries(entries);
        return NULL;
    }
    entries.clear();
    return newstructval(def, payload);
}

struct value_t *eval_struct_lit(struct struct_lit_node *lit) {
    struct_def *def = structtype_find_struct(lit->module, lit->name);
    if (!def) {
        yyerror("unknown struct type %s", lit->name);
        return NULL;
    }

    std::vector<struct_init_entry> entries;
    for (struct struct_init *init = lit->fields; init; init = init->next) {
        struct value_t *value = eval(init->value);
        if (!value) {
            free_struct_init_entries(entries);
            return NULL;
        }
        struct_init_entry entry;
        entry.name = init->name ? init->name : "";
        entry.value = value;
        entry.spread = init->name == NULL;
        entries.push_back(std::move(entry));
    }
    return build_struct_from_entries(def, entries);
}

static void free_constructed_fields(
    std::vector<std::pair<std::string, value_t *>> &fields) {
    for (auto &field : fields)
        freeval(field.second);
    fields.clear();
}

value_t *build_struct_value(
    const char *module, const char *name,
    std::vector<std::pair<std::string, value_t *>> &fields,
    const std::vector<bool> &spread) {
    struct_def *def = structtype_find_struct(module, name);
    if (!def) {
        yyerror("unknown struct type %s", name ? name : "");
        free_constructed_fields(fields);
        return NULL;
    }

    std::vector<struct_init_entry> entries;
    entries.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
        struct_init_entry entry;
        entry.name = fields[i].first;
        entry.value = fields[i].second;
        entry.spread = i < spread.size() && spread[i];
        fields[i].second = NULL;
        entries.push_back(std::move(entry));
    }
    fields.clear();
    return build_struct_from_entries(def, entries);
}

static value_t *build_enum_from_entries(
    enum_def *def, int variant_index,
    std::vector<struct_init_entry> &entries) {
    if (!def || variant_index < 0 ||
        (size_t)variant_index >= def->variants.size()) {
        free_struct_init_entries(entries);
        return NULL;
    }
    const variant_def &variant = def->variants[(size_t)variant_index];
    std::string owner = def->name + "::" + variant.name;

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(variant.fields.size(), NULL);
    std::vector<bool> filled(variant.fields.size(), false);
    std::vector<bool> explicit_seen(variant.fields.size(), false);
    bool ok = true;

    for (auto &entry : entries) {
        if (entry.spread) {
            yyerror("copy-with is only supported for structs");
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        int index =
            structtype_variant_field_index(&variant, entry.name.c_str());
        if (index < 0) {
            yyerror("unknown field %s in %s.%s", entry.name.c_str(),
                    def->name.c_str(), variant.name.c_str());
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        if (explicit_seen[(size_t)index]) {
            yyerror("field %s repeated in %s.%s", entry.name.c_str(),
                    def->name.c_str(), variant.name.c_str());
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        explicit_seen[(size_t)index] = true;
        if (!entry.value ||
            !type_constraint_matches(&variant.fields[(size_t)index].type,
                                     entry.value)) {
            if (entry.value) {
                yyerror("field %s of %s.%s expects %s, got %s",
                        entry.name.c_str(), def->name.c_str(),
                        variant.name.c_str(),
                        type_constraint_name(
                            &variant.fields[(size_t)index].type),
                        typename_of(entry.value));
            }
            freeval(entry.value);
            entry.value = NULL;
            ok = false;
            continue;
        }
        payload->fields[(size_t)index] = entry.value;
        entry.value = NULL;
        filled[(size_t)index] = true;
    }

    if (ok) {
        for (size_t i = 0; i < filled.size(); ++i) {
            if (filled[i] || explicit_seen[i])
                continue;
            const field_def &field = variant.fields[i];
            if (!field.has_default) {
                yyerror("field %s of %s.%s is required",
                        field.name.c_str(), def->name.c_str(),
                        variant.name.c_str());
                ok = false;
                continue;
            }
            struct value_t *value = eval_field_default(
                field, owner.c_str(), field.name.c_str());
            if (!value) {
                ok = false;
                break;
            }
            if (!type_constraint_matches(&field.type, value)) {
                yyerror("field %s of %s.%s expects %s, got %s",
                        field.name.c_str(), def->name.c_str(),
                        variant.name.c_str(),
                        type_constraint_name(&field.type),
                        typename_of(value));
                freeval(value);
                ok = false;
                break;
            }
            payload->fields[i] = value;
            filled[i] = true;
        }
    }

    if (!ok) {
        free_struct_init_entries(entries);
        return NULL;
    }
    entries.clear();
    return newenumval(def, variant_index, payload);
}

value_t *build_enum_value(
    const char *module, const char *name, const char *variant_name,
    std::vector<std::pair<std::string, value_t *>> &fields,
    const std::vector<bool> &spread) {
    enum_def *def = structtype_find_enum(module, name);
    if (!def) {
        yyerror("unknown enum type %s", name ? name : "");
        free_constructed_fields(fields);
        return NULL;
    }

    int variant_index =
        structtype_variant_index(def, variant_name ? variant_name : "");
    if (variant_index < 0) {
        yyerror("unknown enum variant %s.%s", def->name.c_str(),
                variant_name ? variant_name : "");
        free_constructed_fields(fields);
        return NULL;
    }

    std::vector<struct_init_entry> entries;
    entries.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
        struct_init_entry entry;
        entry.name = fields[i].first;
        entry.value = fields[i].second;
        entry.spread = i < spread.size() && spread[i];
        fields[i].second = NULL;
        entries.push_back(std::move(entry));
    }
    fields.clear();
    return build_enum_from_entries(def, variant_index, entries);
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

    std::vector<std::string> forbidden;
    forbidden.push_back("self");
    for (struct enum_variant *v = e->variants; v; v = v->next) {
        for (struct struct_member *m = v->fields; m; m = m->next) {
            if (m->nodetype == 'f' && m->name)
                forbidden.push_back(m->name);
        }
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
            if (m->default_value) {
                if (!check_field_default_names(m->default_value, e->name,
                                               m->name, forbidden)) {
                    structtype_rollback_enum(def);
                    return;
                }
                field.has_default = true;
                field.default_value = make_field_default(
                    m->default_value, e->name, m->name, current_scope());
                m->default_value = NULL;
            }
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
    for (struct struct_init *init = lit->fields; init; init = init->next) {
        if (init->name == NULL) {
            yyerror("copy-with is only supported for structs");
            return NULL;
        }
    }
    enum_def *def = structtype_find_enum(lit->module, lit->name);
    if (!def) {
        yyerror("unknown enum type %s", lit->name);
        return NULL;
    }

    int variant_index = structtype_variant_index(def, lit->variant);
    if (variant_index < 0) {
        yyerror("unknown enum variant %s.%s", def->name.c_str(),
                lit->variant);
        return NULL;
    }

    std::vector<struct_init_entry> entries;
    for (struct struct_init *init = lit->fields; init; init = init->next) {
        struct value_t *value = eval(init->value);
        if (!value) {
            free_struct_init_entries(entries);
            return NULL;
        }
        struct_init_entry entry;
        entry.name = init->name;
        entry.value = value;
        entry.spread = false;
        entries.push_back(std::move(entry));
    }
    return build_enum_from_entries(def, variant_index, entries);
}
