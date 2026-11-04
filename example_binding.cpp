/*
 * Example static binding for the native module ABI.  It is written in C
 * style on purpose: a binding only needs skein_module.h.
 */
#include "skein_module.h"

#include <cstdlib>
#include <cstring>

namespace {

const skein_api *g_api = nullptr;

skein_status add_fn(skein_context *ctx, const skein_value *const *args,
                    size_t argc, skein_value **result) {
    if (argc != 2)
        return g_api->error(ctx, "type", "add expects 2 arguments");
    int64_t left = 0;
    int64_t right = 0;
    if (!g_api->as_int(args[0], &left) || !g_api->as_int(args[1], &right))
        return g_api->error(ctx, "type", "add expects integers");
    *result = g_api->new_int(ctx, left + right);
    return SKEIN_OK;
}

skein_status join_fn(skein_context *ctx, const skein_value *const *args,
                     size_t argc, skein_value **result) {
    if (argc != 2)
        return g_api->error(ctx, "type", "join expects 2 arguments");
    const char *left = g_api->as_string(args[0]);
    const char *right = g_api->as_string(args[1]);
    if (!left || !right)
        return g_api->error(ctx, "type", "join expects strings");
    size_t left_len = std::strlen(left);
    size_t right_len = std::strlen(right);
    char *buffer = (char *)std::malloc(left_len + right_len + 1);
    if (!buffer)
        return g_api->error(ctx, "runtime", "out of memory");
    std::memcpy(buffer, left, left_len);
    std::memcpy(buffer + left_len, right, right_len);
    buffer[left_len + right_len] = '\0';
    *result = g_api->new_string(ctx, buffer);
    std::free(buffer);
    return SKEIN_OK;
}

typedef struct {
    int64_t value;
} counter_data;

const char *counter_describe(void *data) {
    (void)data;
    return "<Counter>";
}

void counter_destroy(void *data) { std::free(data); }

skein_value *counter_get_field(skein_context *ctx, void *data,
                               const char *name) {
    counter_data *counter = (counter_data *)data;
    if (name && std::strcmp(name, "value") == 0)
        return g_api->new_int(ctx, counter->value);
    g_api->error(ctx, "type", "Counter has no such field");
    return NULL;
}

skein_status counter_set_field(skein_context *ctx, void *data,
                               const char *name,
                               const skein_value *value) {
    counter_data *counter = (counter_data *)data;
    if (!name || std::strcmp(name, "value") != 0)
        return g_api->error(ctx, "type", "Counter has no such field");
    int64_t number = 0;
    if (!g_api->as_int(value, &number))
        return g_api->error(ctx, "type", "value expects an integer");
    counter->value = number;
    return SKEIN_OK;
}

void counter_visit_children(void *data, void *ctx, skein_visit_fn fn) {
    (void)data;
    (void)ctx;
    (void)fn;
}

const skein_native_type counter_type = {
    "Counter",
    counter_describe,
    counter_destroy,
    counter_get_field,
    counter_set_field,
    counter_visit_children,
};

const skein_native_type *const module_types[] = {&counter_type};

skein_status counter_new(skein_context *ctx, const skein_value *const *args,
                          size_t argc, skein_value **result) {
    int64_t initial = 0;
    if (argc > 1)
        return g_api->error(ctx, "type", "counter_new expects 0 or 1 arguments");
    if (argc == 1 && !g_api->as_int(args[0], &initial))
        return g_api->error(ctx, "type", "counter_new expects an integer");
    counter_data *counter = (counter_data *)std::malloc(sizeof(counter_data));
    if (!counter)
        return g_api->error(ctx, "runtime", "out of memory");
    counter->value = initial;
    *result = g_api->new_userdata(ctx, &counter_type, counter);
    if (!*result) {
        std::free(counter);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}

const skein_function module_functions[] = {
    {"add", add_fn},
    {"join", join_fn},
    {"counter_new", counter_new},
};

skein_constant make_int_constant(const char *name, int64_t value) {
    skein_constant constant{};
    constant.name = name;
    constant.kind = SKEIN_CONST_INT;
    constant.value.i = value;
    return constant;
}

skein_constant make_float_constant(const char *name, double value) {
    skein_constant constant{};
    constant.name = name;
    constant.kind = SKEIN_CONST_FLOAT;
    constant.value.f = value;
    return constant;
}

skein_constant make_string_constant(const char *name, const char *value) {
    skein_constant constant{};
    constant.name = name;
    constant.kind = SKEIN_CONST_STRING;
    constant.value.s = value;
    return constant;
}

const skein_constant module_constants[] = {
    make_int_constant("ANSWER", 42),
    make_float_constant("PI", 3.14159),
    make_string_constant("NAME", "example"),
};

skein_status module_init(skein_context *ctx, const skein_api *api) {
    (void)ctx;
    g_api = api;
    return SKEIN_OK;
}

const skein_module module = {
    SKEIN_MODULE_ABI_VERSION,
    "example.math",
    module_init,
    NULL,
    module_functions,
    sizeof(module_functions) / sizeof(module_functions[0]),
    module_types,
    sizeof(module_types) / sizeof(module_types[0]),
    module_constants,
    sizeof(module_constants) / sizeof(module_constants[0]),
};

} // namespace

const skein_module *skein_example_module() { return &module; }
