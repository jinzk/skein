#ifndef SKEIN_VM_HOST_CALLS_H
#define SKEIN_VM_HOST_CALLS_H

/*
 * Redirect executor calls to the installed host table.  This header is only
 * included by vm.cpp so vm_bridge.cpp can still take the address of the real
 * implementations while installing them.
 */

#define bind_pattern_value (skein_vm_host().fn_bind_pattern_value)
#define free_pattern_bindings (skein_vm_host().fn_free_pattern_bindings)

#define build_struct_value (skein_vm_host().fn_build_struct_value)
#define build_enum_value (skein_vm_host().fn_build_enum_value)
#define declare_struct_meta (skein_vm_host().fn_declare_struct_meta)
#define declare_enum_meta (skein_vm_host().fn_declare_enum_meta)
#define declare_trait_meta (skein_vm_host().fn_declare_trait_meta)
#define declare_impl_meta (skein_vm_host().fn_declare_impl_meta)

#define fiber_await_task (skein_vm_host().fn_fiber_await_task)
#define fiber_is_generator (skein_vm_host().fn_fiber_is_generator)
#define fiber_yield_value (skein_vm_host().fn_fiber_yield_value)
#define generator_next_value (skein_vm_host().fn_generator_next_value)
#define get_generator_value (skein_vm_host().fn_get_generator_value)
#define get_task_value (skein_vm_host().fn_get_task_value)

#define package_build_namespace (skein_vm_host().fn_package_build_namespace)
#define package_import (skein_vm_host().fn_package_import)
#define package_is_loading (skein_vm_host().fn_package_is_loading)
#define package_publish (skein_vm_host().fn_package_publish)


#define skein_vm_call_value_args (skein_vm_host().fn_skein_vm_call_value_args)
#define skein_vm_prepare_function_body \
    (skein_vm_host().fn_skein_vm_prepare_function_body)

#endif
