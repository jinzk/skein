#include "ffi_module.h"

#include "ast.h"
#include "dynamic_library.h"
#include "eval_helpers.h"
#include "ffi_callbacks.h"
#include "ffi_manifest.h"
#include "ffi_marshal.h"
#include "ffi_types.h"
#include "ffi_value.h"
#include "structtype.h"
#include "valuetype.h"

#include <ffi.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using ffi::kind_t;
using ffi::type_ptr;

template <typename T>
T load_bytes(const unsigned char *src) {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

/* Strip a `pointer(T)` wrapper from a cast target. */
type_ptr cast_target(const type_ptr &type) {
    if (type && type->kind == kind_t::pointer && type->element)
        return type->element;
    return type;
}

std::vector<type_ptr> unwrap_types(const std::vector<value_t *> &items,
                                   std::string &error) {
    std::vector<type_ptr> out;
    out.reserve(items.size());
    for (value_t *item : items) {
        type_ptr type;
        if (!ffi::resolve_type(item, type, error))
            return {};
        out.push_back(std::move(type));
    }
    return out;
}

} // namespace

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

static value_t *ffi_struct(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.struct expects 2 arguments");
        return NULL;
    }
    std::string name;
    if (args[0]->type == T_string)
        name = ((string_value_t *)args[0])->value;
    else if (args[0]->type != T_null) {
        yyerror("ffi.struct name must be a string");
        return NULL;
    }

    std::vector<std::pair<std::string, type_ptr>> fields;
    std::string error;
    if (args[1]->type == T_dict) {
        for (const auto &entry :
             ((dict_value_t *)args[1])->payload->items) {
            if (entry.first->type != T_string) {
                yyerror("ffi.struct field names must be strings");
                return NULL;
            }
            type_ptr type;
            if (!ffi::resolve_type(entry.second, type, error)) {
                yyerror("ffi.struct: %s", error.c_str());
                return NULL;
            }
            fields.emplace_back(((string_value_t *)entry.first)->value,
                                std::move(type));
        }
    } else if (args[1]->type == T_list) {
        for (value_t *item : ((list_value_t *)args[1])->payload->items) {
            if (item->type != T_list) {
                yyerror("ffi.struct fields must be [name, type] pairs");
                return NULL;
            }
            auto &pair = ((list_value_t *)item)->payload->items;
            if (pair.size() != 2 || pair[0]->type != T_string) {
                yyerror("ffi.struct fields must be [name, type] pairs");
                return NULL;
            }
            type_ptr type;
            if (!ffi::resolve_type(pair[1], type, error)) {
                yyerror("ffi.struct: %s", error.c_str());
                return NULL;
            }
            fields.emplace_back(((string_value_t *)pair[0])->value,
                                std::move(type));
        }
    } else {
        yyerror("ffi.struct fields must be a dict or a list of pairs");
        return NULL;
    }

    type_ptr type;
    if (!ffi::make_record(name, fields, type, error)) {
        yyerror("ffi.struct: %s", error.c_str());
        return NULL;
    }
    return ffi::new_type_value(std::move(type));
}

static value_t *ffi_pointer(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.pointer expects 1 argument");
        return NULL;
    }
    std::string error;
    type_ptr pointee;
    if (!ffi::resolve_type(args[0], pointee, error)) {
        yyerror("ffi.pointer: %s", error.c_str());
        return NULL;
    }
    return ffi::new_type_value(ffi::make_pointer(std::move(pointee)));
}

static value_t *ffi_array(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.array expects 2 arguments");
        return NULL;
    }
    std::string error;
    type_ptr element;
    if (!ffi::resolve_type(args[0], element, error)) {
        yyerror("ffi.array: %s", error.c_str());
        return NULL;
    }
    if (args[1]->type != T_int ||
        ((int_value_t *)args[1])->value < 0) {
        yyerror("ffi.array length must be a non-negative integer");
        return NULL;
    }
    long long count = ((int_value_t *)args[1])->value;
    if ((unsigned long long)count >
        (unsigned long long)std::numeric_limits<std::size_t>::max()) {
        yyerror("ffi.array length is too large");
        return NULL;
    }
    type_ptr type;
    if (!ffi::make_array(std::move(element), (std::size_t)count, type,
                         error)) {
        yyerror("ffi.array: %s", error.c_str());
        return NULL;
    }
    return ffi::new_type_value(std::move(type));
}

static value_t *ffi_signature(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.signature expects 2 arguments");
        return NULL;
    }
    std::string error;
    type_ptr result;
    if (!ffi::resolve_type(args[0], result, error)) {
        yyerror("ffi.signature: %s", error.c_str());
        return NULL;
    }
    if (args[1]->type != T_list) {
        yyerror("ffi.signature arguments must be a list of types");
        return NULL;
    }
    std::vector<type_ptr> params =
        unwrap_types(((list_value_t *)args[1])->payload->items, error);
    if (params.size() != ((list_value_t *)args[1])->payload->items.size()) {
        yyerror("ffi.signature: %s", error.c_str());
        return NULL;
    }
    type_ptr type;
    if (!ffi::make_callback(std::move(result), std::move(params), type,
                            error)) {
        yyerror("ffi.signature: %s", error.c_str());
        return NULL;
    }
    return ffi::new_type_value(std::move(type));
}

