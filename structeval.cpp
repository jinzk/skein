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
    if (std::strcmp(sl->sym->name, "self") != 0) {
        error = "method first parameter must be self";
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

static bool evaluate_method(struct struct_member *member, struct_def *owner,
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
    std::vector<field_def> fields;
    for (struct struct_member *m = s->members; m; m = m->next) {
        if (m->nodetype != 'f')
            continue;
        if (!validate_type_constraint(m->type))
            return;
        field_def field;
        field.name = m->name;
        field.type = copy_type_constraint(m->type);
        fields.push_back(field);
    }

    std::string error;
    struct_def *def = NULL;
    if (!structtype_create_struct(
            s->module, diag_current_module_name().c_str(), s->name,
            std::move(fields), def, error)) {
        yyerror("%s", error.c_str());
        return;
    }

    for (struct struct_member *m = s->members; m; m = m->next) {
        if (m->nodetype != 'm')
            continue;
        std::shared_ptr<function_def> code;
        error.clear();
        if (!evaluate_method(m, def, code, error)) {
            if (!error.empty())
                yyerror("%s", error.c_str());
            continue;
        }
        if (!structtype_add_method(def, m->name, code, error))
            yyerror("%s", error.c_str());
    }
}

bool eval_impl_decl(struct impl_node *impl) {
    struct_def *owner =
        structtype_find_struct(impl->module, impl->type_name);
    if (!owner) {
        yyerror("unknown type %s", impl->type_name);
        return false;
    }

    trait_def *trait = NULL;
    if (impl->trait_name) {
        trait = structtype_find_trait(impl->module, impl->trait_name);
        if (!trait) {
            yyerror("unknown trait %s", impl->trait_name);
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
            std::move(methods), def, error))
        yyerror("%s", error.c_str());
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
