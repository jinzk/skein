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

/* -------------------------------------------------------------------------
 * packed / bitfield example: generic FFI rejects these, a C++ binding can
 * expose them safely through get_field/set_field.
 * ---------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint8_t tag;
    uint32_t length;
    uint16_t flags;
} packed_header_data;
#pragma pack(pop)

typedef struct {
    unsigned a : 1;
    unsigned b : 3;
    unsigned c : 4;
} packed_flags_data;

static_assert(sizeof(packed_header_data) == 7, "packed header layout");

const char *packed_header_describe(void *data) {
    (void)data;
    return "<PackedHeader>";
}

void packed_header_destroy(void *data) { std::free(data); }

skein_value *packed_header_get_field(skein_context *ctx, void *data,
                                     const char *name) {
    packed_header_data *header = (packed_header_data *)data;
    if (!name)
        return NULL;
    if (std::strcmp(name, "tag") == 0)
        return g_api->new_int(ctx, header->tag);
    if (std::strcmp(name, "length") == 0) {
        uint32_t length = 0;
        std::memcpy(&length, &header->length, sizeof length);
        return g_api->new_int(ctx, length);
    }
    if (std::strcmp(name, "flags") == 0) {
        uint16_t flags = 0;
        std::memcpy(&flags, &header->flags, sizeof flags);
        return g_api->new_int(ctx, flags);
    }
    g_api->error(ctx, "type", "PackedHeader has no such field");
    return NULL;
}

skein_status packed_header_set_field(skein_context *ctx, void *data,
                                     const char *name,
                                     const skein_value *value) {
    packed_header_data *header = (packed_header_data *)data;
    int64_t number = 0;
    if (!g_api->as_int(value, &number))
        return g_api->error(ctx, "type", "PackedHeader fields expect integers");
    if (!name)
        return g_api->error(ctx, "type", "PackedHeader has no such field");
    if (std::strcmp(name, "tag") == 0) {
        header->tag = (uint8_t)number;
        return SKEIN_OK;
    }
    if (std::strcmp(name, "length") == 0) {
        uint32_t length = (uint32_t)number;
        std::memcpy(&header->length, &length, sizeof length);
        return SKEIN_OK;
    }
    if (std::strcmp(name, "flags") == 0) {
        uint16_t flags = (uint16_t)number;
        std::memcpy(&header->flags, &flags, sizeof flags);
        return SKEIN_OK;
    }
    return g_api->error(ctx, "type", "PackedHeader has no such field");
}

void packed_header_visit_children(void *data, void *ctx, skein_visit_fn fn) {
    (void)data;
    (void)ctx;
    (void)fn;
}

const skein_native_type packed_header_type = {
    "PackedHeader", packed_header_describe, packed_header_destroy,
    packed_header_get_field, packed_header_set_field,
    packed_header_visit_children,
};

const char *packed_flags_describe(void *data) {
    (void)data;
    return "<PackedFlags>";
}

void packed_flags_destroy(void *data) { std::free(data); }

skein_value *packed_flags_get_field(skein_context *ctx, void *data,
                                    const char *name) {
    packed_flags_data *flags = (packed_flags_data *)data;
    if (!name)
        return NULL;
    if (std::strcmp(name, "a") == 0)
        return g_api->new_int(ctx, flags->a);
    if (std::strcmp(name, "b") == 0)
        return g_api->new_int(ctx, flags->b);
    if (std::strcmp(name, "c") == 0)
        return g_api->new_int(ctx, flags->c);
    g_api->error(ctx, "type", "PackedFlags has no such field");
    return NULL;
}

skein_status packed_flags_set_field(skein_context *ctx, void *data,
                                    const char *name,
                                    const skein_value *value) {
    packed_flags_data *flags = (packed_flags_data *)data;
    int64_t number = 0;
    if (!g_api->as_int(value, &number))
        return g_api->error(ctx, "type", "PackedFlags fields expect integers");
    if (!name)
        return g_api->error(ctx, "type", "PackedFlags has no such field");
    if (std::strcmp(name, "a") == 0) {
        flags->a = (unsigned)number & 1u;
        return SKEIN_OK;
    }
    if (std::strcmp(name, "b") == 0) {
        flags->b = (unsigned)number & 7u;
        return SKEIN_OK;
    }
    if (std::strcmp(name, "c") == 0) {
        flags->c = (unsigned)number & 15u;
        return SKEIN_OK;
    }
    return g_api->error(ctx, "type", "PackedFlags has no such field");
}

void packed_flags_visit_children(void *data, void *ctx, skein_visit_fn fn) {
    (void)data;
    (void)ctx;
    (void)fn;
}

const skein_native_type packed_flags_type = {
    "PackedFlags", packed_flags_describe, packed_flags_destroy,
    packed_flags_get_field, packed_flags_set_field,
    packed_flags_visit_children,
};

skein_status packed_header_new(skein_context *ctx,
                               const skein_value *const *args, size_t argc,
                               skein_value **result) {
    if (argc != 3)
        return g_api->error(ctx, "type", "packed_header_new expects 3 arguments");
    int64_t tag = 0;
    int64_t length = 0;
    int64_t flags = 0;
    if (!g_api->as_int(args[0], &tag) || !g_api->as_int(args[1], &length) ||
        !g_api->as_int(args[2], &flags))
        return g_api->error(ctx, "type", "packed_header_new expects integers");
    packed_header_data *header =
        (packed_header_data *)std::malloc(sizeof(packed_header_data));
    if (!header)
        return g_api->error(ctx, "runtime", "out of memory");
    header->tag = (uint8_t)tag;
    uint32_t length32 = (uint32_t)length;
    uint16_t flags16 = (uint16_t)flags;
    std::memcpy(&header->length, &length32, sizeof length32);
    std::memcpy(&header->flags, &flags16, sizeof flags16);
    *result = g_api->new_userdata(ctx, &packed_header_type, header);
    if (!*result) {
        std::free(header);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}

skein_status packed_header_size(skein_context *ctx,
                                const skein_value *const *args, size_t argc,
                                skein_value **result) {
    (void)args;
    if (argc != 0)
        return g_api->error(ctx, "type", "packed_header_size expects no arguments");
    *result = g_api->new_int(ctx, (int64_t)sizeof(packed_header_data));
    return SKEIN_OK;
}

skein_status packed_flags_new(skein_context *ctx,
                              const skein_value *const *args, size_t argc,
                              skein_value **result) {
    if (argc != 3)
        return g_api->error(ctx, "type", "packed_flags_new expects 3 arguments");
    int64_t a = 0;
    int64_t b = 0;
    int64_t c = 0;
    if (!g_api->as_int(args[0], &a) || !g_api->as_int(args[1], &b) ||
        !g_api->as_int(args[2], &c))
        return g_api->error(ctx, "type", "packed_flags_new expects integers");
    packed_flags_data *flags =
        (packed_flags_data *)std::malloc(sizeof(packed_flags_data));
    if (!flags)
        return g_api->error(ctx, "runtime", "out of memory");
    flags->a = (unsigned)a & 1u;
    flags->b = (unsigned)b & 7u;
    flags->c = (unsigned)c & 15u;
    *result = g_api->new_userdata(ctx, &packed_flags_type, flags);
    if (!*result) {
        std::free(flags);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}


const skein_native_type *const module_types[] = {
    &counter_type,
    &packed_header_type,
    &packed_flags_type,
};

const skein_function module_functions[] = {
    {"add", add_fn},
    {"join", join_fn},
    {"counter_new", counter_new},
    {"packed_header_new", packed_header_new},
    {"packed_header_size", packed_header_size},
    {"packed_flags_new", packed_flags_new},
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