static value_t *ffi_callback(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.callback expects 2 arguments");
        return NULL;
    }
    std::string error;
    type_ptr signature;
    if (!ffi::resolve_type(args[0], signature, error)) {
        yyerror("ffi.callback: %s", error.c_str());
        return NULL;
    }
    if (signature->kind != kind_t::callback) {
        yyerror("ffi.callback expects a callback signature");
        return NULL;
    }

    value_t *function = args[1];
    if (function->type == T_func) {
        func_value_t *callee = (func_value_t *)function;
        if (callee->def && callee->def->code &&
            (callee->def->code->is_async ||
             callee->def->code->is_generator)) {
            yyerror("ffi callbacks must be synchronous functions");
            return NULL;
        }
    } else if (function->type == T_method) {
        method_value_t *method = (method_value_t *)function;
        if (method->method && method->method->code &&
            (method->method->code->is_async ||
             method->method->code->is_generator)) {
            yyerror("ffi callbacks must be synchronous functions");
            return NULL;
        }
    } else if (function->type != T_builtin) {
        yyerror("ffi.callback expects a function, got %s",
                typename_of(function));
        return NULL;
    }

    value_t *value = ffi::new_callback_value(signature, function, error);
    if (!value)
        yyerror("ffi.callback: %s", error.c_str());
    return value;
}

/* -------------------------------------------------------------------------
 * Values
 * ---------------------------------------------------------------------- */

static value_t *ffi_new(const std::vector<value_t *> &args) {
    if (args.size() != 1 && args.size() != 2) {
        yyerror("std.ffi.new expects 1 or 2 arguments");
        return NULL;
    }
    std::string error;
    type_ptr type;
    if (!ffi::resolve_type(args[0], type, error)) {
        yyerror("ffi.new: %s", error.c_str());
        return NULL;
    }
    if (!ffi::is_aggregate(type->kind)) {
        yyerror("ffi.new expects a struct or array type");
        return NULL;
    }
    if (!ffi::is_value_type(*type)) {
        yyerror("ffi.new: %s cannot be created by value", type->name.c_str());
        return NULL;
    }

    std::shared_ptr<ffi::memory_block> block =
        std::make_shared<ffi::memory_block>(type->size, type->align);
    if (args.size() == 2 && args[1]->type != T_null) {
        if (!ffi::write_value(type, block->data, args[1], *block->keep,
                              error)) {
            yyerror("ffi.new: %s", error.c_str());
            return NULL;
        }
    }
    return ffi::new_object_value(std::move(type), block->data, block,
                                 block->keep);
}

static value_t *ffi_alloc(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.alloc expects 1 argument");
        return NULL;
    }
    std::string error;
    type_ptr type;
    if (!ffi::resolve_type(args[0], type, error)) {
        yyerror("ffi.alloc: %s", error.c_str());
        return NULL;
    }
    if (!ffi::is_value_type(*type) || type->kind == kind_t::void_) {
        yyerror("ffi.alloc: %s cannot be allocated by value",
                type->name.c_str());
        return NULL;
    }
    std::shared_ptr<ffi::memory_block> block =
        std::make_shared<ffi::memory_block>(type->size, type->align);
    return ffi::new_pointer_value(std::move(type), block->data, block,
                                  block->keep);
}

static value_t *ffi_ptr(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.ptr expects 1 argument");
        return NULL;
    }
    std::string error;
    type_ptr pointee;
    if (!ffi::resolve_type(args[0], pointee, error)) {
        yyerror("ffi.ptr: %s", error.c_str());
        return NULL;
    }
    return ffi::new_pointer_value(std::move(pointee), nullptr, nullptr);
}

static value_t *ffi_deref(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.deref expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi::pointer_handle> pointer;
    if (!ffi::get_pointer(args[0], pointer))
        return NULL;
    if (!pointer->address) {
        yyerror("ffi.deref: null pointer");
        return NULL;
    }
    if (!pointer->pointee) {
        yyerror("ffi.deref: pointer has no pointee type");
        return NULL;
    }
    std::string error;
    value_t *value =
        ffi::read_value(pointer->pointee,
                        (const unsigned char *)pointer->address,
                        pointer->owner, &error, pointer->keep);
    if (!value)
        yyerror("ffi.deref: %s", error.empty() ? "cannot read value"
                                               : error.c_str());
    return value;
}

