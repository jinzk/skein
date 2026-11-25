#ifndef _STRUCTTYPE_H_
#define _STRUCTTYPE_H_

#include "symbol.h"
#include "valuetype.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct field_def {
    std::string name;
    struct type_constraint type;
    bool has_default = false;
    std::shared_ptr<function_def> default_value;
};

struct method_sig {
    std::string name;
    /* Includes the receiver as the first parameter. */
    std::vector<struct type_constraint> params;
    int required_params = 0;
    bool variadic = false;
    /* Null means the protocol does not constrain the return type. */
    std::shared_ptr<const struct type_constraint> return_type;
};

struct protocol_def {
    std::string name;
    std::string source;
    std::string package;
    std::string module;
    bool exported;
    std::vector<method_sig> methods;
    /* Methods with a default body, keyed by method name. */
    std::unordered_map<std::string, std::shared_ptr<function_def>> defaults;
};

struct variant_def {
    std::string name;
    std::vector<field_def> fields;
    std::unordered_map<std::string, int> field_index;
};

enum type_owner_kind {
    TYPE_OWNER_STRUCT,
    TYPE_OWNER_ENUM,
};

struct type_def {
    enum type_owner_kind kind;
    std::string name;
    std::string source;
    std::string package;
    std::string module;
    bool exported;
    std::unordered_map<std::string, std::shared_ptr<function_def>> methods;
    /* Method names explicitly declared by `impl Type`, not injected defaults. */
    std::unordered_set<std::string> explicit_methods;
    /*
     * Method names supplied by a protocol default rather than an impl, mapped
     * to the protocol that supplied them (for conflict detection).
     */
    std::unordered_map<std::string, const protocol_def *> default_methods;
};

struct enum_def : type_def {
    std::vector<variant_def> variants;
    std::unordered_map<std::string, int> variant_index;
};

struct struct_def : type_def {
    std::vector<field_def> fields;
    std::unordered_map<std::string, int> field_index;
};

struct struct_payload {
    std::vector<value_t *> fields;
    /* Cached structural hash for structs used as dict/set keys.  Struct
     * values use copy-on-write, so the payload stored in a dict key never
     * changes in place. */
    mutable std::uint64_t hash = 0;
    mutable bool hash_valid = false;
    ~struct_payload();
};

struct struct_value_t {
    enum val_type type;
    struct_def *def;
    std::shared_ptr<struct_payload> payload;
};

struct method_value_t {
    enum val_type type;
    /* Owned receiver snapshot. */
    value_t *receiver;
    std::shared_ptr<function_def> method;
};

struct enum_value_t {
    enum val_type type;
    enum_def *def;
    int variant;
    std::shared_ptr<struct_payload> payload;
};

void structtype_shutdown();

/*
 * Visit long-lived function definitions owned by the type registry (impl
 * methods and field defaults).  The cycle collector uses this to keep their
 * upvalue cells reachable.
 */
void structtype_visit_functions(
    const std::function<void(const std::shared_ptr<function_def> &)> &visit);

struct_def *structtype_find_struct(const char *module, const char *name);
struct_def *structtype_find_struct_by_name(const char *package,
                                           const char *name,
                                           bool *ambiguous = NULL);
type_def *structtype_find_type(const char *module, const char *name);
protocol_def *structtype_find_protocol(const char *module, const char *name);
protocol_def *structtype_find_protocol_by_name(const char *package,
                                         const char *name,
                                         bool *ambiguous = NULL);
protocol_def *structtype_resolve_protocol(const char *module, const char *name,
                                    bool require_exported,
                                    std::string &error);
enum_def *structtype_find_enum(const char *module, const char *name);
enum_def *structtype_find_enum_by_name(const char *package,
                                       const char *name,
                                       bool *ambiguous = NULL);
bool structtype_type_exists(const char *module, const char *name);
bool named_type_visible(const char *source, const char *name);

enum structtype_lookup_kind {
    STRUCTTYPE_LOOKUP_NONE,
    STRUCTTYPE_LOOKUP_STRUCT,
    STRUCTTYPE_LOOKUP_ENUM,
    STRUCTTYPE_LOOKUP_PROTOCOL,
};

enum structtype_lookup_status {
    STRUCTTYPE_LOOKUP_FOUND,
    STRUCTTYPE_LOOKUP_NOT_FOUND,
    STRUCTTYPE_LOOKUP_AMBIGUOUS,
    STRUCTTYPE_LOOKUP_NOT_VISIBLE,
    STRUCTTYPE_LOOKUP_ERROR,
};

