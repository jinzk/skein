#include "jit.h"

#include "jit_internal.h"

#include "diagnostics.h"
#include "dict.h"
#include "control_runtime.h"
#include "eval_context.h"
#include "gc.h"
#include "gc_roots.h"
#include "list_grow.h"
#include "string_grow.h"
#include "builtin_ids.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"
#include "unicode.h"
#include "value_ops.h"
#include "value_runtime.h"
#include "vm_eval.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define SKEIN_JIT_SYSV __attribute__((sysv_abi))
#else
#define SKEIN_JIT_SYSV
#endif

extern "C" SKEIN_JIT_SYSV void skein_jit_pop_scope(
    skein::jit::thread_state *ts);

namespace {

using skein::jit::internal::read_bool_env;
using skein::jit::internal::resolve_global_symbol;
using skein::jit::internal::unpack_loc;

} // namespace

extern "C" SKEIN_JIT_SYSV int skein_jit_check_interrupt() {
    return g_interrupted ? 1 : 0;
}

/*
 * C helpers called from generated code.  They must not throw C++ exceptions
 * across the JIT frame.
 */

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_null() { return newnull(); }

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_int(long long value) {
    return newintval(value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_dup(value_t *value) { return dupval(value); }

extern "C" SKEIN_JIT_SYSV void skein_jit_free(value_t *value) { freeval(value); }

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_binary(value_t *left, value_t *right, int op,
                                    int line, int column) {
    diag_set_location(line, column);
    if (!left || !right) {
        freeval(left);
        freeval(right);
        return nullptr;
    }
    value_t *result = apply_binary_value(op, left, right);
    freeval(left);
    freeval(right);
    return result;
}

extern "C" SKEIN_JIT_SYSV int skein_jit_compare(int op, value_t *left, value_t *right,
                                 int *valid, int line, int column) {
    diag_set_location(line, column);
    if (valid)
        *valid = 0;
    if (!left || !right) {
        freeval(left);
        freeval(right);
        return 0;
    }

    bool result = false;
    if (left->type == T_int && right->type == T_int) {
        const long long x = ((int_value_t *)left)->value;
        const long long y = ((int_value_t *)right)->value;
        const int cmp = x < y ? -1 : (x > y ? 1 : 0);
        switch (op) {
            case 1: result = cmp > 0; break;
            case 2: result = cmp < 0; break;
            case 3: result = cmp != 0; break;
            case 4: result = cmp == 0; break;
            case 5: result = cmp >= 0; break;
            case 6: result = cmp <= 0; break;
            default: break;
        }
        if (valid)
            *valid = 1;
    } else {
        value_t *compared = skein_vm_do_compare(op, left, right);
        if (compared) {
            result = truthy(compared);
            freeval(compared);
            if (valid)
                *valid = 1;
        }
    }

    freeval(left);
    freeval(right);
    return result ? 1 : 0;
}

extern "C" SKEIN_JIT_SYSV int skein_jit_truthy(value_t *value) {
    int result = truthy(value) ? 1 : 0;
    freeval(value);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_load_slot(value_t *value, int line,
                                        int column) {
    diag_set_location(line, column);
    if (!value) {
        yyerror("symbol not found");
        return nullptr;
    }
    return dupval(value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_define_slot(const skein::bc::SlotDesc *desc,
                                          value_t *value, value_t **dest,
                                          int line, int column) {
    diag_set_location(line, column);
    if (!desc || !dest || !value) {
        freeval(value);
        yyerror("invalid bytecode slot");
        return nullptr;
    }
    if (!validate_type_constraint(&desc->type)) {
        freeval(value);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, value)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(value));
        freeval(value);
        return nullptr;
    }
    freeval(*dest);
    *dest = value;
    return newnull();
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_store_slot(const skein::bc::SlotDesc *desc,
                                         value_t *value, value_t **dest,
                                         int line, int column) {
    diag_set_location(line, column);
    if (!desc || !dest || !value) {
        freeval(value);
        yyerror("invalid bytecode slot");
        return nullptr;
    }
    string_refresh_cache(*dest, value);
    if (desc->is_const) {
        yyerror("cannot assign to constant %s", desc->name.c_str());
        freeval(value);
        return nullptr;
    }
    if (!validate_type_constraint(&desc->type)) {
        freeval(value);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, value)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(value));
        freeval(value);
        return nullptr;
    }
    freeval(*dest);
    *dest = value;
    return dupval(value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_compound_slot(
    const skein::bc::SlotDesc *desc, value_t *value, value_t **dest,
    int op, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!desc || !dest || !value || !*dest) {
        freeval(value);
        yyerror("invalid bytecode slot");
        return nullptr;
    }
    if (desc->is_const) {
        yyerror("cannot assign to constant %s", desc->name.c_str());
        freeval(value);
        return nullptr;
    }
    value_t *result = apply_binary_value(op, *dest, value);
    freeval(value);
    if (!result)
        return nullptr;
    if (!validate_type_constraint(&desc->type)) {
        freeval(result);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, result)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(result));
        freeval(result);
        return nullptr;
    }
    freeval(*dest);
    *dest = result;
    return dupval(result);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_incdec_slot(
    const skein::bc::SlotDesc *desc, value_t **dest, int delta, int prefix,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!desc || !dest || !*dest) {
        yyerror("invalid bytecode slot");
        return nullptr;
    }
    if (desc->is_const) {
        yyerror("cannot assign to constant %s", desc->name.c_str());
        return nullptr;
    }
    value_t *delta_value = newintval(delta);
    value_t *result = apply_binary_value('+', *dest, delta_value);
    freeval(delta_value);
    if (!result)
        return nullptr;
    if (!validate_type_constraint(&desc->type)) {
        freeval(result);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, result)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(result));
        freeval(result);
        return nullptr;
    }
    value_t *ret = prefix ? dupval(result) : dupval(*dest);
    freeval(*dest);
    *dest = result;
    return ret;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_slot_binary(value_t *left, value_t *right,
                                          value_t **dest,
                                          const skein::bc::SlotDesc *desc,
                                          int op, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!left || !right || !dest || !desc) {
        yyerror("symbol not found");
        return nullptr;
    }

    value_t *left_copy = dupval(left);
    value_t *right_copy = dupval(right);
    value_t *result = apply_binary_value(op, left_copy, right_copy);
    freeval(left_copy);
    freeval(right_copy);
    if (!result)
        return nullptr;

    if (desc->is_const) {
        yyerror("cannot assign to constant %s", desc->name.c_str());
        freeval(result);
        return nullptr;
    }
    if (!validate_type_constraint(&desc->type)) {
        freeval(result);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, result)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(result));
        freeval(result);
        return nullptr;
    }
    freeval(*dest);
    *dest = result;
    return dupval(result);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_slot_binary_const(
    value_t *left, const value_t *constant, value_t **dest,
    const skein::bc::SlotDesc *desc, int op, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!left || !dest || !desc) {
        yyerror("symbol not found");
        return nullptr;
    }

    value_t *left_copy = dupval(left);
    value_t *right_copy = constant ? dupval(const_cast<value_t *>(constant))
                                   : newnull();
    value_t *result = apply_binary_value(op, left_copy, right_copy);
    freeval(left_copy);
    freeval(right_copy);
    if (!result)
        return nullptr;

    if (desc->is_const) {
        yyerror("cannot assign to constant %s", desc->name.c_str());
        freeval(result);
        return nullptr;
    }
    if (!validate_type_constraint(&desc->type)) {
        freeval(result);
        return nullptr;
    }
    if (!type_constraint_matches(&desc->type, result)) {
        yyerror("variable %s expects %s, got %s", desc->name.c_str(),
                type_constraint_name(&desc->type), typename_of(result));
        freeval(result);
        return nullptr;
    }
    freeval(*dest);
    *dest = result;
    return dupval(result);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_load_global(const skein::bc::Proto *proto,
                                          uint32_t slot,
                                          uint32_t name_index,
                                          int line, int column) {
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = resolve_global_symbol(proto, slot, name_index);
    if (!sym) {
        yyerror("symbol not found %s", name);
        return nullptr;
    }
    if (sym->is_uninitialized) {
        yyerror("parameter %s is used before it is initialized", name);
        return nullptr;
    }
    return dupval(sym->val);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_store_global(const skein::bc::Proto *proto,
                                           uint32_t slot,
                                           uint32_t name_index,
                                           value_t *value, int line,
                                           int column) {
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        freeval(value);
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = resolve_global_symbol(proto, slot, name_index);
    if (!sym) {
        yyerror("symbol not found %s", name);
        freeval(value);
        return nullptr;
    }
    return assign_symbol_direct(sym, value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_tail_request(
    skein::jit::thread_state *ts, value_t *callee, value_t **args,
    uint32_t argc) {
    if (!ts) {
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        freeval(callee);
        yyerror("JIT tail request without state");
        return nullptr;
    }
    ts->tail_callee = callee;
    ts->tail_args.assign(args, args + argc);
    for (uint32_t i = 0; i < argc; ++i)
        args[i] = nullptr;
    ts->tail_argc = argc;
    ts->tail_pending = true;
    return nullptr;
}

extern "C" SKEIN_JIT_SYSV size_t skein_jit_push_roots(
    skein::jit::thread_state *ts, value_t **values, uint32_t count) {
    if (!ts || !ts->ctx || !values)
        return SIZE_MAX;
    const size_t mark = gc_roots_push_array(
        ts->ctx->scopes, values, static_cast<size_t>(count));
    /*
     * Package loading keeps freshly built namespaces in C++ locals before
     * they are published; a collection triggered from a JIT prologue could
     * clear their package scopes.  Leave stress collections to the VM while
     * a package is loading.
     */
    if (gc_stress_enabled() && g_package_loading == 0)
        collect_cycles(ts->ctx->scopes);
    return mark;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_pop_roots(
    skein::jit::thread_state *ts, size_t mark) {
    if (!ts || !ts->ctx || mark == SIZE_MAX)
        return;
    gc_roots_pop(ts->ctx->scopes, mark);
}

extern "C" SKEIN_JIT_SYSV int skein_jit_tail_self(skein::jit::thread_state *ts,
                                   function_def *current_def,
                                   value_t *callee, value_t **params,
                                   value_t **args, uint32_t argc) {
    if (!ts || !ts->ctx || !current_def || !current_def->code ||
        !callee || callee->type != T_func || !params || !args)
        return 0;
    struct func_value_t *fv = (struct func_value_t *)callee;
    if (!fv->def || !fv->def->code ||
        fv->def->code.get() != current_def->code.get())
        return 0;
    if (current_def->code->is_method ||
        argc != (uint32_t)current_def->code->nparams)
        return 0;

    int index = 0;
    for (struct symlist *sl = current_def->code->sl; sl;
         sl = sl->next, ++index) {
        if (sl->has_pattern || sl->variadic)
            return 0;
        if (!type_constraint_matches(sl->type, args[(size_t)index]))
            return 0;
    }

    /*
     * A reused frame cannot run defers: refuse the fast path when any scope
     * still has pending deferred work (the generic tail path exits cleanly).
     */
    for (size_t i = ts->frame_defer_mark; i < ts->defer_scopes.size(); ++i) {
        if (!ts->defer_scopes[i].empty())
            return 0;
    }

    while (ts->ctx->scopes.restore_stack.size() > ts->frame_scope_mark)
        skein_jit_pop_scope(ts);
    while (ts->defer_scopes.size() > ts->frame_defer_mark)
        ts->defer_scopes.pop_back();
    for (uint32_t i = 0; i < argc; ++i) {
        freeval(params[i]);
        params[i] = args[i];
        args[i] = nullptr;
    }
    freeval(callee);
    return 1;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_tail_invoke(value_t *callee, value_t **args,
                                          uint32_t argc, int line,
                                          int column) {
    diag_set_location(line, column);
    skein::jit::invoke_fn fn = skein::jit::tail_invoke();
    if (!fn) {
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        freeval(callee);
        yyerror("JIT tail call target is not installed");
        return nullptr;
    }
    value_t *result = fn(callee, args, argc);
    if (g_interrupted) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_new_struct(
    const skein::bc::ConstructDesc *desc, value_t **fields) {
    if (!desc || !fields) {
        yyerror("invalid bytecode constructor");
        return nullptr;
    }
    skein::jit::construct_fn fn = skein::jit::construct();
    if (!fn) {
        for (size_t i = 0; i < desc->fields.size(); ++i)
            freeval(fields[i]);
        yyerror("JIT struct construction is not installed");
        return nullptr;
    }
    return fn(*desc, fields);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_get_field(value_t *container,
                                        const char *name, int line,
                                        int column) {
    diag_set_location(line, column);
    if (!container || !name) {
        freeval(container);
        return nullptr;
    }

    if (container->type == T_struct) {
        struct struct_value_t *sv = (struct struct_value_t *)container;
        if (!sv->def) {
            yyerror("invalid struct value");
            freeval(container);
            return nullptr;
        }
        const int field = structtype_field_index(sv->def, name);
        if (field >= 0 && sv->payload &&
            (size_t)field < sv->payload->fields.size()) {
            value_t *result = dupval(sv->payload->fields[(size_t)field]);
            freeval(container);
            return result;
        }
        auto method = sv->def->methods.find(name);
        if (method != sv->def->methods.end()) {
            value_t *result =
                newmethodval(dupval(container), method->second);
            freeval(container);
            return result;
        }
        yyerror("field %s not found in %s", name, sv->def->name.c_str());
        freeval(container);
        return nullptr;
    }

    if (container->type == T_enum) {
        struct enum_value_t *ev = (struct enum_value_t *)container;
        if (!ev->def || ev->variant < 0 ||
            (size_t)ev->variant >= ev->def->variants.size()) {
            yyerror("invalid enum value");
            freeval(container);
            return nullptr;
        }
        const struct variant_def &variant =
            ev->def->variants[(size_t)ev->variant];
        const int field = structtype_variant_field_index(&variant, name);
        if (field >= 0 && ev->payload &&
            (size_t)field < ev->payload->fields.size()) {
            value_t *result = dupval(ev->payload->fields[(size_t)field]);
            freeval(container);
            return result;
        }
        auto method = ev->def->methods.find(name);
        if (method != ev->def->methods.end()) {
            value_t *result =
                newmethodval(dupval(container), method->second);
            freeval(container);
            return result;
        }
        yyerror("field %s not found in %s.%s", name,
                ev->def->name.c_str(), variant.name.c_str());
        freeval(container);
        return nullptr;
    }

    struct string_value_t key;
    key.type = T_string;
    key.value = const_cast<char *>(name);
    value_t *result = skein_vm_read_member_value(
        container, (value_t *)&key, true);
    freeval(container);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_neg(value_t *value, int line, int column) {
    diag_set_location(line, column);
    if (!value)
        return nullptr;
    value_t *result = nullptr;
    if (value->type == T_int) {
        result = new_int_checked(
            -(__int128)((struct int_value_t *)value)->value);
    } else if (value->type == T_float) {
        result = newfloatval(-((struct float_value_t *)value)->value);
    } else {
        yyerror("cannot negate %s", typename_of(value));
    }
    freeval(value);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_not(value_t *value, int line, int column) {
    diag_set_location(line, column);
    if (!value)
        return nullptr;
    value_t *result = newboolval(!truthy(value));
    freeval(value);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_compare_value(
    int op, value_t *left, value_t *right, int line, int column) {
    diag_set_location(line, column);
    if (!left || !right) {
        freeval(left);
        freeval(right);
        yyerror("symbol not found");
        return nullptr;
    }
    value_t *result = skein_vm_do_compare(op, left, right);
    freeval(left);
    freeval(right);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_bitnot(value_t *value, int line, int column) {
    diag_set_location(line, column);
    if (!value)
        return nullptr;
    value_t *result = nullptr;
    if (value->type != T_int) {
        yyerror("bitwise not expects an int, got %s", typename_of(value));
    } else {
        result = newintval(~((struct int_value_t *)value)->value);
    }
    freeval(value);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_compound_global(
    const skein::bc::Proto *proto, uint32_t slot, uint32_t name_index,
    value_t *value, int op, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        freeval(value);
        return nullptr;
    }
    symbol *sym = resolve_global_symbol(proto, slot, name_index);
    if (!sym) {
        yyerror("symbol not found %s",
                proto->consts[name_index].name.c_str());
        freeval(value);
        return nullptr;
    }
    return compound_symbol_direct(sym, op, value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_incdec_global(
    const skein::bc::Proto *proto, uint32_t slot, uint32_t name_index,
    int delta, int prefix, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    symbol *sym = resolve_global_symbol(proto, slot, name_index);
    if (!sym) {
        yyerror("symbol not found %s",
                proto->consts[name_index].name.c_str());
        return nullptr;
    }
    return increment_symbol_direct(sym, delta, prefix != 0);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_get_field_or_enum(
    const skein::bc::FieldOrEnumDesc *desc, int line, int column) {
    diag_set_location(line, column);
    if (!desc) {
        yyerror("invalid field-or-enum descriptor");
        return nullptr;
    }

    struct symbol *base = lookupsym(desc->base.c_str());
    if (base) {
        if (base->is_uninitialized) {
            yyerror("parameter %s is used before it is initialized",
                    desc->base.c_str());
            return nullptr;
        }
        value_t *container = dupval(base->val);
        if (!container)
            return nullptr;
        struct string_value_t key;
        key.type = T_string;
        key.value = const_cast<char *>(desc->field.c_str());
        value_t *result =
            skein_vm_read_member_value(container, (value_t *)&key, true);
        freeval(container);
        return result;
    }

    structtype_lookup_result lookup;
    std::string error;
    structtype_lookup_status status = structtype_lookup_type(
        nullptr, desc->source.c_str(), desc->base.c_str(), lookup, error);
    if (status == STRUCTTYPE_LOOKUP_FOUND &&
        lookup.kind == STRUCTTYPE_LOOKUP_ENUM && lookup.enumeration) {
        skein::jit::build_enum_fn build = skein::jit::build_enum();
        if (!build) {
            yyerror("enum construction is not installed");
            return nullptr;
        }
        std::vector<std::pair<std::string, value_t *>> fields;
        std::vector<bool> spread;
        return build(lookup.enumeration->source.c_str(),
                     lookup.enumeration->name.c_str(), desc->field.c_str(),
                     fields, spread);
    }
    if (status == STRUCTTYPE_LOOKUP_AMBIGUOUS ||
        status == STRUCTTYPE_LOOKUP_NOT_VISIBLE ||
        status == STRUCTTYPE_LOOKUP_ERROR) {
        yyerror("%s", error.c_str());
    } else {
        yyerror("symbol not found %s", desc->base.c_str());
    }
    return nullptr;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_fail(value_t *value,
                                              uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!value) {
        yyerror("fail value is missing");
        return;
    }
    skein_vm_eval_raise(value);
}

extern "C" SKEIN_JIT_SYSV void skein_jit_runtime_error(
    const char *message, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    yyerror("%s", message ? message : "runtime error");
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_load_name(const skein::bc::Proto *proto,
                                        uint32_t name_index, int line,
                                        int column) {
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = lookupsym(name);
    if (!sym) {
        yyerror("symbol not found %s", name);
        return nullptr;
    }
    if (sym->is_uninitialized) {
        yyerror("parameter %s is used before it is initialized", name);
        return nullptr;
    }
    return dupval(sym->val);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_store_name(const skein::bc::Proto *proto,
                                         uint32_t name_index,
                                         value_t *value, int line,
                                         int column) {
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        freeval(value);
        return nullptr;
    }
    return assign_symbol_value(proto->consts[name_index].name.c_str(), value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_compound_name(
    const skein::bc::Proto *proto, uint32_t name_index, value_t *value,
    int op, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size() || !value) {
        freeval(value);
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = lookupsym(name);
    if (!sym) {
        yyerror("symbol not found %s", name);
        freeval(value);
        return nullptr;
    }
    return compound_symbol_direct(sym, op, value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_incdec_name(
    const skein::bc::Proto *proto, uint32_t name_index, int delta, int prefix,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = lookupsym(name);
    if (!sym) {
        yyerror("symbol not found %s", name);
        return nullptr;
    }
    return increment_symbol_direct(sym, delta, prefix != 0);
}

extern "C" SKEIN_JIT_SYSV int skein_jit_check_redefine(
    const skein::bc::Proto *proto, uint32_t name_index, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return 0;
    }
    const char *name = proto->consts[name_index].name.c_str();
    if (isrepeated(name)) {
        yyerror("symbol repeated %s", name ? name : "");
        return 0;
    }
    return 1;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_define_name(
    const skein::bc::Proto *proto, uint32_t decl_index, value_t *value,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || decl_index >= proto->decls.size() || !value) {
        freeval(value);
        yyerror("invalid bytecode declaration");
        return nullptr;
    }
    const skein::bc::DeclDesc &decl = proto->decls[decl_index];
    if (isrepeated(decl.name.c_str())) {
        yyerror("symbol repeated %s", decl.name.c_str());
        freeval(value);
        return nullptr;
    }
    if (!validate_type_constraint(&decl.type)) {
        freeval(value);
        return nullptr;
    }
    if (!type_constraint_matches(&decl.type, value)) {
        yyerror("variable %s expects %s, got %s", decl.name.c_str(),
                type_constraint_name(&decl.type), typename_of(value));
        freeval(value);
        return nullptr;
    }
    symbol *sym = newsym(decl.name.c_str());
    sym->val = value;
    type_constraint_assign(&sym->type, &decl.type);
    sym->exported = decl.exported;
    sym->is_const = decl.is_const;
    savesym(sym);
    return newnull();
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_load_upvalue(
    function_def *def, uint32_t index, int line, int column) {
    diag_set_location(line, column);
    upvalue_cell *cell =
        def && index < def->upvalues.size() ? def->upvalues[index].get()
                                            : nullptr;
    if (!cell) {
        yyerror("invalid bytecode upvalue");
        return nullptr;
    }
    value_t *value = upvalue_load(cell);
    if (!value) {
        yyerror("invalid bytecode upvalue");
        return nullptr;
    }
    return value;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_store_upvalue(
    function_def *def, uint32_t index, value_t *value, int line,
    int column) {
    diag_set_location(line, column);
    upvalue_cell *cell =
        def && index < def->upvalues.size() ? def->upvalues[index].get()
                                            : nullptr;
    if (!cell) {
        freeval(value);
        yyerror("invalid bytecode upvalue");
        return nullptr;
    }
    return upvalue_store(cell, value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_compound_upvalue(
    function_def *def, uint32_t index, int op, value_t *value, int line,
    int column) {
    diag_set_location(line, column);
    upvalue_cell *cell =
        def && index < def->upvalues.size() ? def->upvalues[index].get()
                                            : nullptr;
    if (!cell) {
        freeval(value);
        yyerror("invalid bytecode upvalue");
        return nullptr;
    }
    return upvalue_compound(cell, op, value);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_incdec_upvalue(
    function_def *def, uint32_t index, int delta, int prefix, int line,
    int column) {
    diag_set_location(line, column);
    upvalue_cell *cell =
        def && index < def->upvalues.size() ? def->upvalues[index].get()
                                            : nullptr;
    if (!cell) {
        yyerror("invalid bytecode upvalue");
        return nullptr;
    }
    return upvalue_incdec(cell, delta, prefix != 0);
}

extern "C" SKEIN_JIT_SYSV size_t skein_jit_push_cell_mark(
    skein::jit::thread_state *ts) {
    return ts ? ts->open_cells.size() : SIZE_MAX;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_close_cells(
    skein::jit::thread_state *ts, size_t mark) {
    if (!ts || mark == SIZE_MAX || mark > ts->open_cells.size())
        return;
    for (size_t i = ts->open_cells.size(); i > mark; --i)
        close_upvalue_cell(*ts->open_cells[i - 1]);
    ts->open_cells.resize(mark);
}
extern "C" SKEIN_JIT_SYSV void skein_jit_push_frame_scope(
    function_def *def) {
    skein_vm_eval_push_closure_scope(def ? def->closure : nullptr);
}

namespace {

std::shared_ptr<upvalue_cell> jit_open_cell(
    skein::jit::thread_state *ts, value_t **storage,
    const struct type_constraint *type, const std::string &name, bool is_const) {
    if (ts) {
        for (const auto &cell : ts->open_cells)
            if (cell->slot == storage)
                return cell;
    }
    std::shared_ptr<upvalue_cell> cell = std::make_shared<upvalue_cell>();
    cell->slot = storage;
    cell->type = type;
    cell->owner = &active_scope_context();
    cell->name = name;
    cell->is_const = is_const;
    if (ts)
        ts->open_cells.push_back(cell);
    return cell;
}

const struct type_constraint *jit_param_type(function_def *def, uint32_t index) {
    if (!def || !def->code)
        return nullptr;
    uint32_t current = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++current) {
        if (current == index)
            return sl->type;
    }
    return nullptr;
}

std::shared_ptr<upvalue_cell> jit_capture_upvalue(
    skein::jit::thread_state *ts, function_def *def,
    const skein::bc::Proto *proto, value_t **frame_values,
    const std::string &name) {
    for (uint32_t i = 0; i < proto->param_names.size(); ++i) {
        if (proto->param_names[i] != name)
            continue;
        return jit_open_cell(ts, &frame_values[i],
                             jit_param_type(def, i), name, false);
    }
    for (uint32_t i = 0; i < proto->slots.size(); ++i) {
        const skein::bc::SlotDesc &slot = proto->slots[i];
        if (slot.name != name)
            continue;
        return jit_open_cell(ts, &frame_values[proto->nparams + i],
                             &slot.type, name, slot.is_const);
    }
    /*
     * Name-based locals live in this function's block scopes.  Walk only up
     * to the function boundary: the closure parent belongs to another
     * function, and inherited captures are resolved below through
     * `def->upvalues`.
     */
    for (scope_t *s = active_scope_context().current.get(); s;
         s = s->parent.get()) {
        if (s->is_global)
            break;
        if (struct symbol *local = s->syms.find(std::string_view(name)))
            return jit_open_cell(ts, &local->val, &local->type, name,
                                 local->is_const);
        if (s->is_function)
            break;
    }
    if (def) {
        for (size_t i = 0; i < proto->upvalues.size() &&
                           i < def->upvalues.size();
             ++i) {
            if (proto->upvalues[i] == name)
                return def->upvalues[i];
        }
    }
    /* The VM stores a null cell for names that cannot be captured; leave
     * the runtime error to the first LOAD_UPVALUE, matching its behavior. */
    return nullptr;
}

} // namespace

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_closure(
    skein::jit::thread_state *ts, function_def *def,
    const skein::bc::Proto *proto, uint32_t const_index,
    value_t **frame_values, uint64_t loc) {
    int line = 0;
    int column = 0;
    skein::jit::internal::unpack_loc(loc, line, column);
    diag_set_location(line, column);
    if (!proto || const_index >= proto->consts.size() || !frame_values) {
        yyerror("invalid bytecode function");
        return nullptr;
    }
    const skein::bc::Constant &constant = proto->consts[const_index];
    if (constant.kind != skein::bc::ConstantKind::Function ||
        !constant.function) {
        yyerror("invalid bytecode function");
        return nullptr;
    }
    if (!validate_function_params(constant.function->name.c_str(),
                                  constant.function->sl))
        return nullptr;
    std::shared_ptr<scope_t> closure = current_scope();
    if (closure && closure->is_global)
        closure.reset();
    std::vector<std::shared_ptr<upvalue_cell>> upvalues;
    upvalues.reserve(constant.function->upvalues.size());
    for (const std::string &name : constant.function->upvalues)
        upvalues.push_back(
            jit_capture_upvalue(ts, def, proto, frame_values, name));
    return newfuncval(constant.function, closure, std::move(upvalues));
}

extern "C" SKEIN_JIT_SYSV void skein_jit_push_scope() { skein_vm_eval_pushscope(); }

extern "C" SKEIN_JIT_SYSV void skein_jit_frame_scope_pushed(
    skein::jit::thread_state *ts) {
    if (ts && ts->ctx)
        ts->frame_scope_mark = ts->ctx->scopes.restore_stack.size();
}

namespace {

int jit_handler_index(skein::jit::thread_state *ts, size_t mark, int kind) {
    if (!ts)
        return -1;
    for (size_t i = ts->handlers.size(); i > mark; --i) {
        if (ts->handlers[i - 1].kind == kind)
            return static_cast<int>(i - 1);
    }
    return -1;
}

value_t *jit_end_handler_capture(skein::jit::thread_state::handler_state &h) {
    if (!h.capture)
        return nullptr;
    h.capture = false;
    eval_capture_result result = skein_vm_eval_capture_end(h.snapshot);
    return result.error;
}

} // namespace

extern "C" SKEIN_JIT_SYSV size_t skein_jit_push_handler_mark(
    skein::jit::thread_state *ts) {
    return ts ? ts->handlers.size() : SIZE_MAX;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_discard_handlers(
    skein::jit::thread_state *ts, size_t mark) {
    if (!ts || mark > ts->handlers.size())
        return;
    while (ts->handlers.size() > mark) {
        value_t *error = jit_end_handler_capture(ts->handlers.back());
        freeval(error);
        ts->handlers.pop_back();
    }
}

extern "C" SKEIN_JIT_SYSV int skein_jit_push_handler(
    skein::jit::thread_state *ts, int kind, int32_t target,
    int32_t stack_depth, int flags, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!ts || !ts->ctx) {
        yyerror("handler state is not installed");
        return 0;
    }
    skein::jit::thread_state::handler_state handler;
    handler.kind = kind;
    handler.target = target;
    handler.stack_depth = stack_depth;
    handler.scope_depth = ts->ctx->scopes.restore_stack.size();
    if (kind == 1 && (flags & 1) != 0 && handler.scope_depth > 0)
        --handler.scope_depth;
    handler.body_depth = ts->handler_body_depth;
    handler.eval_handler_depth = ts->ctx->handler_depth;
    handler.capture = kind == 0 && (flags & 1) != 0;
    if (handler.capture)
        skein_vm_eval_capture_begin(handler.snapshot);
    ts->handlers.push_back(std::move(handler));
    return 1;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_pop_handler(
    skein::jit::thread_state *ts, size_t mark) {
    if (!ts || ts->handlers.size() <= mark) {
        yyerror("internal bytecode handler underflow");
        return;
    }
    value_t *error = jit_end_handler_capture(ts->handlers.back());
    freeval(error);
    ts->handlers.pop_back();
}

extern "C" SKEIN_JIT_SYSV void skein_jit_enter_handler_body(
    skein::jit::thread_state *ts) {
    if (!ts || !ts->ctx)
        return;
    ++ts->handler_body_depth;
    ++ts->ctx->handler_depth;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_leave_handler_body(
    skein::jit::thread_state *ts) {
    if (!ts || !ts->ctx)
        return;
    if (ts->handler_body_depth > 0)
        --ts->handler_body_depth;
    if (ts->ctx->handler_depth > ts->saved_handler_depth)
        --ts->ctx->handler_depth;
}

extern "C" SKEIN_JIT_SYSV int skein_jit_check_result(
    skein::jit::thread_state *ts, value_t **slot, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!slot || !*slot) {
        yyerror("internal bytecode stack underflow");
        return -1;
    }
    value_t *value = *slot;
    *slot = nullptr;
    if (value->type != T_result) {
        *slot = value;
        return 0;
    }
    struct result_value_t *result = (struct result_value_t *)value;
    if (result->ok) {
        value_t *inner = dupval(result->value);
        freeval(value);
        *slot = inner ? inner : newnull();
        return 0;
    }
    value_t *error = dupval(result->error);
    freeval(value);
    if (!error)
        error = newerrorval("fail", "");
    set_eval_control(EVAL_CONTROL_FAIL, error);
    (void)ts;
    return 1;
}

extern "C" SKEIN_JIT_SYSV int skein_jit_bind_name(
    skein::jit::thread_state *ts, value_t *value, value_t **slot,
    const char *name, int is_const, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    (void)ts;
    if (!value) {
        yyerror("symbol not found %s", name ? name : "");
        return 0;
    }
    if (slot) {
        freeval(*slot);
        *slot = value;
        return 1;
    }
    if (!name) {
        yyerror("invalid bytecode name");
        freeval(value);
        return 0;
    }
    if (isrepeated(name)) {
        yyerror("symbol repeated %s", name);
        freeval(value);
        return 0;
    }
    struct symbol *sym = newsym(name);
    sym->val = value;
    sym->is_const = is_const != 0;
    savesym(sym);
    return 1;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_bind_func(
    skein::jit::thread_state *ts, value_t *value,
    const skein::bc::Proto *proto, uint32_t name_index, int exported,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    (void)ts;
    if (!value || !proto || name_index >= proto->consts.size()) {
        freeval(value);
        yyerror("invalid bytecode function binding");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    if (isrepeated(name)) {
        yyerror("symbol repeated %s", name);
        freeval(value);
        return nullptr;
    }
    struct symbol *sym = newsym(name);
    sym->val = value;
    sym->exported = exported != 0;
    savesym(sym);
    return newnull();
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_import(
    const skein::bc::Proto *proto, uint32_t index, value_t **slot,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!proto || index >= proto->imports.size()) {
        yyerror("invalid bytecode import descriptor");
        return nullptr;
    }
    const skein::bc::ImportDesc &desc = proto->imports[index];
    std::string display_path = desc.path;
    if (!display_path.empty() && display_path[0] == '\x02')
        display_path.erase(0, 1);

    skein::jit::import_fn load = skein::jit::import();
    if (!load) {
        yyerror("import support is not installed");
        return nullptr;
    }
    std::string imported_package;
    std::string error;
    int errors_before = diag_error_count();
    value_t *ns = load(desc.path.c_str(),
                       desc.base.empty() ? nullptr : desc.base.c_str(),
                       &imported_package, error);
    if (!ns) {
        if (diag_error_count() == errors_before)
            yyerror("import %s: %s", display_path.c_str(), error.c_str());
        else
            diag_mark_script_failure();
        return nullptr;
    }

    const char *name =
        desc.alias.empty()
            ? skein_vm_import_binding_name(desc.path.c_str())
            : desc.alias.c_str();
    if (slot) {
        freeval(*slot);
        *slot = ns;
    } else if (isrepeated(name)) {
        struct symbol *existing = lookupsym(name);
        bool same_namespace =
            existing && existing->val && ns && existing->val->type == T_dict &&
            ns->type == T_dict &&
            ((struct dict_value_t *)existing->val)->payload ==
                ((struct dict_value_t *)ns)->payload;
        if (!same_namespace) {
            yyerror("import name already exists: %s", name);
            freeval(ns);
            return nullptr;
        }
        freeval(ns);
    } else {
        struct symbol *sym = newsym(name);
        sym->val = ns;
        savesym(sym);
    }

    const std::string &path = desc.path;
    if (!path.empty() && path[0] != '.' && path[0] != '@') {
        const char *alias =
            desc.alias.empty() ? nullptr : desc.alias.c_str();
        if (!alias) {
            size_t dot = path.rfind('.');
            alias = dot == std::string::npos ? path.c_str()
                                             : path.c_str() + dot + 1;
        }
        structtype_register_alias(
            diag_current_source_name().c_str(), alias,
            imported_package.empty() ? path.c_str()
                                     : imported_package.c_str());
    }
    return newnull();
}

extern "C" SKEIN_JIT_SYSV void skein_jit_recover(
    skein::jit::thread_state *ts, value_t *value, size_t mark,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!value) {
        yyerror("recover value is missing");
        return;
    }
    if (jit_handler_index(ts, mark, 1) < 0) {
        yyerror("recover outside error handler");
        freeval(value);
        return;
    }
    set_eval_control(EVAL_CONTROL_RECOVER, value);}

extern "C" SKEIN_JIT_SYSV void skein_jit_pass(
    skein::jit::thread_state *ts, value_t *value, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!value) {
        yyerror("pass value is missing");
        return;
    }
    if (!ts || ts->handler_body_depth <= 0) {
        yyerror("pass outside error handler");
        freeval(value);
        return;
    }
    skein_vm_eval_raise(value);
}

extern "C" SKEIN_JIT_SYSV int64_t skein_jit_unwind(
    skein::jit::thread_state *ts, size_t mark) {
    if (!ts || !ts->ctx)
        return -1;
    eval_context &context = *ts->ctx;
    if (context.control == EVAL_CONTROL_CLOSE)
        return -1;

    int kind = 0;
    if (context.control == EVAL_CONTROL_RECOVER)
        kind = 1;
    else if (context.control != EVAL_CONTROL_NONE &&
             context.control != EVAL_CONTROL_FAIL)
        return -1;

    const int index = jit_handler_index(ts, mark, kind);
    if (index < 0)
        return -1;

    value_t *value = nullptr;
    if (context.control == EVAL_CONTROL_FAIL)
        value = take_eval_control(EVAL_CONTROL_FAIL);
    else if (context.control == EVAL_CONTROL_RECOVER)
        value = take_eval_control(EVAL_CONTROL_RECOVER);

    for (size_t i = ts->handlers.size(); i > (size_t)index + 1; --i) {
        value_t *error = jit_end_handler_capture(ts->handlers[i - 1]);
        freeval(error);
    }
    skein::jit::thread_state::handler_state handler =
        ts->handlers[(size_t)index];
    value_t *captured = jit_end_handler_capture(ts->handlers[(size_t)index]);
    ts->handlers.resize((size_t)index);
    if (!value)
        value = captured;
    else
        freeval(captured);
    if (!value) {
        value = eval_build_error(
            context.has_captured_error ? context.captured_error
                                       : std::string("runtime error"),
            context.captured_source, context.captured_line,
            context.captured_column, context.captured_trace);
    }
    if (!value)
        value = newerrorval("runtime", "runtime error");

    while (context.scopes.restore_stack.size() > handler.scope_depth)
        skein_jit_pop_scope(ts);
    ts->handler_body_depth = handler.body_depth;
    context.handler_depth = handler.eval_handler_depth;
    ts->unwind_value = value;
    return handler.target;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_take_unwind_value(
    skein::jit::thread_state *ts) {
    if (!ts)
        return nullptr;
    value_t *value = ts->unwind_value;
    ts->unwind_value = nullptr;
    return value;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_clear_slot(
    skein::jit::thread_state *ts, value_t **slot) {
    if (!slot)
        return;
    if (ts) {
        for (auto &cell : ts->open_cells) {
            if (cell && cell->slot == slot)
                close_upvalue_cell(*cell);
        }
    }
    freeval(*slot);
    *slot = nullptr;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_drop_slots(value_t **slots,
                                                    uint32_t count) {
    if (!slots)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        freeval(slots[i]);
        slots[i] = nullptr;
    }
}

extern "C" SKEIN_JIT_SYSV void skein_jit_enter_defer_frame(
    skein::jit::thread_state *ts) {
    if (!ts)
        return;
    ts->frame_defer_mark = ts->defer_scopes.size();
    ts->defer_scopes.emplace_back();
}

extern "C" SKEIN_JIT_SYSV void skein_jit_push_defer_scope(
    skein::jit::thread_state *ts) {
    if (ts)
        ts->defer_scopes.emplace_back();
}

extern "C" SKEIN_JIT_SYSV void skein_jit_pop_defer_scope(
    skein::jit::thread_state *ts) {
    if (!ts || ts->defer_scopes.size() <= ts->frame_defer_mark)
        return;
    std::vector<skein::jit::thread_state::defer_record> records =
        std::move(ts->defer_scopes.back());
    ts->defer_scopes.pop_back();
    skein::jit::run_defer_scope_fn run = skein::jit::run_defer_scope();
    if (run)
        run(records);
}

extern "C" SKEIN_JIT_SYSV int skein_jit_run_frame_defers(
    skein::jit::thread_state *ts) {
    if (!ts || !ts->ctx)
        return 0;
    eval_context &context = *ts->ctx;
    const int errors_before = context.runtime_error_count;
    skein::jit::run_defer_scope_fn run = skein::jit::run_defer_scope();
    while (ts->defer_scopes.size() > ts->frame_defer_mark) {
        std::vector<skein::jit::thread_state::defer_record> records =
            std::move(ts->defer_scopes.back());
        ts->defer_scopes.pop_back();
        if (run)
            run(records);
    }
    return context.runtime_error_count != errors_before ||
                   context.control != EVAL_CONTROL_NONE
               ? 1
               : 0;
}

extern "C" SKEIN_JIT_SYSV int skein_jit_defer_push(
    skein::jit::thread_state *ts, const skein::bc::Proto *proto,
    uint32_t index, value_t **frame_values, function_def *def,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!ts || !proto || !frame_values || !def ||
        index >= proto->defer_protos.size() || !proto->defer_protos[index] ||
        ts->defer_scopes.empty()) {
        yyerror("invalid bytecode defer descriptor");
        return 0;
    }

    skein::jit::thread_state::defer_record record;
    record.proto = proto->defer_protos[index];
    if (index < proto->defer_captures.size()) {
        for (const skein::bc::DeferCapture &capture :
             proto->defer_captures[index]) {
            std::shared_ptr<upvalue_cell> cell;
            switch (capture.kind) {
                case skein::bc::DeferCaptureKind::Slot: {
                    if (capture.index >= proto->slots.size()) {
                        yyerror("invalid defer slot capture %s",
                                capture.name.c_str());
                        return 0;
                    }
                    const skein::bc::SlotDesc &slot =
                        proto->slots[capture.index];
                    cell = jit_open_cell(
                        ts, &frame_values[proto->nparams + capture.index],
                        &slot.type, capture.name, slot.is_const);
                    break;
                }
                case skein::bc::DeferCaptureKind::Param: {
                    if (capture.index >= proto->nparams) {
                        yyerror("invalid defer parameter capture %s",
                                capture.name.c_str());
                        return 0;
                    }
                    cell = jit_open_cell(
                        ts, &frame_values[capture.index],
                        jit_param_type(def, capture.index), capture.name,
                        false);
                    break;
                }
                case skein::bc::DeferCaptureKind::Upvalue: {
                    if (capture.index >= def->upvalues.size() ||
                        !def->upvalues[capture.index]) {
                        yyerror("upvalue %s is not captured by a frame slot",
                                capture.name.c_str());
                        return 0;
                    }
                    cell = def->upvalues[capture.index];
                    break;
                }
            }
            if (!cell)
                return 0;
            record.upvalues.push_back(std::move(cell));
        }
    }
    ts->defer_scopes.back().push_back(std::move(record));
    return 1;
}

namespace {

constexpr uint64_t jit_iter_active = 1;
constexpr uint64_t jit_iter_generator = 2;

} // namespace

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_iter_new(
    value_t **slot, uint64_t *state, const skein::bc::IterDesc *desc,
    value_t *iterable, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!slot || !state || !desc || !iterable) {
        freeval(iterable);
        yyerror("invalid bytecode iterator");
        return nullptr;
    }
    freeval(*slot);
    *slot = nullptr;
    state[0] = 0;
    state[1] = 0;
    state[2] = 0;

    if (skein::jit::generator_test() &&
        skein::jit::generator_test()(iterable)) {
        if (desc->two_vars) {
            yyerror("two-variable for-in requires dict, got generator");
            freeval(iterable);
            return nullptr;
        }
        *slot = iterable;
        state[2] = jit_iter_active | jit_iter_generator;
        return newnull();
    }

    if (iterable->type == T_deque || iterable->type == T_heap ||
        iterable->type == T_set) {
        struct list_value_t *materialized = newlistval();
        std::vector<value_t *> items;
        if (iterable->type == T_deque) {
            deque_items_in_order(
                ((struct deque_value_t *)iterable)->payload.get(), items);
        } else if (iterable->type == T_heap) {
            heap_items_level_order(
                ((struct heap_value_t *)iterable)->payload.get(), items);
        } else {
            set_values_in_order(
                ((struct set_value_t *)iterable)->payload.get(), items);
        }
        materialized->payload->items.reserve(items.size());
        for (value_t *item : items)
            materialized->payload->items.push_back(dupval(item));
        freeval(iterable);
        iterable = (value_t *)materialized;
    }

    size_t count = 0;
    if (iterable->type == T_list) {
        count = ((struct list_value_t *)iterable)->payload->items.size();
    } else if (iterable->type == T_dict) {
        count = ((struct dict_value_t *)iterable)->payload->items.size();
    } else if (iterable->type == T_string) {
        count = utf8_length(((struct string_value_t *)iterable)->value);
    } else if (iterable->type == T_bytes) {
        count = ((struct bytes_value_t *)iterable)->value.size();
    } else {
        yyerror("cannot iterate %s", typename_of(iterable));
        freeval(iterable);
        return nullptr;
    }

    if (desc->two_vars && iterable->type != T_dict) {
        yyerror("two-variable for-in requires dict, got %s",
                typename_of(iterable));
        freeval(iterable);
        return nullptr;
    }
    if (desc->two_vars && desc->key_name == desc->value_name) {
        yyerror("loop variable repeated: %s", desc->key_name.c_str());
        freeval(iterable);
        return nullptr;
    }

    *slot = iterable;
    state[1] = count;
    state[2] = jit_iter_active;
    return newnull();
}

extern "C" SKEIN_JIT_SYSV int skein_jit_iter_next(
    value_t **slot, uint64_t *state, value_t **dest_key,
    value_t **dest_value, const skein::bc::IterDesc *desc,
    uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!slot || !state || !desc || !dest_key) {
        yyerror("invalid bytecode iterator");
        return -1;
    }
    if (!(state[2] & jit_iter_active) || !*slot)
        return 1;

    if (state[2] & jit_iter_generator) {
        skein::jit::generator_step_fn step = skein::jit::generator_step();
        if (!step) {
            yyerror("generator support is not installed");
            return -1;
        }
        value_t *item = nullptr;
        bool done = false;
        if (!step(*slot, item, done)) {
            freeval(item);
            return -1;
        }
        if (done) {
            freeval(item);
            return 1;
        }
        freeval(*dest_key);
        *dest_key = item;
        ++state[0];
        return 0;
    }

    const size_t index = (size_t)state[0];
    if (index >= (size_t)state[1])
        return 1;

    value_t *item = nullptr;
    value_t *second = nullptr;
    if ((*slot)->type == T_list) {
        item = dupval(((struct list_value_t *)*slot)->payload->items[index]);
    } else if ((*slot)->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)*slot;
        item = dupval(dict->payload->items[index].first);
        if (desc->two_vars)
            second = dupval(dict->payload->items[index].second);
    } else if ((*slot)->type == T_string) {
        std::string ch;
        if (!utf8_char_at(((struct string_value_t *)*slot)->value, index,
                          ch)) {
            yyerror("invalid UTF-8 string iteration");
            return -1;
        }
        size_t consumed = 0;
        unsigned int codepoint = 0;
        if (!utf8_decode_one(ch.data(), ch.size(), consumed, codepoint)) {
            yyerror("invalid UTF-8 string iteration");
            return -1;
        }
        item = newcharval(codepoint);
    } else if ((*slot)->type == T_bytes) {
        item = newintval((unsigned char)((struct bytes_value_t *)*slot)
                             ->value[index]);
    } else {
        yyerror("cannot iterate %s", typename_of(*slot));
        return -1;
    }

    freeval(*dest_key);
    *dest_key = item;
    if (desc->two_vars && dest_value) {
        freeval(*dest_value);
        *dest_value = second;
    } else {
        freeval(second);
    }
    ++state[0];
    return 0;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_iter_end(value_t **slot,
                                                  uint64_t *state) {
    if (slot) {
        freeval(*slot);
        *slot = nullptr;
    }
    if (state) {
        state[0] = 0;
        state[1] = 0;
        state[2] = 0;
    }
}

extern "C" SKEIN_JIT_SYSV void skein_jit_pop_scope(
    skein::jit::thread_state *ts) {
    /*
     * A block scope owns the symbols declared inside it.  Close any cells the
     * JIT opened on those values before the scope (and its symbols) is freed.
     */
    scope_t *scope = topscope();
    if (ts && scope) {
        for (auto &cell : ts->open_cells) {
            if (!cell || !cell->slot)
                continue;
            bool closed = false;
            for (symbol *sym : scope->syms.small) {
                if (sym && cell->slot == &sym->val) {
                    close_upvalue_cell(*cell);
                    closed = true;
                    break;
                }
            }
            if (closed)
                continue;
            for (auto &entry : scope->syms.index) {
                if (entry.second && cell->slot == &entry.second->val) {
                    close_upvalue_cell(*cell);
                    break;
                }
            }
        }
    }
    skein_vm_eval_popscope();
}

extern "C" SKEIN_JIT_SYSV int skein_jit_bind_patterns(function_def *def, value_t **values,
                                       uint32_t argc) {
    skein::jit::bind_frame_fn fn = skein::jit::bind_frame();
    if (!fn) {
        yyerror("JIT pattern binder is not installed");
        return 0;
    }
    return fn(def, values, argc);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_new_list(value_t **items,
                                       uint32_t count, uint32_t frozen,
                                       int line, int column) {
    diag_set_location(line, column);
    struct list_value_t *list =
        frozen ? newtupleval() : newlistval();
    for (uint32_t i = 0; i < count; ++i) {
        list->payload->items.push_back(items[i]);
        items[i] = nullptr;
    }
    return (value_t *)list;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_new_dict(value_t **items, uint32_t count,
                                       int line, int column) {
    diag_set_location(line, column);
    struct dict_value_t *dict = newdictval();
    for (uint32_t i = 0; i < count; ++i) {
        value_t *key = items[(size_t)i * 2];
        value_t *value = items[(size_t)i * 2 + 1];
        items[(size_t)i * 2] = nullptr;
        items[(size_t)i * 2 + 1] = nullptr;
        if (!dict_key_allowed(key)) {
            yyerror("%s, got %s", dict_key_error(), typename_of(key));
            freeval(key);
            freeval(value);
            for (uint32_t j = i + 1; j < count; ++j) {
                freeval(items[(size_t)j * 2]);
                freeval(items[(size_t)j * 2 + 1]);
                items[(size_t)j * 2] = nullptr;
                items[(size_t)j * 2 + 1] = nullptr;
            }
            freeval((value_t *)dict);
            return nullptr;
        }
        int found = dict_find_index(dict, key);
        if (found >= 0) {
            freeval(dict->payload->items[(size_t)found].second);
            dict->payload->items[(size_t)found].second = value;
            freeval(key);
        } else {
            dict_append_item(dict, key, value);
        }
    }
    return (value_t *)dict;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_get_index_cached(
    value_t *container, value_t *index,
    skein::bc::IndexReadCacheEntry *cache, int line, int column) {
    diag_set_location(line, column);
    if (!container || !index) {
        freeval(container);
        freeval(index);
        return nullptr;
    }

    if (container->type == T_dict && ((dict_value_t *)container)->payload) {
        struct dict_value_t *dict = (struct dict_value_t *)container;
        if (!dict_key_allowed(index)) {
            yyerror("%s, got %s", dict_key_error(), typename_of(index));
            freeval(container);
            freeval(index);
            return nullptr;
        }
        const auto &items = dict->payload->items;
        int found = -1;
        if (cache && cache->index >= 0 && cache->size == items.size() &&
            (size_t)cache->index < items.size() &&
            value_equals(items[(size_t)cache->index].first, index)) {
            found = cache->index;
        } else {
            found = dict_find_index(dict, index);
            if (found < 0) {
                yyerror("key not found");
                freeval(container);
                freeval(index);
                return nullptr;
            }
            if (cache) {
                cache->size = items.size();
                cache->index = found;
            }
        }
        value_t *result = dupval(items[(size_t)found].second);
        freeval(container);
        freeval(index);
        return result;
    }

    value_t *result =
        skein_vm_read_member_value(container, index, false);
    freeval(container);
    freeval(index);
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_assign_index(
    const skein::bc::IndexDesc *desc, value_t **indices, uint32_t count,
    value_t *value, value_t **base_slot, uint64_t packed_loc) {
    int line = 0;
    int column = 0;
    unpack_loc(packed_loc, line, column);
    diag_set_location(line, column);
    if (!desc || !indices || !value || desc->fields.empty() ||
        count != desc->fields.size()) {
        freeval(value);
        for (uint32_t i = 0; i < count; ++i)
            freeval(indices[i]);
        return nullptr;
    }

    using skein::bc::IndexBaseKind;
    struct symbol view {};
    symbol *base = nullptr;
    upvalue_cell *upvalue = nullptr;
    value_t *upvalue_before = nullptr;
    value_t **base_slot_ptr = nullptr;
    if (desc->base_kind == IndexBaseKind::Slot ||
        desc->base_kind == IndexBaseKind::Param) {
        if (!base_slot || !*base_slot) {
            yyerror("symbol not found %s", desc->base_name.c_str());
            freeval(value);
            return nullptr;
        }
        base_slot_ptr = base_slot;
        view.name = const_cast<char *>(desc->base_name.c_str());
        view.name_len = desc->base_name.size();
        view.val = *base_slot;
        view.type.kind = TYPE_ANY;
        base = &view;
    } else if (desc->base_kind == IndexBaseKind::Upvalue) {
        function_def *def = reinterpret_cast<function_def *>(base_slot);
        if (!def || desc->base_index >= def->upvalues.size()) {
            yyerror("invalid bytecode upvalue");
            freeval(value);
            return nullptr;
        }
        upvalue = def->upvalues[desc->base_index].get();
        view.name = const_cast<char *>(desc->base_name.c_str());
        view.name_len = desc->base_name.size();
        view.type.kind = TYPE_ANY;
        if (upvalue) {
            upvalue_before = upvalue_cell_value(upvalue);
            view.val = upvalue_before;
            base = &view;
        } else {
            symbol *sym = lookupsym(desc->base_name.c_str());
            scope_context &scopes = active_scope_context();
            if (!sym || !scopes.global_scope ||
                scopes.global_scope->syms.find(desc->base_name) != sym) {
                yyerror("upvalue %s is not captured by a frame slot",
                        desc->base_name.c_str());
                freeval(value);
                return nullptr;
            }
            base = sym;
        }
    } else if (desc->base_kind == IndexBaseKind::Global) {
        base = resolve_global_symbol(desc->base_index,
                                     desc->base_name.c_str());
    } else if (desc->base_kind == IndexBaseKind::Name) {
        base = lookupsym(desc->base_name.c_str());
    }

    std::vector<value_t *> args(indices, indices + count);
    value_t *result =
        assign_index_symbol(base, desc->base_name.c_str(), args,
                            desc->fields, value);
    if (base == &view && view.val != upvalue_before && upvalue) {
        if (upvalue->slot)
            *upvalue->slot = view.val;
        else
            upvalue->value = view.val;
        freeval(upvalue_before);
        view.val = nullptr;
    } else if (base == &view && base_slot_ptr && *base_slot_ptr != view.val) {
        freeval(*base_slot_ptr);
        *base_slot_ptr = view.val;
        view.val = nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_invoke(value_t *callee, value_t **args,
                                     uint32_t argc, int line, int column) {
    diag_set_location(line, column);
    skein::jit::invoke_fn fn = skein::jit::invoke();
    if (!fn) {
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        freeval(callee);
        yyerror("JIT call target is not installed");
        return nullptr;
    }
    value_t *result = fn(callee, args, argc);
    if (g_interrupted) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_invoke_method(
    value_t *receiver, const char *name, value_t **args, uint32_t argc,
    int line, int column) {
    diag_set_location(line, column);
    value_t *callee = nullptr;
    if (receiver && name) {
        if (receiver->type == T_struct) {
            struct struct_value_t *sv = (struct struct_value_t *)receiver;
            if (sv->def) {
                auto method = sv->def->methods.find(name);
                if (method != sv->def->methods.end())
                    callee = newmethodval(receiver, method->second);
            }
        } else if (receiver->type == T_enum) {
            struct enum_value_t *ev = (struct enum_value_t *)receiver;
            if (ev->def) {
                auto method = ev->def->methods.find(name);
                if (method != ev->def->methods.end())
                    callee = newmethodval(receiver, method->second);
            }
        }
        if (!callee) {
            struct string_value_t key;
            key.type = T_string;
            key.value = const_cast<char *>(name);
            callee = skein_vm_read_member_value(receiver,
                                                (struct value_t *)&key,
                                                true);
            if (callee)
                freeval(receiver);
        }
    }
    if (!callee) {
        freeval(receiver);
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        return nullptr;
    }
    skein::jit::invoke_fn fn = skein::jit::invoke();
    if (!fn) {
        freeval(callee);
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        yyerror("JIT call target is not installed");
        return nullptr;
    }
    value_t *result = fn(callee, args, argc);
    if (g_interrupted) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_list_append_self(
    value_t *callee, value_t *list, value_t **args, uint32_t argc,
    int line, int column) {
    diag_set_location(line, column);
    const bool append_builtin =
        callee && callee->type == T_builtin &&
        ((builtin_value_t *)callee)->functype == B_list_append;
    if (append_builtin && list_grow_append(list, args, argc)) {
        freeval(callee);
        value_t *result = dupval(list);
        freeval(list);
        return result;
    }
    std::vector<value_t *> call_args;
    call_args.reserve((size_t)argc + 1);
    call_args.push_back(list);
    for (uint32_t i = 0; i < argc; ++i)
        call_args.push_back(args[i]);
    skein::jit::invoke_fn fn = skein::jit::invoke();
    if (!fn) {
        freeval(callee);
        freeval(list);
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        yyerror("JIT call target is not installed");
        return nullptr;
    }
    value_t *result =
        fn(callee, call_args.data(), (uint32_t)call_args.size());
    if (g_interrupted) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_list_concat_self(
    value_t *left, value_t *right, int line, int column) {
    diag_set_location(line, column);
    if (left && right && left->type == T_list && right->type == T_list &&
        list_grow_concat(left, right)) {
        freeval(right);
        value_t *result = dupval(left);
        freeval(left);
        return result;
    }
    value_t *result = skein_vm_apply_binary_value('+', left, right);
    freeval(left);
    freeval(right);
    if (!result || g_interrupted) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_string_append_self(
    value_t *left, value_t *right, int line, int column) {
    diag_set_location(line, column);
    if (!left || !right) {
        freeval(left);
        freeval(right);
        return nullptr;
    }
    if (string_append_in_place(left, right, 1)) {
        freeval(right);
        return left;
    }
    const int errors_before = active_eval_context().runtime_error_count;
    value_t *result = skein_vm_apply_binary_value('+', left, right);
    freeval(left);
    freeval(right);
    if (!result || g_interrupted ||
        active_eval_context().runtime_error_count != errors_before) {
        freeval(result);
        return nullptr;
    }
    return result;
}

extern "C" SKEIN_JIT_SYSV void *skein_jit_direct_target(
    value_t *callee, value_t **args, uint32_t argc,
    skein::jit::thread_state *ts, function_def **def_out) {
    skein::jit::direct_fn fn = skein::jit::direct();
    if (!fn)
        return nullptr;
    return fn(callee, args, argc, ts, def_out);
}

extern "C" SKEIN_JIT_SYSV int skein_jit_enter(skein::jit::thread_state *ts,
                               function_def *def) {
    if (!ts || !ts->ctx)
        return 0;
    eval_context &context = *ts->ctx;
    ts->tail_pending = false;
    if (!ts->tail_call && context.vm_call_depth >= ts->max_depth) {
        yyerror("maximum recursion depth (%d) exceeded", ts->max_depth);
        return 0;
    }

    /*
     * Direct JIT-to-JIT calls reuse the caller's thread_state.  Save the
     * caller's per-call fields so nested enters/leaves form a proper stack.
     */
    skein::jit::thread_state::call_save saved;
    saved.saved_handler_depth = ts->saved_handler_depth;
    saved.handler_body_depth = ts->handler_body_depth;
    saved.scope_mark = ts->scope_mark;
    saved.frame_scope_mark = ts->frame_scope_mark;
    saved.frame_defer_mark = ts->frame_defer_mark;
    saved.saved_package = ts->saved_package;
    saved.saved_source = ts->saved_source;
    saved.entered = ts->entered;
    saved.depth_counted = ts->depth_counted;
    saved.diag_pushed = ts->diag_pushed;
    saved.tail_call = ts->tail_call;
    saved.package_valid = ts->saved_package_valid;
    saved.source_valid = ts->saved_source_valid;
    ts->saves.push_back(std::move(saved));

    ts->saved_handler_depth = context.handler_depth;
    ts->handler_body_depth = 0;
    ts->scope_mark = context.scopes.restore_stack.size();
    /* Updated by the prologue when the frame pushes its own scope. */
    ts->frame_scope_mark = ts->scope_mark;
    ts->frame_defer_mark = ts->defer_scopes.size();
    ts->saved_package_valid = false;
    ts->saved_source_valid = false;
    ts->entered = true;
    if (!ts->tail_call) {
        ++context.vm_call_depth;
        ts->depth_counted = true;
    } else {
        ts->depth_counted = false;
    }
    context.handler_depth = 0;

    const std::string *caller_source =
        diag_intern_source(diag_current_source_name());
    if (!ts->top_level) {
        if (def && def->code) {
            const std::string &current_package = diag_current_package_name();
            if (!def->code->package.empty() &&
                def->code->package != current_package) {
                ts->saved_package = current_package;
                ts->saved_package_valid = true;
                diag_set_package_name(def->code->package);
            }
            const std::string &current_source = diag_current_source_name();
            if (!def->code->source.empty() &&
                def->code->source != current_source) {
                ts->saved_source = current_source;
                ts->saved_source_valid = true;
                diag_set_source_name(def->code->source);
            }
        }
        const char *name = "<anonymous>";
        int line = g_error_line;
        int column = g_error_column;
        if (def && def->code) {
            if (!def->code->name.empty())
                name = def->code->name.c_str();
            if (line <= 0)
                line = def->code->def_line;
            if (column <= 0)
                column = def->code->def_column;
        }
        if (diag_call_depth() < 64) {
            static const bool debug_enabled =
                read_bool_env("SKEIN_JIT_DEBUG", false);
            if (debug_enabled)
                std::fprintf(stderr, "[jit] enter %s depth=%d\n", name,
                             diag_call_depth());
            diag_push_call(name, caller_source, line, column);
            ts->diag_pushed = true;
        }
    }
    return 1;
}

extern "C" SKEIN_JIT_SYSV void skein_jit_leave(skein::jit::thread_state *ts) {
    if (!ts || !ts->entered || !ts->ctx)
        return;
    eval_context &context = *ts->ctx;
    while (context.scopes.restore_stack.size() > ts->scope_mark)
        skein_vm_eval_popscope();
    context.handler_depth = ts->saved_handler_depth;
    if (!ts->tail_pending) {
        if (ts->saved_package_valid)
            diag_set_package_name(ts->saved_package);
        if (ts->saved_source_valid)
            diag_set_source_name(ts->saved_source);
    }
    if (ts->depth_counted && context.vm_call_depth > 0)
        --context.vm_call_depth;
    if (!ts->top_level && ts->diag_pushed) {
        static const bool debug_enabled =
            read_bool_env("SKEIN_JIT_DEBUG", false);
        if (debug_enabled)
            std::fprintf(stderr, "[jit] leave depth=%d tail=%d\n",
                         diag_call_depth(), ts->tail_pending ? 1 : 0);
        if (ts->tail_pending)
            ++ts->retained_diag_frames;
        else
            diag_pop_call();
    }

    if (!ts->saves.empty()) {
        skein::jit::thread_state::call_save saved = std::move(ts->saves.back());
        ts->saves.pop_back();
        ts->saved_handler_depth = saved.saved_handler_depth;
        ts->handler_body_depth = saved.handler_body_depth;
        ts->scope_mark = saved.scope_mark;
        ts->frame_scope_mark = saved.frame_scope_mark;
        ts->frame_defer_mark = saved.frame_defer_mark;
        ts->saved_package = std::move(saved.saved_package);
        ts->saved_source = std::move(saved.saved_source);
        ts->entered = saved.entered;
        ts->depth_counted = saved.depth_counted;
        ts->diag_pushed = saved.diag_pushed;
        ts->tail_call = saved.tail_call;
        ts->saved_package_valid = saved.package_valid;
        ts->saved_source_valid = saved.source_valid;
    } else {
        ts->diag_pushed = false;
        ts->entered = false;
        ts->handler_body_depth = 0;
        ts->saved_package_valid = false;
        ts->saved_source_valid = false;
    }
}

extern "C" SKEIN_JIT_SYSV void skein_jit_cleanup(skein::jit::thread_state *ts,
                                  value_t **values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        value_t *value = values[i];
        values[i] = nullptr;
        if (!value)
            continue;
        if (value->type == T_int) {
            if (((int_value_t *)value)->interned)
                continue;
        } else if (value->type == T_bool || value->type == T_null) {
            continue;
        }
        freeval(value);
    }
    skein_jit_leave(ts);
}