static value_t *ffi_offset(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.offset expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::pointer_handle> pointer;
    if (!ffi::get_pointer(args[0], pointer))
        return NULL;
    if (!pointer->pointee) {
        yyerror("ffi.offset: pointer has no pointee type");
        return NULL;
    }
    if (args[1]->type != T_int) {
        yyerror("ffi.offset index must be an integer");
        return NULL;
    }
    long long index = ((int_value_t *)args[1])->value;
    if (!pointer->address) {
        if (index == 0)
            return ffi::new_pointer_value(pointer->pointee, nullptr,
                                          pointer->owner, pointer->keep);
        yyerror("ffi.offset: null pointer");
        return NULL;
    }
    const long long size = (long long)pointer->pointee->size;
    if (size <= 0 || (index > 0 && index > (long long)(INT64_MAX / size)) ||
        (index < 0 && index < (long long)(INT64_MIN / size))) {
        yyerror("ffi.offset: index is out of range");
        return NULL;
    }
    unsigned char *address =
        pointer->address + (std::ptrdiff_t)(index * size);
    return ffi::new_pointer_value(pointer->pointee, address, pointer->owner,
                                  pointer->keep);
}

static value_t *ffi_cast(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.cast expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::pointer_handle> pointer;
    if (!ffi::get_pointer(args[0], pointer))
        return NULL;
    std::string error;
    type_ptr type;
    if (!ffi::resolve_type(args[1], type, error)) {
        yyerror("ffi.cast: %s", error.c_str());
        return NULL;
    }
    if (type->kind == kind_t::void_) {
        yyerror("ffi.cast: cannot cast to void");
        return NULL;
    }
    return ffi::new_pointer_value(cast_target(type), pointer->address,
                                  pointer->owner, pointer->keep);
}

