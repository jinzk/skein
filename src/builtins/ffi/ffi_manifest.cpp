#include "ffi_manifest.h"

#include "ast.h"
#include "ffi_value.h"
#include "json.h"
#include "valuetype.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace ffi {
namespace {

constexpr int MAX_MANIFEST_DEPTH = 64;
constexpr std::size_t MAX_MANIFEST_TYPES = 512;
constexpr std::size_t MAX_MANIFEST_FUNCTIONS = 4096;
constexpr std::size_t MAX_STRUCT_FIELDS = 256;
constexpr std::size_t MAX_CALL_ARGS = 32;

bool dict_lookup(const dict_value_t *dict, const char *key,
                 struct value_t *&out) {
    if (!dict)
        return false;
    for (const auto &entry : dict->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((string_value_t *)entry.first)->value, key) == 0) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool as_string(const struct value_t *value, std::string &out) {
    if (!value || value->type != T_string)
        return false;
    out = ((const string_value_t *)value)->value;
    return true;
}

bool as_size(const struct value_t *value, std::size_t &out) {
    if (!value || value->type != T_int)
        return false;
    long long number = ((const int_value_t *)value)->value;
    if (number < 0)
        return false;
    out = (std::size_t)number;
    return true;
}

bool as_bool(const struct value_t *value, bool &out) {
    if (!value)
        return false;
    if (value->type == T_bool) {
        out = ((const bool_value_t *)value)->value;
        return true;
    }
    return false;
}

bool host_is_little_endian() {
    std::uint16_t marker = 1;
    return *(const std::uint8_t *)&marker == 1;
}

bool read_file(const std::string &path, std::string &text,
               std::string &error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open manifest " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    return true;
}

bool validate_abi(struct value_t *abi, std::string &error) {
    if (!abi || abi->type != T_dict) {
        error = "manifest abi must be a dict";
        return false;
    }
    dict_value_t *dict = (dict_value_t *)abi;
    struct value_t *value = nullptr;
    if (dict_lookup(dict, "pointer_bits", value)) {
        std::size_t bits = 0;
        if (!as_size(value, bits) ||
            bits != sizeof(void *) * 8) {
            error = "manifest abi pointer_bits does not match this build";
            return false;
        }
    }
    if (dict_lookup(dict, "endian", value)) {
        std::string endian;
        if (!as_string(value, endian) ||
            (endian != "native" && endian != "little" &&
             endian != "big")) {
            error = "manifest abi endian must be native, little, or big";
            return false;
        }
        if (endian == "little" && !host_is_little_endian()) {
            error = "manifest requires a little-endian host";
            return false;
        }
        if (endian == "big" && host_is_little_endian()) {
            error = "manifest requires a big-endian host";
            return false;
        }
    }
    if (dict_lookup(dict, "calling_convention", value)) {
        std::string convention;
        if (!as_string(value, convention) ||
            (convention != "c" && convention != "C")) {
            error = "manifest abi calling_convention must be c";
            return false;
        }
    }
    return true;
}

/* Resolves a `types` section with forward references.  Named types get a
 * placeholder first so pointers to later/self types work; by-value cycles
 * are rejected during the fill pass. */
class type_resolver {
public:
    type_resolver(const dict_value_t *types,
                  const std::map<std::string, type_ptr> *base,
                  std::string &error)
        : types_(types), base_(base), error_(error) {}

    bool prepare() {
        if (!types_)
            return true;
        if (types_->payload->items.size() > MAX_MANIFEST_TYPES) {
            error_ = "manifest declares too many types";
            return false;
        }
        for (const auto &entry : types_->payload->items) {
            std::string name;
            if (!as_string(entry.first, name) || name.empty()) {
                error_ = "manifest type names must be non-empty strings";
                return false;
            }
            if (scalar(name)) {
                error_ = "manifest type " + name +
                         " shadows a built-in type";
                return false;
            }
            if (slots_.count(name)) {
                error_ = "duplicate manifest type " + name;
                return false;
            }
            if (base_ && base_->count(name)) {
                error_ = "duplicate manifest type " + name;
                return false;
            }
            slots_.emplace(name, std::make_shared<type>());
        }
        return true;
    }

