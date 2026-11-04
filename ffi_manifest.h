#ifndef SKEIN_FFI_MANIFEST_H
#define SKEIN_FFI_MANIFEST_H

#include "ffi_types.h"
#include "valuetype.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ffi {

struct manifest_function {
    std::string name;
    std::string symbol;
    type_ptr result;
    std::vector<type_ptr> args;
    std::vector<std::size_t> consumes;
    std::vector<std::size_t> retains;
    bool returns_owned = false;
    bool variadic = false;
    std::string calling_convention = "c";
    std::string free_function;
};

struct manifest_variable {
    std::string name;
    std::string symbol;
    type_ptr type;
    bool writable = true;
};

struct manifest {
    int version = 1;
    std::string name;
    std::map<std::string, type_ptr> types;
    std::map<std::string, manifest_function> functions;
    std::map<std::string, manifest_variable> variables;
    std::map<std::string, struct value_t *> constants;

    ~manifest();
};

extern const native_type manifest_native_type;

/* Accepts a dict descriptor, a JSON file path, or an existing manifest. */
bool load_manifest(struct value_t *spec,
                   std::shared_ptr<const manifest> &out, std::string &error);

/* Merge a new descriptor into an existing manifest.  Existing declarations
 * cannot be redefined. */
bool extend_manifest(const manifest &base, struct value_t *spec,
                     std::shared_ptr<const manifest> &out,
                     std::string &error);

struct value_t *new_manifest_value(std::shared_ptr<const manifest> manifest);
bool get_manifest(struct value_t *value,
                  std::shared_ptr<const manifest> &out);

} // namespace ffi

#endif