static value_t *ffi_get(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.get expects 2 arguments");
        return NULL;
    }
    value_t *container = args[0];
    if (container->type != T_userdata) {
        yyerror("ffi.get cannot index %s", typename_of(container));
        return NULL;
    }
    const native_type *meta = ((userdata_value_t *)container)->meta;

    if (meta == &ffi::object_native_type || meta == &ffi::array_native_type) {
        std::shared_ptr<ffi::object_handle> object;
        if (!ffi::get_object(container, object))
            return NULL;
        if (object->type && object->type->kind == kind_t::record) {
            if (args[1]->type != T_string) {
                yyerror("ffi.get field name must be a string");
                return NULL;
            }
            const char *name = ((string_value_t *)args[1])->value;
            const ffi::field *f = ffi::find_field(*object->type, name);
            if (!f) {
                yyerror("%s has no field %s", object->type->name.c_str(),
                        name);
                return NULL;
            }
            std::string error;
            value_t *value =
                ffi::read_value(f->type, object->address + f->offset,
                                object->owner, &error, object->keep);
            if (!value)
                yyerror("ffi.get: %s", error.c_str());
            return value;
        }
        if (args[1]->type != T_int) {
            yyerror("ffi.get index must be an integer");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        if (index < 0 ||
            (unsigned long long)index >= object->type->count) {
            yyerror("ffi.get: index %lld out of range", index);
            return NULL;
        }
        std::string error;
        value_t *value = ffi::read_value(
            object->type->element,
            object->address +
                (std::size_t)index * object->type->element->size,
            object->owner, &error, object->keep);
        if (!value)
            yyerror("ffi.get: %s", error.c_str());
        return value;
    }

    if (meta == &ffi::pointer_native_type) {
        std::shared_ptr<ffi::pointer_handle> pointer;
        if (!ffi::get_pointer(container, pointer))
            return NULL;
        if (!pointer->address || !pointer->pointee) {
            yyerror("ffi.get: null pointer or missing pointee type");
            return NULL;
        }
        if (args[1]->type != T_int) {
            yyerror("ffi.get index must be an integer");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        const bool is_array = pointer->pointee->kind == kind_t::array &&
                              pointer->pointee->element;
        const type_ptr &element =
            is_array ? pointer->pointee->element : pointer->pointee;
        if (index < 0 ||
            (is_array &&
             (unsigned long long)index >= pointer->pointee->count)) {
            yyerror("ffi.get: index %lld out of range", index);
            return NULL;
        }
        std::string error;
        value_t *value = ffi::read_value(
            element,
            pointer->address + (std::size_t)index * element->size,
            pointer->owner, &error, pointer->keep);
        if (!value)
            yyerror("ffi.get: %s", error.c_str());
        return value;
    }

    if (meta == &ffi::buffer_native_type) {
        std::shared_ptr<ffi::buffer_handle> buffer;
        if (!ffi::get_buffer(container, buffer))
            return NULL;
        if (args[1]->type != T_int) {
            yyerror("ffi.get index must be an integer");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        if (index < 0 || (unsigned long long)index >= buffer->size) {
            yyerror("ffi.get: index %lld out of range", index);
            return NULL;
        }
        return newintval((long long)buffer->bytes[(std::size_t)index]);
    }

    yyerror("ffi.get cannot index %s", typename_of(container));
    return NULL;
}

static value_t *ffi_set(const std::vector<value_t *> &args) {
    if (args.size() != 3) {
        yyerror("std.ffi.set expects 3 arguments");
        return NULL;
    }
    value_t *container = args[0];
    std::string error;
    if (container->type != T_userdata) {
        yyerror("ffi.set cannot index %s", typename_of(container));
        return NULL;
    }
    const native_type *meta = ((userdata_value_t *)container)->meta;

    if (meta == &ffi::object_native_type || meta == &ffi::array_native_type) {
        std::shared_ptr<ffi::object_handle> object;
        if (!ffi::get_object(container, object))
            return NULL;
        if (object->type && object->type->kind == kind_t::record) {
            if (args[1]->type != T_string) {
                yyerror("ffi.set field name must be a string");
                return NULL;
            }
            const char *name = ((string_value_t *)args[1])->value;
            const ffi::field *f = ffi::find_field(*object->type, name);
            if (!f) {
                yyerror("%s has no field %s", object->type->name.c_str(),
                        name);
                return NULL;
            }
            if (!ffi::write_value(f->type, object->address + f->offset,
                                  args[2], *object->keep, error)) {
                yyerror("ffi.set: %s", error.c_str());
                return NULL;
            }
            return dupval(args[2]);
        }
        if (args[1]->type != T_int) {
            yyerror("ffi.set index must be an integer");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        if (index < 0 ||
            (unsigned long long)index >= object->type->count) {
            yyerror("ffi.set: index %lld out of range", index);
            return NULL;
        }
        if (!ffi::write_value(
                object->type->element,
                object->address +
                    (std::size_t)index * object->type->element->size,
                args[2], *object->keep, error)) {
            yyerror("ffi.set: %s", error.c_str());
            return NULL;
        }
        return dupval(args[2]);
    }

    if (meta == &ffi::pointer_native_type) {
        std::shared_ptr<ffi::pointer_handle> pointer;
        if (!ffi::get_pointer(container, pointer))
            return NULL;
        if (!pointer->address || !pointer->pointee) {
            yyerror("ffi.set: null pointer or missing pointee type");
            return NULL;
        }
        if (args[1]->type != T_int) {
            yyerror("ffi.set index must be an integer");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        const bool is_array = pointer->pointee->kind == kind_t::array &&
                              pointer->pointee->element;
        const type_ptr &element =
            is_array ? pointer->pointee->element : pointer->pointee;
        if (index < 0 ||
            (is_array &&
             (unsigned long long)index >= pointer->pointee->count)) {
            yyerror("ffi.set: index %lld out of range", index);
            return NULL;
        }
        if (!ffi::write_value(
                element,
                pointer->address + (std::size_t)index * element->size,
                args[2], *pointer->keep, error)) {
            yyerror("ffi.set: %s", error.c_str());
            return NULL;
        }
        return dupval(args[2]);
    }

    if (meta == &ffi::buffer_native_type) {
        std::shared_ptr<ffi::buffer_handle> buffer;
        if (!ffi::get_buffer(container, buffer))
            return NULL;
        if (args[1]->type != T_int || args[2]->type != T_int) {
            yyerror("ffi.set buffer index and value must be integers");
            return NULL;
        }
        long long index = ((int_value_t *)args[1])->value;
        long long byte = ((int_value_t *)args[2])->value;
        if (index < 0 || (unsigned long long)index >= buffer->size) {
            yyerror("ffi.set: index %lld out of range", index);
            return NULL;
        }
        if (byte < 0 || byte > 255) {
            yyerror("ffi.set: byte value %lld out of range", byte);
            return NULL;
        }
        buffer->bytes[(std::size_t)index] = (unsigned char)byte;
        return newintval(byte);
    }

    yyerror("ffi.set cannot index %s", typename_of(container));
    return NULL;
}

static value_t *ffi_to_dict(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.to_dict expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi::object_handle> object;
    if (!ffi::get_object(args[0], object))
        return NULL;
    std::string error;
    value_t *value = ffi::to_data(object->type, object->address,
                                 object->owner, &error, object->keep);
    if (!value)
        yyerror("ffi.to_dict: %s", error.c_str());
    return value;
}

static bool size_align_of(value_t *value, std::size_t &size,
                          std::size_t &align, std::string &error) {
    if (value->type == T_userdata) {
        const native_type *meta = ((userdata_value_t *)value)->meta;
        if (meta == &ffi::type_native_type) {
            std::shared_ptr<ffi::type_handle> handle =
                std::static_pointer_cast<ffi::type_handle>(
                    ((userdata_value_t *)value)->data);
            if (handle && handle->type) {
                size = handle->type->size;
                align = handle->type->align;
                return true;
            }
        } else if (meta == &ffi::object_native_type ||
                   meta == &ffi::array_native_type) {
            std::shared_ptr<ffi::object_handle> object =
                std::static_pointer_cast<ffi::object_handle>(
                    ((userdata_value_t *)value)->data);
            if (object && object->type) {
                size = object->type->size;
                align = object->type->align;
                return true;
            }
        } else if (meta == &ffi::pointer_native_type) {
            std::shared_ptr<ffi::pointer_handle> pointer =
                std::static_pointer_cast<ffi::pointer_handle>(
                    ((userdata_value_t *)value)->data);
            if (pointer && pointer->pointee) {
                size = pointer->pointee->size;
                align = pointer->pointee->align;
                return true;
            }
        } else if (meta == &ffi::buffer_native_type) {
            std::shared_ptr<ffi::buffer_handle> buffer =
                std::static_pointer_cast<ffi::buffer_handle>(
                    ((userdata_value_t *)value)->data);
            if (buffer) {
                size = buffer->size;
                align = 1;
                return true;
            }
        }
    }
    error = "expected an ffi type or value, got " +
            std::string(typename_of(value));
    return false;
}

static value_t *ffi_sizeof(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.sizeof expects 1 argument");
        return NULL;
    }
    std::size_t size = 0;
    std::size_t align = 0;
    std::string error;
    if (!size_align_of(args[0], size, align, error)) {
        yyerror("ffi.sizeof: %s", error.c_str());
        return NULL;
    }
    return newintval((long long)size);
}

static value_t *ffi_alignof(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.alignof expects 1 argument");
        return NULL;
    }
    std::size_t size = 0;
    std::size_t align = 1;
    std::string error;
    if (!size_align_of(args[0], size, align, error)) {
        yyerror("ffi.alignof: %s", error.c_str());
        return NULL;
    }
    return newintval((long long)align);
}

/* -------------------------------------------------------------------------
 * Library and calls
 * ---------------------------------------------------------------------- */

static value_t *ffi_open(const std::vector<value_t *> &args) {
    if (args.size() != 1 && args.size() != 2) {
        yyerror("std.ffi.open expects 1 or 2 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("ffi.open path must be a string");
        return NULL;
    }
    std::shared_ptr<const ffi::manifest> spec;
    if (args.size() == 2) {
        std::string error;
        if (!ffi::load_manifest(args[1], spec, error)) {
            yyerror("ffi.open: %s", error.c_str());
            return NULL;
        }
    }
    const char *path = ((string_value_t *)args[0])->value;
    std::string error;
    std::shared_ptr<dynamic_library> library =
        dynamic_library::open(path, error);
    if (!library) {
        yyerror("ffi.open: cannot load %s: %s", path, error.c_str());
        return NULL;
    }
    return ffi::new_library_value(std::move(library), std::move(spec));
}

static value_t *ffi_manifest_builtin(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.manifest expects 1 argument");
        return NULL;
    }
    std::string error;
    std::shared_ptr<const ffi::manifest> spec;
    if (!ffi::load_manifest(args[0], spec, error)) {
        yyerror("ffi.manifest: %s", error.c_str());
        return NULL;
    }
    return ffi::new_manifest_value(std::move(spec));
}

static value_t *ffi_type_lookup(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.type expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    if (args[1]->type != T_string) {
        yyerror("ffi.type name must be a string");
        return NULL;
    }
    if (!library->spec) {
        yyerror("ffi.type: library has no manifest");
        return NULL;
    }
    std::string name = ((string_value_t *)args[1])->value;
    auto it = library->spec->types.find(name);
    if (it == library->spec->types.end()) {
        yyerror("ffi.type: %s is not declared in the manifest", name.c_str());
        return NULL;
    }
    return ffi::new_type_value(it->second);
}

static value_t *ffi_constant(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.constant expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    if (args[1]->type != T_string) {
        yyerror("ffi.constant name must be a string");
        return NULL;
    }
    if (!library->spec) {
        yyerror("ffi.constant: library has no manifest");
        return NULL;
    }
    std::string name = ((string_value_t *)args[1])->value;
    auto it = library->spec->constants.find(name);
    if (it == library->spec->constants.end()) {
        yyerror("ffi.constant: %s is not declared in the manifest",
                name.c_str());
        return NULL;
    }
    return dupval(it->second);
}

static value_t *ffi_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.close expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    library->closed = true;
    return newnull();
}

static value_t *ffi_buffer_new(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_int ||
        ((int_value_t *)args[0])->value < 0) {
        yyerror("ffi.buffer size must be a non-negative integer");
        return NULL;
    }
    long long size = ((int_value_t *)args[0])->value;
    if ((unsigned long long)size > (unsigned long long)SIZE_MAX) {
        yyerror("ffi.buffer size is too large");
        return NULL;
    }
    return ffi::new_buffer_value((std::size_t)size);
}

