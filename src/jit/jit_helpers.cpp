#include "jit.h"

#include "jit_internal.h"

#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "gc.h"
#include "gc_roots.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"
#include "value_ops.h"
#include "value_runtime.h"
#include "vm_eval.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define SKEIN_JIT_SYSV __attribute__((sysv_abi))
#else
#define SKEIN_JIT_SYSV
#endif

namespace {

using skein::jit::internal::read_bool_env;
using skein::jit::internal::resolve_global_symbol;
using skein::jit::internal::unpack_loc;

} // namespace

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
    if (gc_stress_enabled())
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

    while (ts->ctx->scopes.restore_stack.size() > ts->scope_mark)
        skein_vm_eval_popscope();
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

extern "C" SKEIN_JIT_SYSV void skein_jit_push_scope() { skein_vm_eval_pushscope(); }
extern "C" SKEIN_JIT_SYSV void skein_jit_pop_scope() { skein_vm_eval_popscope(); }

extern "C" SKEIN_JIT_SYSV int skein_jit_bind_patterns(function_def *def, value_t **values,
                                       uint32_t argc) {
    skein::jit::bind_frame_fn fn = skein::jit::bind_frame();
    if (!fn) {
        yyerror("JIT pattern binder is not installed");
        return 0;
    }
    return fn(def, values, argc);
}

extern "C" SKEIN_JIT_SYSV value_t *skein_jit_new_list(value_t **items, uint32_t count,
                                       int line, int column) {
    diag_set_location(line, column);
    struct list_value_t *list = newlistval();
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
    value_t *value, int line, int column) {
    diag_set_location(line, column);
    if (!desc || !indices || !value || desc->fields.empty() ||
        count != desc->fields.size()) {
        freeval(value);
        for (uint32_t i = 0; i < count; ++i)
            freeval(indices[i]);
        return nullptr;
    }

    symbol *base = nullptr;
    if (desc->base_kind == skein::bc::IndexBaseKind::Global)
        base = resolve_global_symbol(desc->base_index,
                                     desc->base_name.c_str());
    else if (desc->base_kind == skein::bc::IndexBaseKind::Name)
        base = lookupsym(desc->base_name.c_str());

    std::vector<value_t *> args(indices, indices + count);
    return assign_index_symbol(base, desc->base_name.c_str(), args,
                               desc->fields, value);
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
    saved.scope_mark = ts->scope_mark;
    saved.saved_module = ts->saved_module;
    saved.saved_source = ts->saved_source;
    saved.entered = ts->entered;
    saved.depth_counted = ts->depth_counted;
    saved.diag_pushed = ts->diag_pushed;
    saved.tail_call = ts->tail_call;
    saved.module_valid = ts->saved_module_valid;
    saved.source_valid = ts->saved_source_valid;
    ts->saves.push_back(std::move(saved));

    ts->saved_handler_depth = context.handler_depth;
    ts->scope_mark = context.scopes.restore_stack.size();
    ts->saved_module_valid = false;
    ts->saved_source_valid = false;
    ts->entered = true;
    if (!ts->tail_call) {
        ++context.vm_call_depth;
        ts->depth_counted = true;
    } else {
        ts->depth_counted = false;
    }
    context.handler_depth = 0;

    if (!ts->top_level) {
        if (def && def->code) {
            const std::string &current_module = diag_current_module_name();
            if (!def->code->module.empty() &&
                def->code->module != current_module) {
                ts->saved_module = current_module;
                ts->saved_module_valid = true;
                diag_set_module_name(def->code->module);
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
            diag_push_call(name, line, column);
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
        if (ts->saved_module_valid)
            diag_set_module_name(ts->saved_module);
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
        ts->scope_mark = saved.scope_mark;
        ts->saved_module = std::move(saved.saved_module);
        ts->saved_source = std::move(saved.saved_source);
        ts->entered = saved.entered;
        ts->depth_counted = saved.depth_counted;
        ts->diag_pushed = saved.diag_pushed;
        ts->tail_call = saved.tail_call;
        ts->saved_module_valid = saved.module_valid;
        ts->saved_source_valid = saved.source_valid;
    } else {
        ts->diag_pushed = false;
        ts->entered = false;
        ts->saved_module_valid = false;
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
