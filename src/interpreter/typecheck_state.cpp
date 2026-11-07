#include "typecheck_state.h"

namespace skein {
namespace typecheck {
namespace detail {

std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>
    g_import_aliases;

std::unordered_map<std::string,
                   std::unordered_map<std::string, VarInfo>>
    g_persistent_bindings;

std::unordered_map<std::string,
                   std::unordered_map<std::string, VarInfo>>
    g_exported_types;

} // namespace detail
} // namespace typecheck
} // namespace skein