static value_t *ffi_buffer_len(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer_len expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi::buffer_handle> buffer;
    if (!ffi::get_buffer(args[0], buffer))
        return NULL;
    return newintval((long long)buffer->size);
}

static value_t *ffi_buffer_bytes(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.buffer_bytes expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::buffer_handle> buffer;
    if (!ffi::get_buffer(args[0], buffer))
        return NULL;
    if (args[1]->type != T_int ||
        ((int_value_t *)args[1])->value < 0) {
        yyerror("ffi.buffer_bytes length must be a non-negative integer");
        return NULL;
    }
    long long length = ((int_value_t *)args[1])->value;
    if ((unsigned long long)length > (unsigned long long)buffer->size) {
        yyerror("ffi.buffer_bytes length out of range");
        return NULL;
    }
    if (length == 0)
        return newbytesval(std::string());
    return newbytesval(std::string((const char *)buffer->bytes.data(),
                                   (std::size_t)length));
}

static value_t *ffi_buffer_free(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer_free expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi::buffer_handle> buffer;
    if (!ffi::get_buffer(args[0], buffer))
        return NULL;
    buffer->bytes.clear();
    buffer->bytes.shrink_to_fit();
    buffer->size = 0;
    buffer->freed = true;
    return newnull();
}

static value_t *integer_result(kind_t kind, const unsigned char *buffer) {
    ffi_arg raw = load_bytes<ffi_arg>(buffer);
    switch (kind) {
        case kind_t::i8:
            return newintval((long long)(std::int8_t)raw);
        case kind_t::u8:
            return newintval((long long)(std::uint8_t)raw);
        case kind_t::i16:
            return newintval((long long)(std::int16_t)raw);
        case kind_t::u16:
            return newintval((long long)(std::uint16_t)raw);
        case kind_t::i32:
            return newintval((long long)(std::int32_t)raw);
        case kind_t::u32:
            return newintval((long long)(std::uint32_t)raw);
        case kind_t::i64:
            return newintval((long long)load_bytes<std::int64_t>(buffer));
        case kind_t::u64: {
            std::uint64_t value = load_bytes<std::uint64_t>(buffer);
            if (value >
                (std::uint64_t)std::numeric_limits<long long>::max()) {
                yyerror("ffi.call: u64 return value exceeds int64 range");
                return NULL;
            }
            return newintval((long long)value);
        }
        case kind_t::usize: {
            std::size_t value = load_bytes<std::size_t>(buffer);
            if (value >
                (std::size_t)std::numeric_limits<long long>::max()) {
                yyerror("ffi.call: usize return value exceeds int64 range");
                return NULL;
            }
            return newintval((long long)value);
        }
        case kind_t::isize:
            return newintval((long long)load_bytes<std::ptrdiff_t>(buffer));
        default:
            return newintval((long long)raw);
    }
}

