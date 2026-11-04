/* Dynamic test module for the native module ABI. */
#include "skein_module.h"

namespace {

const skein_api *g_api = nullptr;

skein_status twice_fn(skein_context *ctx, const skein_value *const *args,
                      size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "twice expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "twice expects an integer");
    *result = g_api->new_int(ctx, value * 2);
    return SKEIN_OK;
}

struct counter {
    int64_t value = 0;
};

void counter_destroy(void *data) { delete static_cast<counter *>(data); }

const skein_native_type counter_type = {
    "Counter", nullptr, counter_destroy, nullptr, nullptr, nullptr};

skein_status counter_new(skein_context *ctx, const skein_value *const *args,
                         size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "counter_new expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "counter_new expects an integer");
    counter *data = new counter();
    data->value = value;
    *result = g_api->new_userdata(ctx, &counter_type, data);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status counter_value(skein_context *ctx, const skein_value *const *args,
                           size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "counter_value expects 1 argument");
    void *data = nullptr;
    if (!g_api->as_userdata(args[0], &counter_type, &data))
        return g_api->error(ctx, "type", "counter_value expects a Counter");
    *result = g_api->new_int(ctx, static_cast<counter *>(data)->value);
    return SKEIN_OK;
}

skein_status list_pair(skein_context *ctx, const skein_value *const *args,
                       size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "list_pair expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "list_pair expects an integer");
    skein_value *list = g_api->new_list(ctx);
    if (!list)
        return SKEIN_ERROR;
    if (!g_api->list_push(ctx, list, g_api->new_int(ctx, value)) ||
        !g_api->list_push(ctx, list, g_api->new_int(ctx, value * 2))) {
        g_api->free_value(ctx, list);
        return SKEIN_ERROR;
    }
    *result = list;
    return SKEIN_OK;
}

skein_status dict_value(skein_context *ctx, const skein_value *const *args,
                        size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "dict_value expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "dict_value expects an integer");
    skein_value *dict = g_api->new_dict(ctx);
    if (!dict)
        return SKEIN_ERROR;
    if (!g_api->dict_set(ctx, dict, "value", g_api->new_int(ctx, value))) {
        g_api->free_value(ctx, dict);
        return SKEIN_ERROR;
    }
    *result = dict;
    return SKEIN_OK;
}

const skein_function functions[] = {
    {"twice", twice_fn},
    {"counter_new", counter_new},
    {"counter_value", counter_value},
    {"list_pair", list_pair},
    {"dict_value", dict_value},
};

const skein_native_type *const types[] = {&counter_type};

skein_status module_init(skein_context *ctx, const skein_api *api) {
    (void)ctx;
    g_api = api;
    return SKEIN_OK;
}

const skein_module module = {
    SKEIN_MODULE_ABI_VERSION,
    "test.dynamic",
    module_init,
    nullptr,
    functions,
    5,
    types,
    1,
    nullptr,
    0,
};

} // namespace

SKEIN_MODULE_EXPORT const skein_module *skein_module_descriptor(void) {
    return &module;
}
