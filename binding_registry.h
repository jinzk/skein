#ifndef SKEIN_BINDING_REGISTRY_H
#define SKEIN_BINDING_REGISTRY_H

#include "skein_module.h"
#include "valuetype.h"

#include <memory>
#include <string>

struct dynamic_library;

namespace skein {

/* Register a native module.  Static bindings pass a null library. */
bool register_module(const skein_module *module,
                     std::shared_ptr<dynamic_library> library,
                     std::string &error);

bool module_registered(const std::string &name);

/* dlopen a plugin and register its `skein_module_descriptor`.  Returns the
 * suggested import path. */
bool load_dynamic_module(const std::string &path, std::string &import_path,
                         std::string &error);

/* Build the script-visible namespace for a registered module.  Runs the
 * module's init hook once. */
struct value_t *import_module(const std::string &name, std::string &error);

} // namespace skein

#endif