static value_t *perform_call(
    const std::shared_ptr<ffi::library_handle> &library,
    const std::string &symbol_name, const type_ptr &return_type,
    const std::vector<type_ptr> &arg_types,
    const std::vector<value_t *> &values,
    const ffi::manifest_function *declared) {
    if (return_type->kind == kind_t::bytes ||
        return_type->kind == kind_t::buffer) {
        yyerror("ffi.call: bytes/buffer cannot be returned directly; "
                "use std.ffi.buffer");
        return NULL;
    }

    const std::size_t count = arg_types.size();
    if (values.size() != count) {
        yyerror("ffi.call: argument type/value count mismatch");
        return NULL;
    }
    if (count > 32) {
        yyerror("ffi.call: too many arguments");
        return NULL;
    }

    std::string error;
    std::vector<ffi::arg_slot> slots(count);
    std::vector<ffi_type *> cif_types;
    std::vector<void *> argv;
    cif_types.reserve(count);
    argv.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        if (!arg_types[i] || arg_types[i]->kind == kind_t::void_) {
            yyerror("ffi.call: invalid argument type");
            return NULL;
        }
        if (!ffi::prepare_arg(arg_types[i], values[i], slots[i], error)) {
            yyerror("ffi.call: argument %d: %s", (int)i + 1,
                    error.c_str());
            return NULL;
        }
        cif_types.push_back(arg_types[i]->cif);
    }
    for (const ffi::arg_slot &slot : slots)
        argv.push_back(slot.value);

    void *symbol = library->library->symbol(symbol_name.c_str(), error);
    if (!symbol) {
        yyerror("ffi.call: symbol not found: %s: %s", symbol_name.c_str(),
                error.c_str());
        return NULL;
    }

    ffi_cif cif;
    if (!return_type->cif) {
        yyerror("ffi.call: %s cannot be returned by value",
                return_type->name.c_str());
        return NULL;
    }
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)count,
                     return_type->cif,
                     cif_types.empty() ? nullptr : cif_types.data()) !=
        FFI_OK) {
        yyerror("ffi.call: cannot prepare call interface");
        return NULL;
    }

    ffi::callback_scope_guard callback_scope;
    if (return_type->kind == kind_t::void_) {
        ffi_call(&cif, FFI_FN(symbol), nullptr,
                 argv.empty() ? nullptr : argv.data());
        if (callback_scope.failed()) {
            yyerror("ffi.call: %s", callback_scope.error().c_str());
            return NULL;
        }
    } else {
        std::shared_ptr<ffi::memory_block> result_block;
        unsigned char *result = nullptr;
        alignas(16) unsigned char scalar[16] = {0};
        if (ffi::is_aggregate(return_type->kind)) {
            result_block = std::make_shared<ffi::memory_block>(
                return_type->size, return_type->align);
            result = result_block->data;
        } else {
            result = scalar;
        }

        ffi_call(&cif, FFI_FN(symbol), result,
                 argv.empty() ? nullptr : argv.data());
        if (callback_scope.failed()) {
            yyerror("ffi.call: %s", callback_scope.error().c_str());
            return NULL;
        }

        if (declared) {
            for (std::size_t index : declared->consumes) {
                if (index < values.size())
                    ffi::mark_consumed(values[index]);
            }
            for (std::size_t index : declared->retains) {
                if (index < values.size())
                    ffi::retain_value(library, values[index]);
            }
        }

        switch (return_type->kind) {
            case kind_t::f32:
                return newfloatval((double)load_bytes<float>(result));
            case kind_t::f64:
                return newfloatval(load_bytes<double>(result));
            case kind_t::cstring: {
                const char *value = load_bytes<const char *>(result);
                return value ? newstrval(value) : newnull();
            }
            case kind_t::pointer:
            case kind_t::opaque:
            case kind_t::callback: {
                void *raw = load_bytes<void *>(result);
                std::shared_ptr<void> owner = library;
                if (declared && declared->returns_owned && raw) {
                    std::string free_name = declared->free_function.empty()
                                                ? std::string("free")
                                                : declared->free_function;
                    std::string free_error;
                    void *free_symbol =
                        library->library->symbol(free_name.c_str(),
                                                 free_error);
                    if (!free_symbol) {
                        yyerror("ffi.call: returns_owned free function not "
                                "found: %s: %s", free_name.c_str(),
                                free_error.c_str());
                        return NULL;
                    }
                    using free_fn_t = void (*)(void *);
                    free_fn_t release = (free_fn_t)free_symbol;
                    std::shared_ptr<dynamic_library> keep =
                        library->library;
                    owner = std::shared_ptr<void>(
                        raw, [keep, release](void *pointer) {
                            if (pointer)
                                release(pointer);
                        });
                }
                return ffi::new_pointer_value(
                    return_type->kind == kind_t::pointer
                        ? return_type->element
                        : nullptr,
                    (unsigned char *)raw, owner);
            }
            case kind_t::record:
            case kind_t::array: {
                std::string read_error;
                value_t *value = ffi::read_value(
                    return_type, result, result_block, &read_error,
                    result_block->keep);
                if (!value)
                    yyerror("ffi.call: %s", read_error.c_str());
                return value;
            }
            case kind_t::void_:
                return newnull();
            default:
                return integer_result(return_type->kind, result);
        }
    }

    if (declared) {
        for (std::size_t index : declared->consumes) {
            if (index < values.size())
                ffi::mark_consumed(values[index]);
        }
        for (std::size_t index : declared->retains) {
            if (index < values.size())
                ffi::retain_value(library, values[index]);
        }
    }
    return newnull();
}

