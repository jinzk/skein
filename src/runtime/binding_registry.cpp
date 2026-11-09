#include "binding_registry.h"

#include "dict.h"
#include "dynamic_library.h"
#include "runtime_context.h"
#include "symbol.h"
#include "value_ops.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skein {
namespace {

/* -------------------------------------------------------------------------
 * Value API
 * ---------------------------------------------------------------------- */

const value_t *as_value(const skein_value *value) {
    return reinterpret_cast<const value_t *>(value);
}

value_t *as_value(skein_value *value) {
    return reinterpret_cast<value_t *>(value);
}

skein_value *as_handle(value_t *value) {
    return reinterpret_cast<skein_value *>(value);
}

skein_status api_error(skein_context *ctx, const char *kind,
                       const char *message) {
    (void)ctx;
    (void)kind;
    yyerror("%s", message ? message : "native module error");
    return SKEIN_ERROR;
}

bool api_is_null(const skein_value *value) {
    return value && as_value(value)->type == T_null;
}

bool api_as_bool(const skein_value *value, bool *out) {
    if (!value || !out || as_value(value)->type != T_bool)
        return false;
    *out = ((const bool_value_t *)as_value(value))->value;
    return true;
}

bool api_as_int(const skein_value *value, int64_t *out) {
    if (!value || !out || as_value(value)->type != T_int)
        return false;
    *out = (int64_t)((const int_value_t *)as_value(value))->value;
    return true;
}

bool api_as_float(const skein_value *value, double *out) {
    if (!value || !out)
        return false;
    const value_t *v = as_value(value);
    if (v->type != T_int && v->type != T_float)
        return false;
    *out = as_double(v);
    return true;
}

const char *api_as_string(const skein_value *value) {
    if (!value || as_value(value)->type != T_string)
        return NULL;
    return ((const string_value_t *)as_value(value))->value;
}

const void *api_as_bytes(const skein_value *value, size_t *len) {
    if (!value || as_value(value)->type != T_bytes)
        return NULL;
    const std::string &bytes = ((const bytes_value_t *)as_value(value))->value;
    if (len)
        *len = bytes.size();
    return bytes.data();
}

skein_value *api_new_null(skein_context *ctx) {
    (void)ctx;
    return as_handle(newnull());
}

skein_value *api_new_bool(skein_context *ctx, bool value) {
    (void)ctx;
    return as_handle(newboolval(value));
}

skein_value *api_new_int(skein_context *ctx, int64_t value) {
    (void)ctx;
    return as_handle(newintval((long long)value));
}

skein_value *api_new_float(skein_context *ctx, double value) {
    (void)ctx;
    return as_handle(newfloatval(value));
}

skein_value *api_new_string(skein_context *ctx, const char *value) {
    (void)ctx;
    return as_handle(newstrval(value ? value : ""));
}

skein_value *api_new_bytes(skein_context *ctx, const void *data, size_t len) {
    (void)ctx;
    if (len > 0 && !data)
        return NULL;
    return as_handle(newbytesval(std::string((const char *)data, len)));
}

struct c_userdata {
    const skein_native_type *type = nullptr;
    void *data = nullptr;
    std::shared_ptr<void> module_owner;

    ~c_userdata() {
        if (type && type->destroy)
            type->destroy(data);
    }
};

thread_local std::shared_ptr<void> current_module_owner;

struct module_owner_guard {
    std::shared_ptr<void> previous;

