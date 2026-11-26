#include "typecheck_state.h"

namespace skein {
namespace typecheck {
namespace detail {

std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>
    g_import_aliases;

std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>
    g_selective_imports;

std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>
    g_selective_import_names;

std::unordered_map<std::string,
                   std::unordered_map<std::string, VarInfo>>
    g_persistent_bindings;

std::unordered_map<std::string,
                   std::unordered_map<std::string, VarInfo>>
    g_exported_types;

std::unordered_map<std::string, const TypeNode *>
    g_generator_element_types;
std::unordered_map<std::string, const TypeNode *>
    g_generator_send_type_map;

std::unordered_map<std::string, std::vector<const TypeNode *>>
    g_generic_default_types;

} // namespace detail
} // namespace typecheck
} // namespace skein
