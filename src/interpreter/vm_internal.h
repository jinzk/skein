#ifndef SKEIN_VM_INTERNAL_H
#define SKEIN_VM_INTERNAL_H

#include "bytecode.h"
#include "pattern.h"
#include "valuetype.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * Shared VM statistics.  The executor increments these counters; the bridge
 * owns compilation timing and fallback reporting.
 */
struct vm_stats_data {
    std::atomic<bool> enabled{false};
    uint64_t statements_compiled = 0;
    uint64_t statements_fallback = 0;
    uint64_t functions_compiled = 0;
    uint64_t functions_fallback = 0;
    uint64_t instructions = 0;
    uint64_t calls = 0;
    uint64_t fast_calls = 0;
    uint64_t tail_calls = 0;
    uint64_t fallback_calls = 0;
    uint64_t name_ic_hits = 0;
    uint64_t name_ic_misses = 0;
    uint64_t member_ic_hits = 0;
    uint64_t member_ic_misses = 0;
    uint64_t index_ic_hits = 0;
    uint64_t index_ic_misses = 0;
    uint64_t max_depth = 0;
    uint64_t max_stack = 0;
    uint64_t max_handler_depth = 0;
    uint64_t max_defer_depth = 0;
    uint64_t max_iterator_depth = 0;
    uint64_t compile_ns = 0;
    uint64_t execute_ns = 0;
    std::unordered_map<std::string, uint64_t> fallback_reasons;
    std::array<uint64_t, static_cast<size_t>(skein::bc::Op::Count)> opcodes{};
};

extern vm_stats_data vm_stats;

/* Executor entry points used by the AST/compiler bridge. */
struct value_t *skein_vm_execute_proto(
    const skein::bc::Proto &proto,
    const std::shared_ptr<function_def> &def = nullptr);
bool skein_vm_match_pattern_proto(
    const skein::bc::Proto &proto, uint32_t index, struct value_t *value,
    std::vector<pattern_binding> &bindings);
int skein_vm_max_depth();
void skein_vm_ensure_stats_flags();

/* Compile/cache a function body.  Implemented by vm_bridge.cpp. */
skein::bc::ProtoPtr skein_vm_prepare_function_body(
    const std::shared_ptr<function_def> &def);

#endif