    explicit module_owner_guard(std::shared_ptr<void> owner)
        : previous(std::move(current_module_owner)) {
        current_module_owner = std::move(owner);
    }
    ~module_owner_guard() {
        current_module_owner = std::move(previous);
    }
};

std::string c_describe(const void *payload) {
    const c_userdata *userdata = (const c_userdata *)payload;
    if (!userdata || !userdata->type)
        return "<userdata>";
    if (userdata->type->describe) {
        const char *text = userdata->type->describe(userdata->data);
        if (text)
            return text;
    }
    return userdata->type->name ? userdata->type->name : "<userdata>";
}

value_t *c_get_field(void *payload, const char *name) {
    c_userdata *userdata = (c_userdata *)payload;
    if (!userdata || !userdata->type || !userdata->type->get_field) {
        yyerror("userdata does not have named fields");
        return NULL;
    }
    return as_value(userdata->type->get_field(
        skein_context_current(), userdata->data, name));
}

bool c_set_field(void *payload, const char *name, value_t *value) {
    c_userdata *userdata = (c_userdata *)payload;
    if (!userdata || !userdata->type || !userdata->type->set_field) {
        yyerror("userdata fields are read-only");
        return false;
    }
    return userdata->type->set_field(skein_context_current(),
                                     userdata->data, name,
                                     as_handle(value)) == SKEIN_OK;
}

struct visit_bridge {
    const std::function<void(const value_t *)> *fn = nullptr;
};

void visit_thunk(void *ctx, const skein_value *value) {
    visit_bridge *bridge = (visit_bridge *)ctx;
    if (bridge && bridge->fn)
        (*bridge->fn)(as_value(value));
}

void c_for_each_child(
    const void *payload,
    const std::function<void(const value_t *)> &fn) {
    const c_userdata *userdata = (const c_userdata *)payload;
    if (!userdata || !userdata->type || !userdata->type->visit_children)
        return;
    visit_bridge bridge;
    bridge.fn = &fn;
    userdata->type->visit_children(userdata->data, &bridge, visit_thunk);
}

std::mutex &registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const skein_native_type *, std::unique_ptr<native_type>>
    &type_metas() {
    static std::unordered_map<const skein_native_type *,
                              std::unique_ptr<native_type>>
        metas;
    return metas;
}

const native_type *meta_for(const skein_native_type *type) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::unique_ptr<native_type> &slot = type_metas()[type];
    if (!slot) {
        slot = std::make_unique<native_type>();
        slot->name = (type && type->name) ? type->name : "native";
        slot->describe = c_describe;
        slot->get_field = c_get_field;
        slot->set_field = c_set_field;
        slot->for_each_child = c_for_each_child;
    }
    return slot.get();
}

skein_value *api_new_userdata(skein_context *ctx,
                              const skein_native_type *type, void *data) {
    (void)ctx;
    if (!type) {
        api_error(ctx, "type", "new_userdata requires a type");
        return NULL;
    }
    std::shared_ptr<c_userdata> payload = std::make_shared<c_userdata>();
    payload->type = type;
    payload->data = data;
    payload->module_owner = current_module_owner;
    return as_handle(newuserdataval(meta_for(type), payload));
}

size_t api_list_len(const skein_value *value) {
    if (!value || as_value(value)->type != T_list)
        return 0;
    return ((const list_value_t *)as_value(value))->payload->items.size();
}

skein_value *api_list_get(const skein_value *value, size_t index) {
    if (!value || as_value(value)->type != T_list)
        return NULL;
    const list_value_t *list = (const list_value_t *)as_value(value);
    if (index >= list->payload->items.size())
        return NULL;
    return as_handle(list->payload->items[index]);
}

bool api_list_push(skein_context *ctx, skein_value *list,
                   skein_value *value) {
    (void)ctx;
    if (!list || !value || as_value(list)->type != T_list)
        return false;
    ((list_value_t *)as_value(list))
        ->payload->items.push_back(as_value(value));
    return true;
}

size_t api_dict_len(const skein_value *value) {
    if (!value || as_value(value)->type != T_dict)
        return 0;
    return ((const dict_value_t *)as_value(value))->payload->items.size();
}

skein_value *api_dict_get(const skein_value *value, const char *key) {
    if (!value || !key || as_value(value)->type != T_dict)
        return NULL;
    const dict_value_t *dict = (const dict_value_t *)as_value(value);
    for (const auto &entry : dict->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((const string_value_t *)entry.first)->value, key) ==
                0)
            return as_handle(entry.second);
    }
    return NULL;
}

