#ifndef SKEIN_VM_STATE_H
#define SKEIN_VM_STATE_H

#include "bytecode.h"
#include "valuetype.h"
#include "vm_eval.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct symbol;

enum class UnwindKind {
    Fail,
    Recover
};

struct handler_state {
    UnwindKind kind = UnwindKind::Fail;
    size_t target = 0;
    size_t stack_depth = 0;
    size_t scope_depth = 0;
    int handler_body_depth = 0;
    int eval_handler_depth = 0;
    bool capture = false;
    eval_capture_snapshot snapshot;
};

struct defer_record {
    std::shared_ptr<const skein::bc::Proto> proto;
    std::vector<std::shared_ptr<struct upvalue_cell>> upvalues;
};

struct iterator_state {
    value_t *iterable = NULL;
    size_t index = 0;
    size_t count = 0;
    bool generator = false;
    bool two_vars = false;
    bool active = false;
    bool direct = false;
    uint32_t key_slot = UINT32_MAX;
    uint32_t value_slot = UINT32_MAX;
    struct symbol *key_sym = NULL;
    struct symbol *value_sym = NULL;
};

struct NameCacheSlot {
    struct symbol *sym = nullptr;
    uint64_t generation = 0;
};

/*
 * Snapshot of the caller's execution state while a fast VM frame runs.
 * Fast frames are restricted to Protos without handlers, defers, iterators
 * or suspension points, so the saved vectors are usually empty.
 */
struct SavedVmState {
    const skein::bc::Proto *proto = nullptr;
    size_t pc = 0;
    size_t scope_mark = 0;
    size_t frame_stack_base = 0;
    int errors_before = 0;
    int saved_handler_depth = 0;
    int caller_handler_depth = 0;
    int handler_body_depth = 0;
    size_t diag_frames = 0;
    std::string diag_package;
    std::string diag_source;
    uint64_t name_scope_generation = 0;
    bool param_pool_safe = false;
    bool slot_pool_safe = false;
    bool direct_params = false;
    bool direct_slots = false;
    std::shared_ptr<function_def> def;
    std::vector<NameCacheSlot> name_cache;
    std::vector<struct symbol *> param_symbols;
    std::vector<value_t *> param_values;
    std::vector<struct symbol *> slot_symbols;
    std::vector<value_t *> slot_values;
    std::vector<uint8_t> slot_const;
    std::vector<std::shared_ptr<struct upvalue_cell>> open_upvalues;
    std::vector<iterator_state> iterators;
    std::vector<std::vector<defer_record>> defer_scopes;
    std::vector<handler_state> handlers;
};

#endif
