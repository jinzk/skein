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
    std::vector<method_sig> methods;
};

struct struct_def {
    std::string name;
    std::string module;
    std::string module_name;
    std::vector<field_def> fields;
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

void structtype_shutdown();

struct_def *structtype_find_struct(const char *module, const char *name);
trait_def *structtype_find_trait(const char *module, const char *name);
bool structtype_type_exists(const char *module, const char *name);

bool structtype_create_struct(const char *module, const char *module_name,
                              const char *name, std::vector<field_def> fields,
                              struct_def *&out, std::string &error);
bool structtype_create_trait(const char *module, const char *module_name,
                             const char *name, std::vector<method_sig> methods,
                             trait_def *&out, std::string &error);
bool structtype_add_method(struct_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error);
bool structtype_add_trait(struct_def *def, trait_def *trait,
                          std::string &error);

bool structtype_has_trait(const struct_def *def, const trait_def *trait);
int structtype_field_index(const struct_def *def, const std::string &name);

struct value_t *newstructval(struct_def *def,
                             std::shared_ptr<struct_payload> payload);
void ensure_unique_struct(struct struct_value_t *value);

/* Runtime implementation of a TYPE_NAMED type constraint. */
bool named_type_matches(const char *module, const char *name,
                        const struct value_t *value);

#endif
