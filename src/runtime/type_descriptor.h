#ifndef SKEIN_TYPE_DESCRIPTOR_H
#define SKEIN_TYPE_DESCRIPTOR_H

#include <memory>
#include <string>
#include <vector>

/*
 * Parsed form of a type descriptor such as "Box[int]" or
 * "dict[string, list[int]]".  Descriptors are syntax-level: they name types
 * and type arguments but do not carry runtime values.
 */
struct type_descriptor;
using type_descriptor_ptr = std::shared_ptr<type_descriptor>;

struct type_descriptor {
    /* Base type name, without package qualifier. */
    std::string name;
    /* Package/module qualifier before the final dot, if any. */
    std::string package;
    std::vector<type_descriptor_ptr> args;
    bool nullable = false;
};

bool parse_type_descriptor(const std::string &text, type_descriptor &out,
                           std::string &error);
std::string type_descriptor_display(const type_descriptor &desc);

#endif
