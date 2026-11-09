#ifndef SKEIN_VM_H
#define SKEIN_VM_H

#include "pattern.h"
#include "valuetype.h"
#include "bytecode.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ast;
struct function_code;
struct function_def;

struct thread_transfer_function;
struct thread_transfer_heap;
struct thread_transfer_heap_node;

/* One value captured by a Proto-backed thread entry or a transferred
 * function.  `Data` values are owned data-only snapshots; `Function` values
 * point at another prepared Proto payload. */
struct thread_transfer_value {
    enum class Kind { Null, Data, Namespace, Function, Heap };
    Kind kind = Kind::Null;
    std::shared_ptr<struct value_t> data;
    std::vector<std::pair<std::string, thread_transfer_value>> fields;
    std::shared_ptr<struct thread_transfer_function> function;
    std::shared_ptr<struct thread_transfer_heap> heap;
};

struct thread_transfer_heap_node {
    thread_transfer_value value;
    uint32_t rank = 0;
    uint32_t left = UINT32_MAX;
    uint32_t right = UINT32_MAX;
};

struct thread_transfer_heap {
    thread_transfer_value less;
    std::vector<thread_transfer_heap_node> nodes;
    uint32_t root = UINT32_MAX;
    uint64_t size = 0;
};

/* A function prepared in the parent thread and reconstructed in the child. */
struct thread_transfer_function {
    std::string name;
    skein::bc::ProtoPtr proto;
    /* Parallel to proto->upvalues. */
    std::vector<thread_transfer_value> captures;
};

struct thread_transfer_module {
    std::string key;
    std::string path;
    std::string base;
    thread_transfer_value value;
};

struct thread_transfer_type_field {
    std::string name;
    struct type_constraint type;
    bool has_default = false;
    uint32_t default_index = UINT32_MAX;
    std::shared_ptr<struct thread_transfer_function> default_function;
};

struct thread_transfer_type_variant {
    std::string name;
    std::vector<thread_transfer_type_field> fields;
};

struct thread_transfer_type {
    bool is_enum = false;
    std::string module;
    std::string name;
    bool exported = false;
    std::vector<thread_transfer_type_field> fields;
    std::vector<thread_transfer_type_variant> variants;
    std::vector<std::pair<std::string,
                          std::shared_ptr<struct thread_transfer_function>>>
        methods;
    std::vector<uint32_t> method_indices;
};

/* Opaque memo table used while materializing one transfer graph. */
struct thread_transfer_context;

enum class ParamPatternResult {
    Matched,
    NotMatched,
    Unsupported
};

/* Enabled by default; --no-vm or SKEIN_VM=0 selects the AST backend. */
bool skein_vm_enabled();
/*
 * Turn the VM on or off.  This only affects work that has not been compiled
 * yet: functions with a cached Proto continue to run through the VM even
 * after the VM is disabled, because their source AST may already be gone.
 */
void skein_vm_set_enabled(bool enabled);

/* Enable/disable VM statistics and print the final report. */
void skein_vm_set_stats(bool enabled);
void skein_vm_stats_report();

/*
 * Execute a top-level statement through the VM.  Falls back to eval() when
 * the VM is disabled or the statement cannot be compiled safely.
 */
struct value_t *skein_vm_eval_statement(struct ast *statement);

/*
 * Try to execute a function body through the VM.  The caller must already
 * have pushed the function scope and bound the parameters.
 *
 * Returns false if the body could not be compiled and the caller should use
 * the AST interpreter instead.  Returns true when the VM ran; `out` then
 * contains the body result/control value (possibly NULL after an error).
 */
bool skein_vm_try_execute_function_body(
    const std::shared_ptr<function_def> &def, struct value_t *&out);

/* Try to evaluate one default-parameter expression in the current scope. */
bool skein_vm_try_eval_default(const std::shared_ptr<function_def> &def,
                               int index, struct value_t *&out);

/*
 * Match one destructuring parameter using its compiled descriptor.
 *
 * `Unsupported` means the pattern has not been compiled (or compilation
 * failed); the caller should fall back to the AST matcher.  `NotMatched`
 * leaves any partial bindings in `bindings`; the caller owns and frees them.
 */
ParamPatternResult skein_vm_match_parameter_pattern(
    const std::shared_ptr<function_def> &def, int index,
    struct value_t *value, std::vector<pattern_binding> &bindings);

/* Drop cached bytecode before a function_code is freed. */
void skein_vm_forget_function_code(struct function_code *code);
/* Clear VM-side function caches before runtime/type teardown. */
void skein_vm_shutdown();

/*
 * Prepare an immutable Proto for a thread entry function.
 * The function must be a synchronous, one-parameter function whose body can
 * run without the parent's global scope.  Captured upvalues are carried by
 * value and re-bound in the child runtime.
 */
bool skein_vm_prepare_thread_proto(
    const struct value_t *function, skein::bc::ProtoPtr &proto,
    std::string &error);

/* Prepare a function value for transfer as a child-side function.  Bodies
 * must not contain nested functions, defers, default/pattern/variadic
 * parameters, or methods. */
bool skein_vm_prepare_thread_helper(
    const struct value_t *function, skein::bc::ProtoPtr &proto,
    std::string &error);

/* Materialize a prepared transfer graph in the current (child) thread. */
std::shared_ptr<thread_transfer_context>
skein_vm_thread_transfer_context_create();
bool skein_vm_materialize_thread_value(
    const std::shared_ptr<thread_transfer_context> &context,
    const struct thread_transfer_value &value, struct value_t *&out,
    std::string &error);

/* Attach transferred methods and field defaults to registered types. */
bool skein_vm_apply_thread_types(
    const std::shared_ptr<thread_transfer_context> &context,
    const std::vector<struct thread_transfer_type> &types,
    std::string &error);

/* Serialize a prepared thread payload into a portable byte stream. */
bool skein_vm_serialize_thread_payload(
    const std::shared_ptr<struct thread_transfer_function> &entry,
    const std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    const std::vector<struct thread_transfer_module> &modules,
    std::string &bytes, std::string &error);

/* Deserialize a thread payload previously produced by
 * skein_vm_serialize_thread_payload. */
bool skein_vm_deserialize_thread_payload(
    const std::string &bytes,
    std::shared_ptr<struct thread_transfer_function> &entry,
    std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    std::vector<struct thread_transfer_module> &modules,
    std::vector<struct thread_transfer_type> &types,
    std::string &error);

#endif
