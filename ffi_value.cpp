#include "ffi_value.h"

#include "ast.h"

#include <string>

namespace ffi {
namespace {

void library_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *)> &fn) {
    const library_handle *library = (const library_handle *)data;
    if (!library)
        return;
    for (const struct value_t *v : library->retained.values)
        fn(v);
}

std::string describe_library(const void *data) {
    const library_handle *handle = (const library_handle *)data;
    if (!handle || handle->closed || !handle->library ||
        !handle->library->valid())
        return "<ffi_library closed>";
    return "<ffi_library>";
}

std::string describe_buffer(const void *data) {
    const buffer_handle *buffer = (const buffer_handle *)data;
    if (!buffer || buffer->freed)
        return "<ffi_buffer freed>";
    return "<ffi_buffer " + std::to_string(buffer->size) + ">";
}

std::string describe_type(const void *data) {
    const type_handle *handle = (const type_handle *)data;
    if (!handle || !handle->type)
        return "<ffi_type>";
    return "<ffi_type " + handle->type->name + ">";
}

std::string describe_pointer(const void *data) {
    const pointer_handle *handle = (const pointer_handle *)data;
    if (!handle || !handle->address)
        return "<ffi_pointer null>";
    return "<ffi_pointer>";
}

std::string describe_object(const void *data) {
    const object_handle *handle = (const object_handle *)data;
    if (!handle || !handle->type)
        return "<ffi_struct>";
    return "<" + handle->type->name + " " +
           std::to_string(handle->type->size) + ">";
}

struct value_t *object_get_field(void *data, const char *name) {
    object_handle *object = (object_handle *)data;
    if (!object || !object->type || object->type->kind != kind_t::record) {
        yyerror("userdata does not have named fields");
        return NULL;
    }
    const field *f = find_field(*object->type, name ? name : "");
    if (!f) {
        yyerror("%s has no field %s", object->type->name.c_str(),
                name ? name : "");
        return NULL;
    }
    return read_value(f->type, object->address + f->offset,
                      object->owner, nullptr, object->keep);
}

bool object_set_field(void *data, const char *name, struct value_t *value) {
    object_handle *object = (object_handle *)data;
    if (!object || !object->type || object->type->kind != kind_t::record) {
        yyerror("userdata does not have named fields");
        return false;
    }
    const field *f = find_field(*object->type, name ? name : "");
    if (!f) {
        yyerror("%s has no field %s", object->type->name.c_str(),
                name ? name : "");
        return false;
    }
    std::string error;
    if (!write_value(f->type, object->address + f->offset, value,
                     *object->keep, error)) {
        yyerror("field %s: %s", name ? name : "", error.c_str());
        return false;
    }
    return true;
}

void object_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *)> &fn) {
    const object_handle *object = (const object_handle *)data;
    if (!object)
        return;
    if (!object->keep)
        return;
    for (const struct value_t *v : object->keep->values)
        fn(v);
}

void pointer_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *)> &fn) {
    const pointer_handle *pointer = (const pointer_handle *)data;
    if (!pointer)
        return;
    if (!pointer->keep)
        return;
    for (const struct value_t *v : pointer->keep->values)
        fn(v);
}

struct value_t *type_get_field(void *data, const char *name) {
    type_handle *handle = (type_handle *)data;
    if (!handle || !handle->type) {
        yyerror("invalid ffi type");
        return NULL;
    }
    const type &t = *handle->type;
    if (!name)
        name = "";
    if (std::string(name) == "name")
        return newstrval(t.name.c_str());
    if (std::string(name) == "size")
        return newintval((long long)t.size);
    if (std::string(name) == "align")
        return newintval((long long)t.align);
    if (std::string(name) == "kind")
        return newstrval(kind_name(t.kind));
    yyerror("ffi type has no field %s", name);
    return NULL;
}

} // namespace

const native_type library_native_type = {
    "ffi_library", describe_library, nullptr, library_get_field, nullptr,
    library_for_each_child};

const native_type buffer_native_type = {"ffi_buffer", describe_buffer};

const native_type type_native_type = {
    "ffi_type", describe_type, nullptr, type_get_field};

const native_type pointer_native_type = {
    "ffi_pointer", describe_pointer, nullptr, nullptr, nullptr,
    pointer_for_each_child};

const native_type object_native_type = {
    "ffi_struct", describe_object, nullptr, object_get_field,
    object_set_field, object_for_each_child};

const native_type array_native_type = {
    "ffi_array", describe_object, nullptr, nullptr, nullptr,
    object_for_each_child};

struct value_t *new_library_value(std::shared_ptr<dynamic_library> library,
                                  std::shared_ptr<const manifest> spec) {
    std::shared_ptr<library_handle> handle = std::make_shared<library_handle>();
    handle->library = std::move(library);
    handle->spec = std::move(spec);
    struct value_t *value = newuserdataval(&library_native_type, handle);
    handle->self = value;
    return value;
}

struct value_t *new_buffer_value(std::size_t size) {
    std::shared_ptr<buffer_handle> buffer = std::make_shared<buffer_handle>();
    buffer->size = size;
    /* Keep a non-null pointer for zero-length buffers so they can still be
     * passed to C functions that do not special-case a null data pointer. */
    buffer->bytes.resize(size ? size : 1, 0);
    return newuserdataval(&buffer_native_type, buffer);
}