    bool finish() {
        for (const auto &entry : slots_) {
            if (!fill(entry.first))
                return false;
        }
        for (const auto &entry : slots_)
            resolved_[entry.first] = entry.second;
        return true;
    }

    const std::map<std::string, type_ptr> &types() const { return resolved_; }

    bool resolve_ref(struct value_t *value, type_ptr &out, int depth) {
        if (!value) {
            error_ = "missing type";
            return false;
        }
        if (depth > MAX_MANIFEST_DEPTH) {
            error_ = "manifest type nesting is too deep";
            return false;
        }
        if (value->type == T_string) {
            std::string name = ((string_value_t *)value)->value;
            type_ptr builtin = scalar(name);
            if (builtin) {
                out = std::move(builtin);
                return true;
            }
            if (base_) {
                auto base_it = base_->find(name);
                if (base_it != base_->end()) {
                    out = base_it->second;
                    return true;
                }
            }
            auto it = slots_.find(name);
            if (it == slots_.end()) {
                error_ = "unknown manifest type " + name;
                return false;
            }
            if (!fill(name))
                return false;
            out = it->second;
            return true;
        }
        if (value->type == T_userdata &&
            ((userdata_value_t *)value)->meta == &type_native_type) {
            std::shared_ptr<type_handle> handle =
                std::static_pointer_cast<type_handle>(
                    ((userdata_value_t *)value)->data);
            if (!handle || !handle->type) {
                error_ = "invalid ffi type";
                return false;
            }
            out = handle->type;
            return true;
        }
        if (value->type == T_dict)
            return build_decl("", value, out, depth + 1);
        error_ = "manifest type must be a name or a type descriptor";
        return false;
    }

private:
    bool resolve_field_type(struct value_t *value, type_ptr &out, int depth) {
        return resolve_ref(value, out, depth);
    }

    bool resolve_pointer_target(struct value_t *value, type_ptr &out,
                                int depth) {
        if (!value) {
            error_ = "pointer target is missing";
            return false;
        }
        if (value->type == T_string) {
            std::string name = ((string_value_t *)value)->value;
            type_ptr builtin = scalar(name);
            if (builtin) {
                out = std::move(builtin);
                return true;
            }
            if (base_) {
                auto base_it = base_->find(name);
                if (base_it != base_->end()) {
                    out = base_it->second;
                    return true;
                }
            }
            auto it = slots_.find(name);
            if (it == slots_.end()) {
                error_ = "unknown manifest type " + name;
                return false;
            }
            /* Do not require the target to be complete: a pointer to a
             * later or self type is fine. */
            out = it->second;
            return true;
        }
        return resolve_ref(value, out, depth);
    }

    bool build_fields(struct value_t *fields,
                      std::vector<std::pair<std::string, type_ptr>> &out,
                      int depth) {
        if (!fields) {
            error_ = "struct fields are missing";
            return false;
        }
        if (fields->type == T_dict) {
            for (const auto &entry : ((dict_value_t *)fields)->payload->items) {
                std::string name;
                if (!as_string(entry.first, name)) {
                    error_ = "struct field names must be strings";
                    return false;
                }
                type_ptr type;
                if (!resolve_field_type(entry.second, type, depth))
                    return false;
                out.emplace_back(name, std::move(type));
                if (out.size() > MAX_STRUCT_FIELDS) {
                    error_ = "struct has too many fields";
                    return false;
                }
            }
            return true;
        }
        if (fields->type != T_list) {
            error_ = "struct fields must be a dict or a list";
            return false;
        }
        for (struct value_t *item : ((list_value_t *)fields)->payload->items) {
            std::string name;
            struct value_t *type_value = nullptr;
            if (item->type == T_list) {
                auto &pair = ((list_value_t *)item)->payload->items;
                if (pair.size() != 2 || !as_string(pair[0], name)) {
                    error_ = "struct fields must be [name, type] pairs";
                    return false;
                }
                type_value = pair[1];
            } else if (item->type == T_dict) {
                dict_value_t *field = (dict_value_t *)item;
                if (!dict_lookup(field, "name", type_value) ||
                    !as_string(type_value, name)) {
                    error_ = "struct field is missing a name";
                    return false;
                }
                struct value_t *bits = nullptr;
                if ((dict_lookup(field, "bits", bits) ||
                     dict_lookup(field, "bit_width", bits))) {
                    error_ = "bitfields are not supported by generic ffi; "
                             "use a C++ binding";
                    return false;
                }
                if (!dict_lookup(field, "type", type_value) ||
                    !type_value) {
                    error_ = "struct field " + name + " is missing a type";
                    return false;
                }
            } else {
                error_ = "struct fields must be [name, type] pairs or dicts";
                return false;
            }
            type_ptr type;
            if (!resolve_field_type(type_value, type, depth))
                return false;
            out.emplace_back(name, std::move(type));
            if (out.size() > MAX_STRUCT_FIELDS) {
                error_ = "struct has too many fields";
                return false;
            }
        }
        return true;
    }