struct ffi_bound_function {
    std::shared_ptr<ffi::library_handle> library;
    std::shared_ptr<const ffi::manifest> spec;
    const ffi::manifest_function *declared = nullptr;
};

static value_t *make_bound_function(
    const std::shared_ptr<ffi::library_handle> &library,
    const std::shared_ptr<const ffi::manifest> &spec,
    const ffi::manifest_function &declared, value_t *library_value) {
    std::shared_ptr<ffi_bound_function> bound =
        std::make_shared<ffi_bound_function>();
    bound->library = library;
    bound->spec = spec;
    bound->declared = &declared;

    value_t *value = newnativeval(
        [bound](const std::vector<value_t *> &args) -> value_t * {
            if (!bound->library || bound->library->closed ||
                !bound->library->library ||
                !bound->library->library->valid()) {
                yyerror("ffi library is closed");
                return NULL;
            }
            return perform_call(bound->library, bound->declared->symbol,
                                bound->declared->result,
                                bound->declared->args, args,
                                bound->declared);
        },
        bound);
    if (library_value) {
        native_function_value *native = (native_function_value *)value;
        native->children.push_back(dupval(library_value));
    }
    return value;
}

static value_t *ffi_function(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.function expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    if (args[1]->type != T_string) {
        yyerror("ffi.function name must be a string");
        return NULL;
    }
    if (!library->spec) {
        yyerror("ffi.function: library has no manifest");
        return NULL;
    }
    std::string name = ((string_value_t *)args[1])->value;
    auto it = library->spec->functions.find(name);
    if (it == library->spec->functions.end()) {
        yyerror("ffi.function: %s is not declared in the manifest",
                name.c_str());
        return NULL;
    }
    return make_bound_function(library, library->spec, it->second, args[0]);
}

static value_t *ffi_declare(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.declare expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    std::string error;
    std::shared_ptr<const ffi::manifest> merged;
    bool ok = false;
    if (library->spec)
        ok = ffi::extend_manifest(*library->spec, args[1], merged, error);
    else
        ok = ffi::load_manifest(args[1], merged, error);
    if (!ok) {
        yyerror("ffi.declare: %s", error.c_str());
        return NULL;
    }
    library->spec = std::move(merged);
    return newnull();
}

