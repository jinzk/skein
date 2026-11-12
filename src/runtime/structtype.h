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
};

struct trait_def {
    std::string name;
    std::string source;
    std::string package;
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
    bool exported;
    std::unordered_map<std::string, std::shared_ptr<function_def>> methods;
    /*
     * Method names supplied by a trait default rather than an impl, mapped
     * to the trait that supplied them (for conflict detection).
     */
    std::unordered_map<std::string, const trait_def *> default_methods;
    std::vector<const trait_def *> traits;
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
trait_def *structtype_find_trait(const char *module, const char *name);
trait_def *structtype_find_trait_by_name(const char *package,
                                         const char *name,
                                         bool *ambiguous = NULL);
trait_def *structtype_resolve_trait(const char *module, const char *name,
                                    bool require_exported,
                                    std::string &error);
enum_def *structtype_find_enum(const char *module, const char *name);
enum_def *structtype_find_enum_by_name(const char *package,
                                       const char *name,
                                       bool *ambiguous = NULL);
bool structtype_type_exists(const char *module, const char *name);

enum structtype_lookup_kind {
    STRUCTTYPE_LOOKUP_NONE,
    STRUCTTYPE_LOOKUP_STRUCT,
    STRUCTTYPE_LOOKUP_ENUM,
    STRUCTTYPE_LOOKUP_TRAIT,
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
    trait_def *trait;

    structtype_lookup_result()
        : kind(STRUCTTYPE_LOOKUP_NONE),
          structure(NULL),
          enumeration(NULL),
          trait(NULL) {}
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
                              const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error);
bool structtype_prepare_struct(const char *source, const char *package,
                               const char *name, bool exported,
                               struct_def *&out, std::string &error);
bool structtype_finalize_struct(struct_def *def,
                                std::vector<field_def> fields,
                                std::string &error);
void structtype_rollback_struct(struct_def *def);
bool structtype_create_trait(const char *source, const char *package,
                             const char *name, bool exported,
                             std::vector<method_sig> methods,
                             trait_def *&out, std::string &error);
bool structtype_create_enum(const char *source, const char *package,
                            const char *name, bool exported,
                            std::vector<variant_def> variants,
                            enum_def *&out, std::string &error);
bool structtype_add_trait_default(trait_def *trait, const std::string &name,
                                  std::shared_ptr<function_def> code,
                                  std::string &error);
bool structtype_prepare_enum(const char *source, const char *package,
                             const char *name, bool exported,
                             enum_def *&out, std::string &error);
bool structtype_finalize_enum(enum_def *def,
                              std::vector<variant_def> variants,
                              std::string &error);
void structtype_rollback_enum(enum_def *def);
bool structtype_add_method(type_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error);
bool structtype_add_trait(type_def *def, trait_def *trait,
                          std::string &error);

bool structtype_has_trait(const type_def *def, const trait_def *trait);
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