    bool build_decl(const std::string &name, struct value_t *decl,
                    type_ptr &out, int depth) {
        if (depth > MAX_MANIFEST_DEPTH) {
            error_ = "manifest type nesting is too deep";
            return false;
        }
        if (!decl) {
            error_ = "missing type declaration";
            return false;
        }
        if (decl->type == T_string) {
            type_ptr target;
            if (!resolve_ref(decl, target, depth))
                return false;
            std::shared_ptr<type> alias = std::make_shared<type>(*target);
            if (alias->owned_cif) {
                alias->cif = alias->owned_cif.get();
                alias->owned_cif->elements = alias->cif_elements.data();
            }
            if (!name.empty())
                alias->name = name;
            out = std::move(alias);
            return true;
        }
        if (decl->type == T_userdata &&
            ((userdata_value_t *)decl)->meta == &type_native_type) {
            return resolve_ref(decl, out, depth);
        }
        if (decl->type != T_dict) {
            error_ = "type declaration must be an object or a name";
            return false;
        }
        dict_value_t *dict = (dict_value_t *)decl;
        std::string kind;
        struct value_t *value = nullptr;
        if (!dict_lookup(dict, "kind", value) || !as_string(value, kind) ||
            kind.empty()) {
            error_ = "type declaration is missing a kind";
            return false;
        }
        if (kind == "struct") {
            struct value_t *packed = nullptr;
            bool is_packed = false;
            if (dict_lookup(dict, "packed", packed) &&
                as_bool(packed, is_packed) && is_packed) {
                error_ = "packed structs are not supported by generic ffi; "
                         "use a C++ binding";
                return false;
            }
            struct value_t *fields = nullptr;
            if (!dict_lookup(dict, "fields", fields)) {
                error_ = "struct " + name + " is missing fields";
                return false;
            }
            std::vector<std::pair<std::string, type_ptr>> list;
            if (!build_fields(fields, list, depth + 1))
                return false;
            type_ptr built;
            if (!make_record(name, list, built, error_))
                return false;
            out = std::move(built);
            return true;
        }
        if (kind == "pointer") {
            struct value_t *target = nullptr;
            if (!dict_lookup(dict, "to", target) &&
                !dict_lookup(dict, "element", target) &&
                !dict_lookup(dict, "pointee", target)) {
                error_ = "pointer type is missing its target";
                return false;
            }
            type_ptr pointee;
            if (!resolve_pointer_target(target, pointee, depth + 1))
                return false;
            out = make_pointer(std::move(pointee));
            return true;
        }
        if (kind == "array") {
            struct value_t *element = nullptr;
            if (!dict_lookup(dict, "element", element) &&
                !dict_lookup(dict, "of", element)) {
                error_ = "array type is missing its element";
                return false;
            }
            struct value_t *count = nullptr;
            if (!dict_lookup(dict, "count", count)) {
                error_ = "array type is missing its count";
                return false;
            }
            type_ptr element_type;
            if (!resolve_ref(element, element_type, depth + 1))
                return false;
            std::size_t length = 0;
            if (!as_size(count, length) || length == 0) {
                error_ = "array count must be a positive integer";
                return false;
            }
            type_ptr built;
            if (!make_array(std::move(element_type), length, built, error_))
                return false;
            out = std::move(built);
            return true;
        }
        if (kind == "callback") {
            struct value_t *result = nullptr;
            if (!dict_lookup(dict, "result", result) &&
                !dict_lookup(dict, "returns", result)) {
                error_ = "callback type is missing its result";
                return false;
            }
            type_ptr result_type;
            if (!resolve_ref(result, result_type, depth + 1))
                return false;
            std::vector<type_ptr> args;
            struct value_t *argument_values = nullptr;
            if (dict_lookup(dict, "args", argument_values) ||
                dict_lookup(dict, "arguments", argument_values)) {
                if (argument_values->type != T_list) {
                    error_ = "callback args must be a list";
                    return false;
                }
                for (struct value_t *item :
                     ((list_value_t *)argument_values)->payload->items) {
                    type_ptr arg;
                    if (!resolve_ref(item, arg, depth + 1))
                        return false;
                    args.push_back(std::move(arg));
                }
            }
            type_ptr built;
            if (!make_callback(std::move(result_type), std::move(args),
                               built, error_))
                return false;
            out = std::move(built);
            return true;
        }
        if (kind == "opaque") {
            out = make_opaque(name);
            return true;
        }
        error_ = "unknown type kind " + kind;
        return false;
    }

