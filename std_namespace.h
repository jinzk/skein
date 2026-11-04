#ifndef SKEIN_STD_NAMESPACE_H
#define SKEIN_STD_NAMESPACE_H

#include <functional>
#include <string>

struct value_t;

namespace skein {
namespace stdlib {

using adapter_parser = std::function<bool(
    const std::string &source, const std::string &source_name,
    const std::string &expected_name, const std::string &module_name,
    value_t *&result, std::string &error)>;

/* Build the native std namespace.  Container modules are also copied under
 * std.raw so the adapter can call the original builtins. */
value_t *build_namespace();

/* Parse the private container adapter and replace selected builtin entries
 * with script wrappers that use language-level default parameters. */
bool install_adapters(value_t *root, const adapter_parser &parse,
                      std::string &error);

} // namespace stdlib
} // namespace skein

#endif