static value_t *ffi_call(const std::vector<value_t *> &args) {
    if (args.size() != 3 && args.size() != 5) {
        yyerror("std.ffi.call expects 3 or 5 arguments");
        return NULL;
    }
    std::shared_ptr<ffi::library_handle> library;
    if (!ffi::get_library(args[0], library))
        return NULL;
    if (args[1]->type != T_string) {
        yyerror("ffi.call symbol must be a string");
        return NULL;
    }
    const std::string symbol = ((string_value_t *)args[1])->value;

    if (args.size() == 3) {
        if (!library->spec) {
            yyerror("ffi.call: library has no manifest; "
                    "pass types explicitly");
            return NULL;
        }
        auto declared = library->spec->functions.find(symbol);
        if (declared == library->spec->functions.end()) {
            yyerror("ffi.call: function %s is not declared in the manifest",
                    symbol.c_str());
            return NULL;
        }
        if (args[2]->type != T_list) {
            yyerror("ffi.call arguments must be a list");
            return NULL;
        }
        return perform_call(library, declared->second.symbol,
                            declared->second.result,
                            declared->second.args,
                            ((list_value_t *)args[2])->payload->items,
                            &declared->second);
    }

    if (args[3]->type != T_list || args[4]->type != T_list) {
        yyerror("ffi.call argument types and values must be lists");
        return NULL;
    }

    std::string error;
    type_ptr return_type;
    if (!ffi::resolve_type(args[2], return_type, error)) {
        yyerror("ffi.call: %s", error.c_str());
        return NULL;
    }

    list_value_t *types = (list_value_t *)args[3];
    list_value_t *values = (list_value_t *)args[4];
    if (types->payload->items.size() != values->payload->items.size()) {
        yyerror("ffi.call: argument type/value count mismatch");
        return NULL;
    }
    if (types->payload->items.size() > 32) {
        yyerror("ffi.call: too many arguments");
        return NULL;
    }

    std::vector<type_ptr> arg_types;
    arg_types.reserve(types->payload->items.size());
    for (value_t *item : types->payload->items) {
        type_ptr type;
        if (!ffi::resolve_type(item, type, error)) {
            yyerror("ffi.call: %s", error.c_str());
            return NULL;
        }
        arg_types.push_back(std::move(type));
    }
    return perform_call(library, symbol, return_type, arg_types,
                        values->payload->items, nullptr);
}

/* -------------------------------------------------------------------------
 * Dispatch
 * ---------------------------------------------------------------------- */

struct value_t *call_ffi_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_ffi_open:
        case B_ffi_bind:
            return ffi_open(args);
        case B_ffi_close:
            return ffi_close(args);
        case B_ffi_call:
            return ffi_call(args);
        case B_ffi_buffer:
            return ffi_buffer_new(args);
        case B_ffi_buffer_len:
            return ffi_buffer_len(args);
        case B_ffi_buffer_bytes:
            return ffi_buffer_bytes(args);
        case B_ffi_buffer_free:
            return ffi_buffer_free(args);
        case B_ffi_struct:
            return ffi_struct(args);
        case B_ffi_pointer:
            return ffi_pointer(args);
        case B_ffi_array:
            return ffi_array(args);
        case B_ffi_signature:
            return ffi_signature(args);
        case B_ffi_new:
            return ffi_new(args);
        case B_ffi_alloc:
            return ffi_alloc(args);
        case B_ffi_ptr:
            return ffi_ptr(args);
        case B_ffi_deref:
            return ffi_deref(args);
        case B_ffi_offset:
            return ffi_offset(args);
        case B_ffi_cast:
            return ffi_cast(args);
        case B_ffi_get:
            return ffi_get(args);
        case B_ffi_set:
            return ffi_set(args);
        case B_ffi_to_dict:
            return ffi_to_dict(args);
        case B_ffi_sizeof:
            return ffi_sizeof(args);
        case B_ffi_alignof:
            return ffi_alignof(args);
        case B_ffi_callback:
            return ffi_callback(args);
        case B_ffi_manifest:
            return ffi_manifest_builtin(args);
        case B_ffi_constant:
            return ffi_constant(args);
        case B_ffi_type_lookup:
            return ffi_type_lookup(args);
        case B_ffi_function:
            return ffi_function(args);
        case B_ffi_declare:
            return ffi_declare(args);
        default:
            yyerror("unknown ffi function");
            return NULL;
    }
}

struct value_t *ffi::library_get_field(void *data, const char *name) {
    ffi::library_handle *library = (ffi::library_handle *)data;
    if (!library || !library->spec) {
        yyerror("ffi library has no manifest");
        return NULL;
    }
    std::string key = name ? name : "";
    auto function = library->spec->functions.find(key);
    if (function != library->spec->functions.end()) {
        return make_bound_function(library->shared_from_this(),
                                   library->spec, function->second,
                                   library->self);
    }
    auto constant = library->spec->constants.find(key);
    if (constant != library->spec->constants.end())
        return dupval(constant->second);
    yyerror("ffi library has no member %s", key.c_str());
    return NULL;
}