    bool fill(const std::string &name) {
        if (filled_.count(name))
            return true;
        if (filling_.count(name)) {
            error_ = "recursive by-value type " + name;
            return false;
        }
        filling_.insert(name);
        struct value_t *decl = nullptr;
        if (!dict_lookup(types_, name.c_str(), decl)) {
            error_ = "unknown manifest type " + name;
            return false;
        }
        type_ptr built;
        if (!build_decl(name, decl, built, 0)) {
            filling_.erase(name);
            return false;
        }
        std::shared_ptr<type> slot = slots_[name];
        *slot = *built;
        if (slot->owned_cif) {
            slot->cif = slot->owned_cif.get();
            slot->owned_cif->elements = slot->cif_elements.data();
        }
        filling_.erase(name);
        filled_.insert(name);
        return true;
    }

    const dict_value_t *types_;
    const std::map<std::string, type_ptr> *base_;
    std::string &error_;
    std::map<std::string, std::shared_ptr<type>> slots_;
    std::map<std::string, type_ptr> resolved_;
    std::set<std::string> filling_;
    std::set<std::string> filled_;
};

bool parse_index_list(struct value_t *value, std::size_t limit,
                      std::vector<std::size_t> &out, const char *what,
                      std::string &error) {
    if (!value)
        return true;
    if (value->type != T_list) {
        error = std::string(what) + " must be a list of argument indices";
        return false;
    }
    for (struct value_t *item : ((list_value_t *)value)->payload->items) {
        std::size_t index = 0;
        if (!as_size(item, index) || index >= limit) {
            error = std::string(what) + " contains an invalid argument index";
            return false;
        }
        out.push_back(index);
    }
    return true;
}

bool parse_returns_owned(struct value_t *value, bool &owned,
                         std::string &free_function, type_ptr &free_result,
                         std::vector<type_ptr> &free_args,
                         type_resolver &resolver, std::string &error) {
    owned = false;
    free_function.clear();
    free_result = scalar("void");
    free_args.clear();
    if (!value)
        return true;
    if (value->type == T_bool) {
        owned = ((bool_value_t *)value)->value;
        if (owned) {
            free_function = "free";
            free_args.push_back(scalar("ptr"));
        }
        return true;
    }
    if (value->type != T_dict) {
        error = "returns_owned must be a bool or an object";
        return false;
    }

    owned = true;
    free_function = "free";
    free_args.push_back(scalar("ptr"));
    dict_value_t *decl = (dict_value_t *)value;
    struct value_t *item = nullptr;
    if (dict_lookup(decl, "free", item)) {
        if (!as_string(item, free_function) || free_function.empty()) {
            error = "returns_owned.free must be a function name";
            return false;
        }
    }
    if (dict_lookup(decl, "free_result", item)) {
        if (!resolver.resolve_ref(item, free_result, 0))
            return false;
        if (free_result->kind == kind_t::record ||
            free_result->kind == kind_t::array) {
            error = "returns_owned.free_result must be a scalar or void";
            return false;
        }
    }
    free_args.clear();
    if (dict_lookup(decl, "free_args", item)) {
        if (item->type != T_list) {
            error = "returns_owned.free_args must be a list";
            return false;
        }
        for (struct value_t *arg :
             ((list_value_t *)item)->payload->items) {
            type_ptr type;
            if (!resolver.resolve_ref(arg, type, 0))
                return false;
            free_args.push_back(std::move(type));
        }
    } else {
        free_args.push_back(scalar("ptr"));
    }
    if (free_args.size() != 1 ||
        (free_args[0]->kind != kind_t::pointer &&
         free_args[0]->kind != kind_t::opaque)) {
        error = "returns_owned.free must take one pointer argument";
        return false;
    }
    return true;
}

bool build_manifest(struct value_t *spec,
                    const std::map<std::string, type_ptr> *base,
                    std::shared_ptr<manifest> &out, std::string &error) {
    if (!spec || spec->type != T_dict) {
        error = "manifest must be a dict";
        return false;
    }
    dict_value_t *root = (dict_value_t *)spec;
    std::shared_ptr<manifest> result = std::make_shared<manifest>();

    struct value_t *value = nullptr;
    if (dict_lookup(root, "ffi", value)) {
        std::size_t version = 0;
        if (!as_size(value, version) || version != 1) {
            error = "unsupported manifest version; expected ffi: 1";
            return false;
        }
        result->version = (int)version;
    } else {
        error = "manifest is missing the ffi version";
        return false;
    }
    if (dict_lookup(root, "library", value))
        as_string(value, result->name);

    if (dict_lookup(root, "abi", value) && !validate_abi(value, error))
        return false;

    dict_value_t *types = nullptr;
    if (dict_lookup(root, "types", value)) {
        if (value->type != T_dict) {
            error = "manifest types must be a dict";
            return false;
        }
        types = (dict_value_t *)value;
    }
    type_resolver resolver(types, base, error);
    if (!resolver.prepare() || !resolver.finish())
        return false;
    result->types = resolver.types();

    dict_value_t *functions = nullptr;
    if (dict_lookup(root, "functions", value)) {
        if (value->type != T_dict) {
            error = "manifest functions must be a dict";
            return false;
        }
        functions = (dict_value_t *)value;
    }
    if (functions) {
        if (functions->payload->items.size() > MAX_MANIFEST_FUNCTIONS) {
            error = "manifest declares too many functions";
            return false;
        }
        for (const auto &entry : functions->payload->items) {
            std::string name;
            if (!as_string(entry.first, name) || name.empty()) {
                error = "manifest function names must be non-empty strings";
                return false;
            }
            if (entry.second->type != T_dict) {
                error = "manifest function " + name + " must be a dict";
                return false;
            }
            dict_value_t *decl = (dict_value_t *)entry.second;
            manifest_function fn;
            fn.name = name;
            fn.symbol = name;

            struct value_t *item = nullptr;
            if (dict_lookup(decl, "symbol", item)) {
                if (!as_string(item, fn.symbol) || fn.symbol.empty()) {
                    error = "function " + name + " has an invalid symbol";
                    return false;
                }
            }
            if (dict_lookup(decl, "variadic", item) &&
                !as_bool(item, fn.variadic)) {
                error = "function " + name +
                        " has an invalid variadic flag";
                return false;
            }
            if (dict_lookup(decl, "calling_convention", item)) {
                std::string convention;
                if (!as_string(item, convention) ||
                    (convention != "c" && convention != "cdecl" &&
                     convention != "stdcall" && convention != "fastcall" &&
                     convention != "thiscall")) {
                    error = "function " + name +
                            " uses an unsupported calling convention";
                    return false;
                }
                fn.calling_convention = convention;
            }

            fn.result = scalar("void");
            if (dict_lookup(decl, "result", item) ||
                dict_lookup(decl, "returns", item)) {
                if (!resolver.resolve_ref(item, fn.result, 0))
                    return false;
            }
            struct value_t *arguments = nullptr;
            if (dict_lookup(decl, "args", arguments) ||
                dict_lookup(decl, "arguments", arguments)) {
                if (arguments->type != T_list) {
                    error = "function " + name + " args must be a list";
                    return false;
                }
                for (struct value_t *arg :
                     ((list_value_t *)arguments)->payload->items) {
                    type_ptr type;
                    if (!resolver.resolve_ref(arg, type, 0))
                        return false;
                    fn.args.push_back(std::move(type));
                }
            }
            if (fn.args.size() > MAX_CALL_ARGS) {
                error = "function " + name + " has too many arguments";
                return false;
            }
            if (dict_lookup(decl, "consumes", item) &&
                !parse_index_list(item, fn.args.size(), fn.consumes,
                                  "consumes", error))
                return false;
            if (dict_lookup(decl, "retains", item) &&
                !parse_index_list(item, fn.args.size(), fn.retains,
                                  "retains", error))
                return false;
            if (dict_lookup(decl, "returns_owned", item) &&
                !parse_returns_owned(item, fn.returns_owned,
                                     fn.free_function, fn.free_result,
                                     fn.free_args, resolver, error))
                return false;
            for (std::size_t index : fn.consumes) {
                kind_t kind = fn.args[index]->kind;
                if (kind != kind_t::pointer && kind != kind_t::opaque) {
                    error = "function " + name +
                            " consumes a non-pointer argument";
                    return false;
                }
            }
            for (std::size_t index : fn.retains) {
                if (!is_pointer_like(fn.args[index]->kind)) {
                    error = "function " + name +
                            " retains a non-pointer argument";
                    return false;
                }
            }
            if (fn.returns_owned &&
                fn.result->kind != kind_t::pointer &&
                fn.result->kind != kind_t::opaque &&
                fn.result->kind != kind_t::callback) {
                error = "function " + name +
                        " returns_owned needs a pointer result";
                return false;
            }
            result->functions.emplace(name, std::move(fn));
        }
    }

    if (dict_lookup(root, "variables", value)) {
        if (value->type != T_dict) {
            error = "manifest variables must be a dict";
            return false;
        }
        for (const auto &entry :
             ((dict_value_t *)value)->payload->items) {
            std::string name;
            if (!as_string(entry.first, name) || name.empty()) {
                error = "manifest variable names must be non-empty strings";
                return false;
            }
            if (result->functions.count(name) ||
                result->variables.count(name)) {
                error = "duplicate manifest symbol " + name;
                return false;
            }
            manifest_variable variable;
            variable.name = name;
            variable.symbol = name;
            struct value_t *type_value = entry.second;
            if (entry.second->type == T_dict) {
                dict_value_t *decl = (dict_value_t *)entry.second;
                struct value_t *item = nullptr;
                if (dict_lookup(decl, "symbol", item)) {
                    if (!as_string(item, variable.symbol) ||
                        variable.symbol.empty()) {
                        error = "variable " + name +
                                " has an invalid symbol";
                        return false;
                    }
                }
                if (dict_lookup(decl, "type", item))
                    type_value = item;
                bool read_only = false;
                if ((dict_lookup(decl, "const", item) ||
                     dict_lookup(decl, "readonly", item)) &&
                    !as_bool(item, read_only)) {
                    error = "variable " + name +
                            " has an invalid const flag";
                    return false;
                }
                variable.writable = !read_only;
            }
            if (!resolver.resolve_ref(type_value, variable.type, 0))
                return false;
            result->variables.emplace(name, std::move(variable));
        }
    }

    if (dict_lookup(root, "constants", value)) {
        if (value->type != T_dict) {
            error = "manifest constants must be a dict";
            return false;
        }
        for (const auto &entry : ((dict_value_t *)value)->payload->items) {
            std::string name;
            if (!as_string(entry.first, name) || name.empty()) {
                error = "manifest constant names must be non-empty strings";
                return false;
            }
            if (result->functions.count(name) ||
                result->variables.count(name)) {
                error = "duplicate manifest symbol " + name;
                return false;
            }
            switch (entry.second->type) {
                case T_int:
                case T_float:
                case T_string:
                case T_bool:
                case T_null:
                    break;
                default:
                    error = "constant " + name +
                            " must be an int, float, string, bool, or null";
                    return false;
            }
            result->constants[name] = dupval(entry.second);
        }
    }

    out = std::move(result);
    return true;
}

struct manifest_handle {
    std::shared_ptr<const manifest> value;
};

std::shared_ptr<manifest> clone_manifest(const manifest &source) {
    std::shared_ptr<manifest> copy = std::make_shared<manifest>();
    copy->version = source.version;
    copy->name = source.name;
    copy->types = source.types;
    copy->functions = source.functions;
    copy->variables = source.variables;
    for (const auto &entry : source.constants)
        copy->constants.emplace(entry.first, dupval(entry.second));
    return copy;
}

std::string describe_manifest(const void *data) {
    const manifest_handle *handle = (const manifest_handle *)data;
    if (!handle || !handle->value)
        return "<ffi_manifest>";
    return "<ffi_manifest " + handle->value->name + ">";
}

} // namespace

