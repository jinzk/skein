#ifndef _STRUCTTYPE_H_
#define _STRUCTTYPE_H_

#include "symbol.h"
#include "valuetype.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct field_def {
    std::string name;
    struct type_constraint type;
};

struct method_sig {
    std::string name;
    /* Includes the receiver as the first parameter. */
    std::vector<struct type_constraint> params;
};

struct trait_def {
    std::string name;
    std::string module;
    std::string module_name;
    bool exported;
    std::vector<method_sig> methods;
};

struct variant_def {
    std::string name;
    std::vector<field_def> fields;
    std::unordered_map<std::string, int> field_index;
};

struct enum_def {
    std::string name;
    std::string module;
    std::string module_name;
    bool exported;
    std::vector<variant_def> variants;
    std::unordered_map<std::string, int> variant_index;
};

struct struct_def {
    std::string name;
    std::string module;
    std::string module_name;
    bool exported;
    std::vector<field_def> fields;
    std::unordered_map<std::string, int> field_index;
    std::unordered_map<std::string, std::shared_ptr<function_def>> methods;
    std::vector<const trait_def *> traits;
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
    struct_def *def;
    std::shared_ptr<struct_payload> receiver;
    std::shared_ptr<function_def> method;
};

struct enum_value_t {
    enum val_type type;
    enum_def *def;
    int variant;
    std::shared_ptr<struct_payload> payload;
};

void structtype_shutdown();

struct_def *structtype_find_struct(const char *module, const char *name);
struct_def *structtype_find_struct_by_name(const char *module_name,
                                           const char *name);
trait_def *structtype_find_trait(const char *module, const char *name);
enum_def *structtype_find_enum(const char *module, const char *name);
enum_def *structtype_find_enum_by_name(const char *module_name,
                                       const char *name);
bool structtype_type_exists(const char *module, const char *name);

bool structtype_create_struct(const char *module, const char *module_name,
                              const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error);
bool structtype_prepare_struct(const char *module, const char *module_name,
                               const char *name, bool exported,
                               struct_def *&out, std::string &error);
bool structtype_finalize_struct(struct_def *def,
                                std::vector<field_def> fields,
                                std::string &error);
void structtype_rollback_struct(struct_def *def);
bool structtype_create_trait(const char *module, const char *module_name,
                             const char *name, bool exported,
                             std::vector<method_sig> methods,
                             trait_def *&out, std::string &error);
bool structtype_create_enum(const char *module, const char *module_name,
                            const char *name, bool exported,
                            std::vector<variant_def> variants,
                            enum_def *&out, std::string &error);
bool structtype_prepare_enum(const char *module, const char *module_name,
                             const char *name, bool exported,
                             enum_def *&out, std::string &error);
bool structtype_finalize_enum(enum_def *def,
                              std::vector<variant_def> variants,
                              std::string &error);
void structtype_rollback_enum(enum_def *def);
bool structtype_add_method(struct_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error);
bool structtype_add_trait(struct_def *def, trait_def *trait,
                          std::string &error);

bool structtype_has_trait(const struct_def *def, const trait_def *trait);
int structtype_field_index(const struct_def *def, const std::string &name);
int structtype_variant_index(const enum_def *def, const std::string &name);
int structtype_variant_field_index(const variant_def *variant,
                                   const std::string &name);

/* importer is the source identity; target is the package/module name. */
void structtype_register_alias(const char *importer, const char *alias,
                               const char *target);

struct value_t *newstructval(struct_def *def,
                             std::shared_ptr<struct_payload> payload);
struct value_t *newenumval(enum_def *def, int variant,
                           std::shared_ptr<struct_payload> payload);
struct value_t *newmethodval(struct_def *def,
                             std::shared_ptr<struct_payload> receiver,
                             std::shared_ptr<function_def> method);
void ensure_unique_struct(struct struct_value_t *value);

/* Runtime implementation of a TYPE_NAMED type constraint. */
bool named_type_matches(const char *module, const char *name,
                        const struct value_t *value);

#endif