struct structtype_lookup_result {
    enum structtype_lookup_kind kind;
    struct_def *structure;
    enum_def *enumeration;
    protocol_def *protocol;

    structtype_lookup_result()
        : kind(STRUCTTYPE_LOOKUP_NONE),
          structure(NULL),
          enumeration(NULL),
          protocol(NULL) {}
};

/*
 * Resolve a type name as seen from a source file / logical module.
 *
 * Types declared in importer_source or importer_module are visible even when
 * not exported.  Other types must be exported.  Qualified names resolve
 * import aliases registered for importer_source.
 */
enum structtype_lookup_status structtype_lookup_type(
    const char *importer_source, const char *importer_module,
    const char *name, struct structtype_lookup_result &out,
    std::string &error);

bool structtype_create_struct(const char *source, const char *package,
                              const char *module, const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error);
bool structtype_prepare_struct(const char *source, const char *package,
                               const char *module, const char *name, bool exported,
                               struct_def *&out, std::string &error);
bool structtype_finalize_struct(struct_def *def,
                                std::vector<field_def> fields,
                                std::string &error);
void structtype_rollback_struct(struct_def *def);
bool structtype_create_protocol(const char *source, const char *package,
                             const char *module, const char *name, bool exported,
                             std::vector<method_sig> methods,
                             protocol_def *&out, std::string &error);
bool structtype_create_enum(const char *source, const char *package,
                            const char *module, const char *name, bool exported,
                            std::vector<variant_def> variants,
                            enum_def *&out, std::string &error);
bool structtype_add_protocol_default(protocol_def *protocol, const std::string &name,
                                  std::shared_ptr<function_def> code,
                                  std::string &error);
bool structtype_prepare_enum(const char *source, const char *package,
                             const char *module, const char *name, bool exported,
                             enum_def *&out, std::string &error);
bool structtype_finalize_enum(enum_def *def,
                              std::vector<variant_def> variants,
                              std::string &error);
void structtype_rollback_enum(enum_def *def);
bool structtype_add_method(type_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error);
bool structtype_set_default_method(type_def *def, const std::string &name,
                                   std::shared_ptr<function_def> code,
                                   const protocol_def *protocol,
                                   std::string &error);
std::shared_ptr<function_def> structtype_method(const type_def *def,
                                                const std::string &name);
void structtype_set_thread_method(const type_def *def, const std::string &name,
                                  std::shared_ptr<function_def> method);
uint64_t structtype_method_generation();
std::vector<std::string> structtype_method_names(const type_def *def);
std::shared_ptr<function_def> structtype_protocol_default(
    const protocol_def *protocol, const std::string &name);

/* Structural protocol conformance and injected default-method refresh. */
bool structtype_method_signature_matches(const method_sig &sig,
                                          const struct function_code *code,
                                          std::string &error);
bool structtype_satisfies_protocol(const type_def *def,
                                   const protocol_def *protocol);
std::vector<const protocol_def *> structtype_protocols(const type_def *def);
bool structtype_refresh_protocol_defaults(type_def *def, std::string &error);
bool structtype_refresh_all_protocol_defaults(std::string &error);
bool structtype_has_protocol(const type_def *def, const protocol_def *protocol);
int structtype_field_index(const struct_def *def, const std::string &name);
int structtype_variant_index(const enum_def *def, const std::string &name);
int structtype_variant_field_index(const variant_def *variant,
                                   const std::string &name);

/* Compare named types after resolving package-qualified names and aliases. */
bool structtype_named_types_equal(const char *module_a, const char *name_a,
                                  const char *module_b, const char *name_b);

/* importer is the source identity; target is the package/module name. */
void structtype_register_alias(const char *importer, const char *alias,
                               const char *target);

struct value_t *newstructval(struct_def *def,
                             std::shared_ptr<struct_payload> payload);
struct value_t *newenumval(enum_def *def, int variant,
                           std::shared_ptr<struct_payload> payload);
struct value_t *newmethodval(value_t *receiver,
                             std::shared_ptr<function_def> method);
void ensure_unique_struct(struct struct_value_t *value);

/* Runtime implementation of a TYPE_NAMED type constraint. */
bool named_type_matches(const char *module, const char *name,
                        const struct value_t *value);

#endif
