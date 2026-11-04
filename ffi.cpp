#include "ffi_module.h"

#include "ast.h"
#include "dynamic_library.h"
#include "eval_helpers.h"
#include "valuetype.h"

#include <ffi.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ffi_library {
    std::shared_ptr<dynamic_library> library;
};

std::string ffi_describe(const void *data) {
    const ffi_library *library = (const ffi_library *)data;
    if (!library || !library->library)
        return "<ffi_library closed>";
    return "<ffi_library>";
}

const struct native_type ffi_library_type = {
    "ffi_library", ffi_describe
};

struct ffi_buffer {
    std::vector<unsigned char> data;
    size_t size = 0;
    bool freed = false;
};

std::string ffi_buffer_describe(const void *data) {
    const ffi_buffer *buffer = (const ffi_buffer *)data;
    if (!buffer || buffer->freed)
        return "<ffi_buffer freed>";
    return "<ffi_buffer " + std::to_string(buffer->size) + ">";
}

const struct native_type ffi_buffer_type = {
    "ffi_buffer", ffi_buffer_describe
};

value_t *new_buffer_value(size_t size) {
    std::shared_ptr<ffi_buffer> buffer = std::make_shared<ffi_buffer>();
    buffer->size = size;
    // Keep a non-null pointer for zero-length buffers so they can still be
    // passed to C functions that do not special-case a null data pointer.
    buffer->data.resize(size ? size : 1, 0);
    return newuserdataval(&ffi_buffer_type, buffer);
}

bool get_buffer(value_t *value, std::shared_ptr<ffi_buffer> &buffer) {
    if (!value || value->type != T_userdata) {
        yyerror("expected an ffi buffer, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &ffi_buffer_type) {
        yyerror("expected an ffi buffer, got %s", typename_of(value));
        return false;
    }
    buffer = std::static_pointer_cast<ffi_buffer>(userdata->data);
    if (!buffer || buffer->freed) {
        yyerror("ffi buffer is freed");
        return false;
    }
    return true;
}

value_t *new_library_value(std::shared_ptr<dynamic_library> lib) {
    std::shared_ptr<ffi_library> library = std::make_shared<ffi_library>();
    library->library = std::move(lib);
    return newuserdataval(&ffi_library_type, library);
}

bool get_library(value_t *value, std::shared_ptr<ffi_library> &library) {
    if (!value || value->type != T_userdata) {
        yyerror("expected an ffi library, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &ffi_library_type) {
        yyerror("expected an ffi library, got %s", typename_of(value));
        return false;
    }
    library = std::static_pointer_cast<ffi_library>(userdata->data);
    if (!library || !library->library || !library->library->valid()) {
        yyerror("ffi library is closed");
        return false;
    }
    return true;
}

enum ffi_kind {
    FFI_I8,
    FFI_U8,
    FFI_I16,
    FFI_U16,
    FFI_I32,
    FFI_U32,
    FFI_I64,
    FFI_U64,
    FFI_F32,
    FFI_F64,
    FFI_CSTRING,
    FFI_BYTES,
    FFI_BUFFER,
    FFI_USIZE,
    FFI_ISIZE,
    FFI_VOID
};

bool parse_ffi_type(const char *text, ffi_kind &kind) {
    if (!text) return false;
    if (std::strcmp(text, "i8") == 0) kind = FFI_I8;
    else if (std::strcmp(text, "u8") == 0) kind = FFI_U8;
    else if (std::strcmp(text, "i16") == 0) kind = FFI_I16;
    else if (std::strcmp(text, "u16") == 0) kind = FFI_U16;
    else if (std::strcmp(text, "i32") == 0) kind = FFI_I32;
    else if (std::strcmp(text, "u32") == 0) kind = FFI_U32;
    else if (std::strcmp(text, "i64") == 0) kind = FFI_I64;
    else if (std::strcmp(text, "u64") == 0) kind = FFI_U64;
    else if (std::strcmp(text, "f32") == 0) kind = FFI_F32;
    else if (std::strcmp(text, "f64") == 0) kind = FFI_F64;
    else if (std::strcmp(text, "cstring") == 0) kind = FFI_CSTRING;
    else if (std::strcmp(text, "bytes") == 0) kind = FFI_BYTES;
    else if (std::strcmp(text, "buffer") == 0) kind = FFI_BUFFER;
    else if (std::strcmp(text, "usize") == 0) kind = FFI_USIZE;
    else if (std::strcmp(text, "isize") == 0) kind = FFI_ISIZE;
    else if (std::strcmp(text, "void") == 0) kind = FFI_VOID;
    else return false;
    return true;
}

ffi_type *ffi_type_for(ffi_kind kind) {
    switch (kind) {
        case FFI_I8: return &ffi_type_sint8;
        case FFI_U8: return &ffi_type_uint8;
        case FFI_I16: return &ffi_type_sint16;
        case FFI_U16: return &ffi_type_uint16;
        case FFI_I32: return &ffi_type_sint32;
        case FFI_U32: return &ffi_type_uint32;
        case FFI_I64: return &ffi_type_sint64;
        case FFI_U64: return &ffi_type_uint64;
        case FFI_F32: return &ffi_type_float;
        case FFI_F64: return &ffi_type_double;
        case FFI_CSTRING: return &ffi_type_pointer;
        case FFI_BYTES: return &ffi_type_pointer;
        case FFI_BUFFER: return &ffi_type_pointer;
        case FFI_USIZE:
            return sizeof(size_t) == 8 ? &ffi_type_uint64 : &ffi_type_uint32;
        case FFI_ISIZE:
            return sizeof(size_t) == 8 ? &ffi_type_sint64 : &ffi_type_sint32;
        case FFI_VOID: return &ffi_type_void;
        default: return &ffi_type_void;
    }
}

value_t *ffi_open(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.open expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("ffi.open path must be a string");
        return NULL;
    }
    const char *path = ((struct string_value_t *)args[0])->value;
    std::string error;
    std::shared_ptr<dynamic_library> lib =
        dynamic_library::open(path, error);
    if (!lib) {
        yyerror("ffi.open: cannot load %s: %s", path, error.c_str());
        return NULL;
    }
    return new_library_value(std::move(lib));
}

value_t *ffi_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.close expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi_library> library;
    if (!get_library(args[0], library))
        return NULL;
    library->library.reset();
    return newnull();
}

