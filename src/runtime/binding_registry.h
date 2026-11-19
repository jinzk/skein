#ifndef SKEIN_BINDING_REGISTRY_H
#define SKEIN_BINDING_REGISTRY_H

#include "skein_module.h"
#include "valuetype.h"

#include <memory>
#include <string>

struct dynamic_library;

namespace skein {

/* Module owning the native call currently executing on this thread.  Async
 * completions install it again so userdata created in a finish callback keeps
 * the plugin library alive. */
extern thread_local std::shared_ptr<void> current_module_owner;

struct module_owner_guard {
    std::shared_ptr<void> previous;

    explicit module_owner_guard(std::shared_ptr<void> owner)
        : previous(std::move(current_module_owner)) {
        current_module_owner = std::move(owner);
    }
    ~module_owner_guard() { current_module_owner = std::move(previous); }
};

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

/* Remove a dynamic module from the registry.  The library stays loaded until
 * the last bound function/userdata is released, then fini/dlclose run. */
bool unload_module(const std::string &name, std::string &error);

} // namespace skein

#endif