bool api_dict_set(skein_context *ctx, skein_value *dict, const char *key,
                  skein_value *value) {
    (void)ctx;
    if (!dict || !key || !value || as_value(dict)->type != T_dict)
        return false;
    dict_value_t *target = (dict_value_t *)as_value(dict);
    for (auto &entry : target->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((const string_value_t *)entry.first)->value, key) ==
                0) {
            freeval(entry.second);
            entry.second = as_value(value);
            dict_invalidate_index(target);
            return true;
        }
    }
    dict_append_item(target, newstrval(key), as_value(value));
    return true;
}

skein_value *api_dup_value(skein_context *ctx, const skein_value *value) {
    (void)ctx;
    return as_handle(dupval(const_cast<value_t *>(as_value(value))));
}

void api_free_value(skein_context *ctx, skein_value *value) {
    (void)ctx;
    freeval(as_value(value));
}

skein_value *api_call(skein_context *ctx, const skein_value *callee,
                      const skein_value *const *args, size_t argc) {
    (void)ctx;
    std::vector<value_t *> call_args;
    call_args.reserve(argc);
    for (size_t i = 0; i < argc; ++i)
        call_args.push_back(dupval(const_cast<value_t *>(as_value(args[i]))));
    return as_handle(
        skein_call_function(const_cast<value_t *>(as_value(callee)),
                            std::move(call_args)));
}

skein_value *api_new_list(skein_context *ctx) {
    (void)ctx;
    return as_handle((value_t *)newlistval());
}

skein_value *api_new_dict(skein_context *ctx) {
    (void)ctx;
    return as_handle((value_t *)newdictval());
}

bool api_as_userdata(const skein_value *value,
                     const skein_native_type *type, void **data) {
    if (!value || !type || as_value(value)->type != T_userdata)
        return false;
    const userdata_value_t *userdata =
        (const userdata_value_t *)as_value(value);
    if (userdata->meta != meta_for(type))
        return false;
    const c_userdata *payload = (const c_userdata *)userdata->data.get();
    if (!payload || payload->type != type)
        return false;
    if (data)
        *data = payload->data;
    return true;
}

const skein_api *api_table() {
    static const skein_api api = {
        SKEIN_MODULE_ABI_VERSION,
        sizeof(skein_api),
        api_error,
        api_is_null,
        api_as_bool,
        api_as_int,
        api_as_float,
        api_as_string,
        api_as_bytes,
        api_new_null,
        api_new_bool,
        api_new_int,
        api_new_float,
        api_new_string,
        api_new_bytes,
        api_new_userdata,
        api_list_len,
        api_list_get,
        api_list_push,
        api_dict_len,
        api_dict_get,
        api_dict_set,
        api_dup_value,
        api_free_value,
        api_call,
        api_new_list,
        api_new_dict,
        api_as_userdata,
    };
    return &api;
}

/* -------------------------------------------------------------------------
 * Registry
 * ---------------------------------------------------------------------- */

struct module_state {
    const skein_module *module = nullptr;
    std::shared_ptr<dynamic_library> library;
    std::mutex init_mutex;
    bool initialized = false;
    bool finished = false;

    ~module_state() {
        if (!finished && initialized && module && module->fini &&
            skein_context_current())
            module->fini(skein_context_current());
        finished = true;
    }
};

struct registered_module {
    std::string import_path;
    std::string internal_key;
    std::shared_ptr<module_state> state;
};

std::vector<registered_module> &modules() {
    static std::vector<registered_module> entries;
    return entries;
}

registered_module *find_module(const std::string &name) {
    for (registered_module &entry : modules()) {
        if (entry.import_path == name)
            return &entry;
    }
    return NULL;
}

value_t *constant_value(const skein_constant &constant) {
    switch (constant.kind) {
        case SKEIN_CONST_INT:
            return newintval((long long)constant.value.i);
        case SKEIN_CONST_FLOAT:
            return newfloatval(constant.value.f);
        case SKEIN_CONST_STRING:
            return newstrval(constant.value.s ? constant.value.s : "");
        case SKEIN_CONST_BOOL:
            return newboolval(constant.value.b);
        case SKEIN_CONST_NULL:
        default:
            return newnull();
    }
}