bool extend_manifest(const manifest &base, struct value_t *spec,
                     std::shared_ptr<const manifest> &out,
                     std::string &error) {
    std::shared_ptr<manifest> extension;
    if (!build_manifest(spec, &base.types, extension, error))
        return false;

    std::shared_ptr<manifest> merged = clone_manifest(base);
    for (const auto &entry : extension->types) {
        if (!merged->types.emplace(entry.first, entry.second).second) {
            error = "duplicate manifest type " + entry.first;
            return false;
        }
    }
    for (const auto &entry : extension->functions) {
        if (merged->variables.count(entry.first) ||
            !merged->functions.emplace(entry.first, entry.second).second) {
            error = "duplicate manifest function " + entry.first;
            return false;
        }
    }
    for (const auto &entry : extension->variables) {
        if (merged->functions.count(entry.first) ||
            !merged->variables.emplace(entry.first, entry.second).second) {
            error = "duplicate manifest variable " + entry.first;
            return false;
        }
    }
    for (const auto &entry : extension->constants) {
        if (merged->constants.count(entry.first) ||
            merged->functions.count(entry.first) ||
            merged->variables.count(entry.first)) {
            error = "duplicate manifest constant " + entry.first;
            return false;
        }
        merged->constants.emplace(entry.first, dupval(entry.second));
    }
    out = std::move(merged);
    return true;
}