value_t *ffi_buffer_new(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_int ||
        ((struct int_value_t *)args[0])->value < 0) {
        yyerror("ffi.buffer size must be a non-negative integer");
        return NULL;
    }
    long long size = ((struct int_value_t *)args[0])->value;
    if ((unsigned long long)size > (unsigned long long)SIZE_MAX) {
        yyerror("ffi.buffer size is too large");
        return NULL;
    }
    return new_buffer_value((size_t)size);
}

value_t *ffi_buffer_len(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer_len expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi_buffer> buffer;
    if (!get_buffer(args[0], buffer))
        return NULL;
    return newintval((long long)buffer->size);
}

value_t *ffi_buffer_bytes(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.ffi.buffer_bytes expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<ffi_buffer> buffer;
    if (!get_buffer(args[0], buffer))
        return NULL;
    if (args[1]->type != T_int ||
        ((struct int_value_t *)args[1])->value < 0) {
        yyerror("ffi.buffer_bytes length must be a non-negative integer");
        return NULL;
    }
    long long length = ((struct int_value_t *)args[1])->value;
    if ((unsigned long long)length > (unsigned long long)buffer->size) {
        yyerror("ffi.buffer_bytes length out of range");
        return NULL;
    }
    if (length == 0)
        return newbytesval(std::string());
    return newbytesval(std::string(
        (const char *)buffer->data.data(), (size_t)length));
}

value_t *ffi_buffer_free(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.ffi.buffer_free expects 1 argument");
        return NULL;
    }
    std::shared_ptr<ffi_buffer> buffer;
    if (!get_buffer(args[0], buffer))
        return NULL;
    buffer->data.clear();
    buffer->data.shrink_to_fit();
    buffer->size = 0;
    buffer->freed = true;
    return newnull();
}