value_t *build_namespace(const std::shared_ptr<module_state> &state) {
    dict_value_t *namespace_value = newdictval();
    namespace_value->payload->is_namespace = true;

    const skein_module *module = state->module;
    for (size_t i = 0; i < module->function_count; ++i) {
        const skein_function &function = module->functions[i];
        if (!function.name || !function.call)
            continue;
        value_t *native = newnativeval(
            [state, call = function.call](
                const std::vector<value_t *> &args) -> value_t * {
                module_owner_guard guard(state);
                std::vector<const skein_value *> call_args;
                call_args.reserve(args.size());
                for (value_t *arg : args)
                    call_args.push_back(as_handle(arg));
                skein_value *result = NULL;
                skein_status status = call(
                    skein_context_current(),
                    call_args.empty() ? NULL : call_args.data(),
                    call_args.size(), &result);
                if (status != SKEIN_OK)
                    return NULL;
                return as_value(result);
            },
            state);
        dict_append_item(namespace_value, newstrval(function.name), native);
    }

    for (size_t i = 0; i < module->constant_count; ++i) {
        const skein_constant &constant = module->constants[i];
        if (!constant.name)
            continue;
        dict_append_item(namespace_value, newstrval(constant.name),
                         constant_value(constant));
    }

    return (value_t *)namespace_value;
}

} // namespace

bool register_module(const skein_module *module,
                     std::shared_ptr<dynamic_library> library,
                     std::string &error) {
    if (!module) {
        error = "native module descriptor is null";
        return false;
    }
    if (module->abi_version != SKEIN_MODULE_ABI_VERSION) {
        error = "native module ABI version mismatch";
        return false;
    }
    if (!module->name || !*module->name) {
        error = "native module name is required";
        return false;
    }
    if (std::strcmp(module->name, "std") == 0 ||
        std::strcmp(module->name, "main") == 0) {
        error = std::string("native module name \"") + module->name +
                "\" is reserved";
        return false;
    }

    registered_module entry;
    entry.import_path = module->name;
    const bool dynamic = library != nullptr;
    entry.internal_key = dynamic ? std::string("dyn:") + module->name
                                 : std::string("static:") + module->name;
    entry.state = std::make_shared<module_state>();
    entry.state->module = module;
    entry.state->library = std::move(library);

    std::lock_guard<std::mutex> lock(registry_mutex());
    if (find_module(module->name)) {
        error = std::string("duplicate native module ") + module->name;
        return false;
    }
    modules().push_back(std::move(entry));
    return true;
}

bool module_registered(const std::string &name) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return find_module(name) != NULL;
}

bool load_dynamic_module(const std::string &path, std::string &import_path,
                         std::string &error) {
    std::shared_ptr<dynamic_library> library =
        dynamic_library::open(path, error);
    if (!library)
        return false;

    void *symbol = library->symbol("skein_module_descriptor", error);
    if (!symbol) {
        error = "missing skein_module_descriptor: " + error;
        return false;
    }
    using descriptor_fn = const skein_module *(*)();
    descriptor_fn descriptor = (descriptor_fn)symbol;
    const skein_module *module = descriptor();
    if (!module) {
        error = "skein_module_descriptor returned null";
        return false;
    }
    if (!register_module(module, library, error))
        return false;
    import_path = module->name;
    return true;
}

struct value_t *import_module(const std::string &name, std::string &error) {
    std::shared_ptr<module_state> state;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registered_module *entry = find_module(name);
        if (!entry) {
            error = "native module not found: " + name;
            return NULL;
        }
        state = entry->state;
    }

    {
        std::lock_guard<std::mutex> init_lock(state->init_mutex);
        if (!state->initialized) {
            module_owner_guard guard(state);
            if (state->module->init &&
                state->module->init(skein_context_current(), api_table()) !=
                    SKEIN_OK) {
                error = "native module " + name + " failed to initialize";
                return NULL;
            }
            state->initialized = true;
        }
    }
    return build_namespace(state);
}

bool unload_module(const std::string &name, std::string &error) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::vector<registered_module> &entries = modules();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->import_path != name)
            continue;
        if (!it->state->library) {
            error = "cannot unload a static native module";
            return false;
        }
        entries.erase(it);
        return true;
    }
    error = "native module not found: " + name;
    return false;
}

} // namespace skein
