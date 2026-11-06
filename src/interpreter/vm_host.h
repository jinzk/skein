#ifndef SKEIN_VM_HOST_H
#define SKEIN_VM_HOST_H

#include "fiber.h"
#include "package.h"
#include "pattern.h"
#include "structeval.h"
#include "vm_eval.h"
#include "vm_internal.h"

/*
 * Host services used by the bytecode executor.  vm.cpp must not link against
 * the AST evaluator, fiber runtime, package loader or struct metadata layer;
 * the interpreter installs the real implementations into this table before
 * executing bytecode.
 */
struct vm_host {
    decltype(&bind_pattern_value) fn_bind_pattern_value = nullptr;
    decltype(&free_pattern_bindings) fn_free_pattern_bindings = nullptr;

    decltype(&build_struct_value) fn_build_struct_value = nullptr;
    decltype(&build_enum_value) fn_build_enum_value = nullptr;
    decltype(&declare_struct_meta) fn_declare_struct_meta = nullptr;
    decltype(&declare_enum_meta) fn_declare_enum_meta = nullptr;
    decltype(&declare_trait_meta) fn_declare_trait_meta = nullptr;
    decltype(&declare_impl_meta) fn_declare_impl_meta = nullptr;

    decltype(&fiber_await_task) fn_fiber_await_task = nullptr;
    decltype(&fiber_is_generator) fn_fiber_is_generator = nullptr;
    decltype(&fiber_yield_value) fn_fiber_yield_value = nullptr;
    decltype(&generator_next_value) fn_generator_next_value = nullptr;
    decltype(&get_generator_value) fn_get_generator_value = nullptr;
    decltype(&get_task_value) fn_get_task_value = nullptr;

    decltype(&package_build_namespace) fn_package_build_namespace = nullptr;
    decltype(&package_import) fn_package_import = nullptr;
    decltype(&package_is_loading) fn_package_is_loading = nullptr;
    decltype(&package_publish) fn_package_publish = nullptr;


    decltype(&skein_vm_call_value_args) fn_skein_vm_call_value_args = nullptr;
    decltype(&skein_vm_prepare_function_body)
        fn_skein_vm_prepare_function_body = nullptr;
};

/* Returns the process-wide host table.  Fields are null until installed. */
vm_host &skein_vm_host();

#endif