value_t *ffi_call(const std::vector<value_t *> &args) {
    if (args.size() != 5) {
        yyerror("std.ffi.call expects 5 arguments");
        return NULL;
    }
    std::shared_ptr<ffi_library> library;
    if (!get_library(args[0], library))
        return NULL;
    if (args[1]->type != T_string) {
        yyerror("ffi.call symbol must be a string");
        return NULL;
    }
    if (args[2]->type != T_string) {
        yyerror("ffi.call return type must be a string");
        return NULL;
    }
    if (args[3]->type != T_list || args[4]->type != T_list) {
        yyerror("ffi.call argument types and values must be lists");
        return NULL;
    }

    ffi_kind return_kind;
    if (!parse_ffi_type(((struct string_value_t *)args[2])->value,
                        return_kind)) {
        yyerror("ffi.call: unknown return type");
        return NULL;
    }
    if (return_kind == FFI_BYTES || return_kind == FFI_BUFFER) {
        yyerror("ffi.call: bytes/buffer cannot be returned directly; "
                "use std.ffi.buffer");
        return NULL;
    }
    struct list_value_t *types = (struct list_value_t *)args[3];
    struct list_value_t *values = (struct list_value_t *)args[4];
    if (types->payload->items.size() != values->payload->items.size()) {
        yyerror("ffi.call: argument type/value count mismatch");
        return NULL;
    }
    if (types->payload->items.size() > 32) {
        yyerror("ffi.call: too many arguments");
        return NULL;
    }

    std::vector<ffi_kind> kinds;
    std::vector<ffi_type *> ffi_types;
    for (size_t i = 0; i < types->payload->items.size(); ++i) {
        struct value_t *type_value = types->payload->items[i];
        if (type_value->type != T_string) {
            yyerror("ffi.call: argument types must be strings");
            return NULL;
        }
        ffi_kind kind;
        if (!parse_ffi_type(((struct string_value_t *)type_value)->value,
                            kind) ||
            kind == FFI_VOID) {
            yyerror("ffi.call: invalid argument type");
            return NULL;
        }
        kinds.push_back(kind);
        ffi_types.push_back(ffi_type_for(kind));
    }

    std::vector<ffi_arg> integer_values;
    std::vector<float> float_values;
    std::vector<double> double_values;
    std::vector<const char *> string_values;
    std::vector<std::vector<unsigned char>> byte_values;
    std::vector<void *> pointer_values;
    std::vector<std::shared_ptr<ffi_buffer>> buffer_values;
    integer_values.reserve(kinds.size());
    float_values.reserve(kinds.size());
    double_values.reserve(kinds.size());
    string_values.reserve(kinds.size());
    byte_values.reserve(kinds.size());
    pointer_values.reserve(kinds.size());
    buffer_values.reserve(kinds.size());
    std::vector<void *> arg_values;
    arg_values.reserve(kinds.size());

    for (size_t i = 0; i < kinds.size(); ++i) {
        struct value_t *value = values->payload->items[i];
        ffi_kind kind = kinds[i];
        switch (kind) {
            case FFI_I8:
            case FFI_U8:
            case FFI_I16:
            case FFI_U16:
            case FFI_I32:
            case FFI_U32:
            case FFI_I64:
            case FFI_U64: {
                if (value->type != T_int) {
                    yyerror("ffi.call: argument %d expects an integer",
                            (int)i + 1);
                    return NULL;
                }
                long long n = ((struct int_value_t *)value)->value;
                if ((kind == FFI_U8 || kind == FFI_U16 ||
                     kind == FFI_U32 || kind == FFI_U64) && n < 0) {
                    yyerror("ffi.call: argument %d cannot be negative",
                            (int)i + 1);
                    return NULL;
                }
                integer_values.push_back((ffi_arg)n);
                arg_values.push_back(&integer_values.back());
                break;
            }
            case FFI_F32: {
                if (value->type != T_int && value->type != T_float) {
                    yyerror("ffi.call: argument %d expects a number",
                            (int)i + 1);
                    return NULL;
                }
                float_values.push_back((float)as_double(value));
                arg_values.push_back(&float_values.back());
                break;
            }
            case FFI_F64: {
                if (value->type != T_int && value->type != T_float) {
                    yyerror("ffi.call: argument %d expects a number",
                            (int)i + 1);
                    return NULL;
                }
                double_values.push_back(as_double(value));
                arg_values.push_back(&double_values.back());
                break;
            }
            case FFI_CSTRING: {
                if (value->type != T_string) {
                    yyerror("ffi.call: argument %d expects a string",
                            (int)i + 1);
                    return NULL;
                }
                string_values.push_back(
                    ((struct string_value_t *)value)->value);
                arg_values.push_back(&string_values.back());
                break;
            }
            case FFI_BYTES: {
                if (value->type != T_bytes) {
                    yyerror("ffi.call: argument %d expects bytes",
                            (int)i + 1);
                    return NULL;
                }
                const std::string &data =
                    ((struct bytes_value_t *)value)->value;
                byte_values.emplace_back(data.begin(), data.end());
                if (data.empty())
                    byte_values.back().push_back(0);
                pointer_values.push_back(byte_values.back().data());
                arg_values.push_back(&pointer_values.back());
                break;
            }
            case FFI_BUFFER: {
                std::shared_ptr<ffi_buffer> buffer;
                if (!get_buffer(value, buffer))
                    return NULL;
                buffer_values.push_back(buffer);
                pointer_values.push_back(buffer->data.data());
                arg_values.push_back(&pointer_values.back());
                break;
            }
            case FFI_USIZE:
            case FFI_ISIZE: {
                if (value->type != T_int) {
                    yyerror("ffi.call: argument %d expects an integer",
                            (int)i + 1);
                    return NULL;
                }
                long long n = ((struct int_value_t *)value)->value;
                if (kind == FFI_USIZE && n < 0) {
                    yyerror("ffi.call: argument %d cannot be negative",
                            (int)i + 1);
                    return NULL;
                }
                integer_values.push_back((ffi_arg)n);
                arg_values.push_back(&integer_values.back());
                break;
            }
            case FFI_VOID:
            default:
                yyerror("ffi.call: invalid argument type");
                return NULL;
        }
    }

    const char *symbol_name = ((struct string_value_t *)args[1])->value;
    std::shared_ptr<dynamic_library> lib = library->library;
    std::string error;
    void *symbol = lib->symbol(symbol_name, error);
    if (!symbol) {
        yyerror("ffi.call: symbol not found: %s: %s", symbol_name,
                error.c_str());
        return NULL;
    }

    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)kinds.size(),
                     ffi_type_for(return_kind),
                     ffi_types.empty() ? NULL : ffi_types.data()) != FFI_OK) {
        yyerror("ffi.call: cannot prepare call interface");
        return NULL;
    }

    if (return_kind == FFI_VOID) {
        ffi_call(&cif, FFI_FN(symbol), NULL, arg_values.data());
        return newnull();
    }
    if (return_kind == FFI_F32) {
        float result = 0;
        ffi_call(&cif, FFI_FN(symbol), &result, arg_values.data());
        return newfloatval((double)result);
    }
    if (return_kind == FFI_F64) {
        double result = 0;
        ffi_call(&cif, FFI_FN(symbol), &result, arg_values.data());
        return newfloatval(result);
    }
    if (return_kind == FFI_CSTRING) {
        const char *result = NULL;
        ffi_call(&cif, FFI_FN(symbol), &result, arg_values.data());
        return result ? newstrval(result) : newnull();
    }

    ffi_arg result = 0;
    ffi_call(&cif, FFI_FN(symbol), &result, arg_values.data());

    unsigned long long max_signed =
        (unsigned long long)std::numeric_limits<long long>::max();
    if (return_kind == FFI_U64) {
        unsigned long long value = (unsigned long long)result;
        if (value > max_signed) {
            yyerror("ffi.call: u64 return value exceeds int64 range");
            return NULL;
        }
        return newintval((long long)value);
    }
    if (return_kind == FFI_USIZE) {
        unsigned long long value =
            (unsigned long long)(std::uintptr_t)result;
        if (value > max_signed) {
            yyerror("ffi.call: usize return value exceeds int64 range");
            return NULL;
        }
        return newintval((long long)value);
    }

    long long signed_result = (long long)result;
    if (return_kind == FFI_I8)
        signed_result = (std::int8_t)result;
    else if (return_kind == FFI_U8)
        signed_result = (std::uint8_t)result;
    else if (return_kind == FFI_I16)
        signed_result = (std::int16_t)result;
    else if (return_kind == FFI_U16)
        signed_result = (std::uint16_t)result;
    else if (return_kind == FFI_I32)
        signed_result = (std::int32_t)result;
    else if (return_kind == FFI_U32)
        signed_result = (std::uint32_t)result;
    else if (return_kind == FFI_ISIZE)
        signed_result = (long long)(std::intptr_t)result;
    return newintval(signed_result);
}

} // namespace

struct value_t *call_ffi_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_ffi_open:
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
        default:
            yyerror("unknown ffi function");
            return NULL;
    }
}
