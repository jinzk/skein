#include "ffi_check.h"

#include "dynamic_library.h"
#include "ffi_manifest.h"
#include "ffi_types.h"
#include "valuetype.h"

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ffi_check {
namespace {

struct value_holder {
    struct value_t *value = nullptr;

    ~value_holder() {
        if (value)
            freeval(value);
    }
};

} // namespace

int run_check(const std::string &library_path,
              const std::string &manifest_path, std::string &error) {
    struct value_t *path_value = newstrval(manifest_path.c_str());
    if (!path_value) {
        error = "cannot allocate the manifest path";
        return 1;
    }
    value_holder holder;
    holder.value = path_value;

    std::string load_error;
    std::shared_ptr<const ffi::manifest> manifest;
    if (!ffi::load_manifest(path_value, manifest, load_error)) {
        error = "invalid manifest: " + load_error;
        return 1;
    }

    std::string open_error;
    std::shared_ptr<dynamic_library> library =
        dynamic_library::open(library_path, open_error);
    if (!library) {
        error = "cannot load library: " + open_error;
        return 1;
    }

    std::vector<std::string> missing;
    std::set<std::string> checked;
    for (const auto &entry : manifest->functions) {
        const ffi::manifest_function &function = entry.second;
        std::string symbol_error;
        if (checked.insert(function.symbol).second &&
            !library->symbol(function.symbol.c_str(), symbol_error))
            missing.push_back(function.symbol);
        if (!function.returns_owned)
            continue;
        std::string free_name = function.free_function.empty()
                                    ? std::string("free")
                                    : function.free_function;
        if (!checked.insert(free_name).second)
            continue;
        if (!library->symbol(free_name.c_str(), symbol_error))
            missing.push_back(free_name);
    }

    std::printf("manifest: %s\n", manifest_path.c_str());
    std::printf("library:  %s\n", library_path.c_str());
    std::printf("types:    %zu\n", manifest->types.size());
    std::printf("functions: %zu\n", manifest->functions.size());
    std::printf("constants: %zu\n", manifest->constants.size());

    if (!missing.empty()) {
        for (const std::string &name : missing)
            std::fprintf(stderr, "missing symbol: %s\n", name.c_str());
        error = std::to_string(missing.size()) + " missing symbol(s)";
        return 1;
    }
    std::printf("all %zu symbols resolved\n", checked.size());
    return 0;
}

} // namespace ffi_check