struct value_t *new_type_value(type_ptr type) {
    std::shared_ptr<type_handle> handle = std::make_shared<type_handle>();
    handle->type = std::move(type);
    return newuserdataval(&type_native_type, handle);
}

struct value_t *new_pointer_value(type_ptr pointee, unsigned char *address,
                                  std::shared_ptr<void> owner,
                                  std::shared_ptr<keepalive> keep) {
    std::shared_ptr<pointer_handle> handle = std::make_shared<pointer_handle>();
    handle->pointee = std::move(pointee);
    handle->address = address;
    handle->owner = std::move(owner);
    handle->keep = keep ? std::move(keep) : std::make_shared<keepalive>();
    return newuserdataval(&pointer_native_type, handle);
}

struct value_t *new_object_value(type_ptr type, unsigned char *address,
                                 std::shared_ptr<void> owner,
                                 std::shared_ptr<keepalive> keep) {
    std::shared_ptr<object_handle> handle = std::make_shared<object_handle>();
    handle->type = std::move(type);
    handle->address = address;
    handle->owner = std::move(owner);
    handle->keep = keep ? std::move(keep) : std::make_shared<keepalive>();
    const native_type *meta = (handle->type && handle->type->kind ==
                                                   kind_t::array)
                                  ? &array_native_type
                                  : &object_native_type;
    return newuserdataval(meta, handle);
}

bool get_library(struct value_t *value,
                 std::shared_ptr<library_handle> &out) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &library_native_type) {
        yyerror("expected an ffi library, got %s", typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<library_handle>(
        ((userdata_value_t *)value)->data);
    if (!out || out->closed || !out->library || !out->library->valid()) {
        yyerror("ffi library is closed");
        return false;
    }
    return true;
}

bool get_buffer(struct value_t *value, std::shared_ptr<buffer_handle> &out) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &buffer_native_type) {
        yyerror("expected an ffi buffer, got %s", typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<buffer_handle>(
        ((userdata_value_t *)value)->data);
    if (!out || out->freed) {
        yyerror("ffi buffer is freed");
        return false;
    }
    return true;
}

bool get_type(struct value_t *value, type_ptr &out) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &type_native_type) {
        yyerror("expected an ffi type, got %s", typename_of(value));
        return false;
    }
    std::shared_ptr<type_handle> handle = std::static_pointer_cast<type_handle>(
        ((userdata_value_t *)value)->data);
    if (!handle || !handle->type) {
        yyerror("invalid ffi type");
        return false;
    }
    out = handle->type;
    return true;
}

bool get_pointer(struct value_t *value, std::shared_ptr<pointer_handle> &out) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &pointer_native_type) {
        yyerror("expected an ffi pointer, got %s", typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<pointer_handle>(
        ((userdata_value_t *)value)->data);
    if (!out) {
        yyerror("invalid ffi pointer");
        return false;
    }
    if (out->consumed) {
        yyerror("ffi pointer was consumed by C");
        return false;
    }
    return true;
}

bool get_object(struct value_t *value, std::shared_ptr<object_handle> &out) {
    if (!value || value->type != T_userdata) {
        yyerror("expected ffi struct data, got %s", typename_of(value));
        return false;
    }
    const native_type *meta = ((userdata_value_t *)value)->meta;
    if (meta != &object_native_type && meta != &array_native_type) {
        yyerror("expected ffi struct data, got %s", typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<object_handle>(
        ((userdata_value_t *)value)->data);
    if (!out || !out->type || !out->address) {
        yyerror("invalid ffi struct data");
        return false;
    }
    if (out->consumed) {
        yyerror("ffi value was consumed by C");
        return false;
    }
    return true;
}

bool resolve_type(struct value_t *value, type_ptr &out, std::string &error) {
    if (!value) {
        error = "missing ffi type";
        return false;
    }
    if (value->type == T_string) {
        const char *name = ((string_value_t *)value)->value;
        type_ptr scalar_type = scalar(name ? name : "");
        if (!scalar_type) {
            error = "unknown ffi type " + std::string(name ? name : "");
            return false;
        }
        out = std::move(scalar_type);
        return true;
    }
    if (value->type == T_userdata &&
        ((userdata_value_t *)value)->meta == &type_native_type) {
        std::shared_ptr<type_handle> handle =
            std::static_pointer_cast<type_handle>(
                ((userdata_value_t *)value)->data);
        if (!handle || !handle->type) {
            error = "invalid ffi type";
            return false;
        }
        out = handle->type;
        return true;
    }
    error = "expected an ffi type or type name, got " +
            std::string(typename_of(value));
    return false;
}

void mark_consumed(struct value_t *value) {
    if (!value || value->type != T_userdata)
        return;
    const userdata_value_t *userdata = (const userdata_value_t *)value;
    if (userdata->meta == &pointer_native_type) {
        std::static_pointer_cast<pointer_handle>(userdata->data)->consumed =
            true;
    } else if (userdata->meta == &object_native_type ||
               userdata->meta == &array_native_type) {
        std::static_pointer_cast<object_handle>(userdata->data)->consumed =
            true;
    }
}

void retain_value(const std::shared_ptr<library_handle> &library,
                  struct value_t *value) {
    if (!library || !value || value->type != T_userdata)
        return;
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (!userdata->data)
        return;
    library->retained.owners.push_back(userdata->data);
    library->retained.values.push_back(dupval(value));
}

} // namespace ffi
