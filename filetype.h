#ifndef CALC_FILETYPE_H
#define CALC_FILETYPE_H

#include <memory>
#include <string>

#include "platform.h"
#include "valuetype.h"

struct file_resource {
    platform_file_ptr file;
    std::string path;
};

extern const struct native_type file_native_type;

bool is_file_value(const struct value_t *value);
std::shared_ptr<file_resource> file_value(const struct value_t *value);
struct value_t *new_file_value(platform_file_ptr file, const std::string &path);

/* Normalize r/w/a and optional + into a binary fopen mode. */
std::string normalize_file_mode(const std::string &mode, std::string &error);

#endif