const native_type manifest_native_type = {
    "ffi_manifest", describe_manifest};

manifest::~manifest() {
    for (auto &entry : constants)
        freeval(entry.second);
}

bool load_manifest(struct value_t *spec,
                   std::shared_ptr<const manifest> &out, std::string &error) {
    if (spec && spec->type == T_userdata &&
        ((userdata_value_t *)spec)->meta == &manifest_native_type)
        return get_manifest(spec, out);

    struct value_t *parsed = nullptr;
    if (spec && spec->type == T_string) {
        std::string path = ((string_value_t *)spec)->value;
        std::string text;
        if (!read_file(path, text, error))
            return false;
        parsed = json_parse_text(text, error);
        if (!parsed)
            return false;
    } else if (spec && spec->type == T_dict) {
        parsed = spec;
    } else {
        error = "manifest must be a dict or a JSON file path";
        return false;
    }

    std::shared_ptr<manifest> built;
    bool ok = build_manifest(parsed, nullptr, built, error);
    if (parsed != spec)
        freeval(parsed);
    if (ok)
        out = std::move(built);
    return ok;
}

struct value_t *new_manifest_value(std::shared_ptr<const manifest> manifest) {
    std::shared_ptr<manifest_handle> holder = std::make_shared<manifest_handle>();
    holder->value = std::move(manifest);
    return newuserdataval(&manifest_native_type, holder);
}

bool get_manifest(struct value_t *value,
                  std::shared_ptr<const manifest> &out) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &manifest_native_type) {
        yyerror("expected an ffi manifest, got %s", typename_of(value));
        return false;
    }
    std::shared_ptr<manifest_handle> holder =
        std::static_pointer_cast<manifest_handle>(
            ((userdata_value_t *)value)->data);
    if (!holder || !holder->value) {
        yyerror("invalid ffi manifest");
        return false;
    }
    out = holder->value;
    return true;
}

} // namespace ffi
