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

const skein_function functions[] = {{"twice", twice_fn}};

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
    1,
    nullptr,
    0,
    nullptr,
    0,
};

} // namespace

SKEIN_MODULE_EXPORT const skein_module *skein_module_descriptor(void) {
    return &module;
}
