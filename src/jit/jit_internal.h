#ifndef SKEIN_JIT_INTERNAL_H
#define SKEIN_JIT_INTERNAL_H

#include "bytecode.h"
#include "scopestack.h"
#include "symbol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

/*
 * Small helpers shared by the JIT code generator and its runtime helper
 * translation unit.  They stay header-only so the helper ABI remains a
 * plain C-callable boundary.
 */
namespace skein {
namespace jit {
namespace internal {

inline bool read_bool_env(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0)
        return false;
    return true;
}

inline void unpack_loc(uint64_t packed, int &line, int &column) {
    line = static_cast<int>(packed >> 32);
    column = static_cast<int>(packed & 0xffffffffu);
}

inline symbol *resolve_global_symbol(uint32_t slot, const char *name) {
    if (!name)
        return nullptr;
    scope_context &scope = active_scope_context();
    if (slot < scope.global_slots.size()) {
        symbol *candidate = scope.global_slots[slot];
        if (candidate && candidate->name &&
            std::strcmp(candidate->name, name) == 0)
            return candidate;
    }
    return lookupsym(name);
}

inline symbol *resolve_global_symbol(const skein::bc::Proto *proto,
                                     uint32_t slot, uint32_t name_index) {
    if (!proto || name_index >= proto->consts.size())
        return nullptr;
    return resolve_global_symbol(
        slot, proto->consts[name_index].name.c_str());
}

} // namespace internal
} // namespace jit
} // namespace skein

#endif
