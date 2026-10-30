#include "filetype.h"

#include <memory>
#include <string>

static std::string describe_file(const void *data) {
    const file_resource *resource = static_cast<const file_resource *>(data);
    if (!resource)
        return "<file>";
    std::string text = "<file: " + resource->path;
    if (!platform_file_is_open(resource->file))
        text += " (closed)";
    text += ">";
    return text;
}

const struct native_type file_native_type = {
    "file",
    describe_file,
};

bool is_file_value(const struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    const userdata_value_t *userdata =
        (const userdata_value_t *)value;
    return userdata->meta == &file_native_type;
}

std::shared_ptr<file_resource> file_value(const struct value_t *value) {
    if (!is_file_value(value))
        return nullptr;
    const userdata_value_t *userdata =
        (const userdata_value_t *)value;
    return std::static_pointer_cast<file_resource>(userdata->data);
}

struct value_t *new_file_value(platform_file_ptr file,
                               const std::string &path) {
    std::shared_ptr<file_resource> resource =
        std::make_shared<file_resource>();
    resource->file = std::move(file);
    resource->path = path;
    return newuserdataval(&file_native_type, resource);
}

std::string normalize_file_mode(const std::string &mode, std::string &error) {
    error.clear();
    if (mode.empty()) {
        error = "invalid file mode";
        return std::string();
    }

    char first = mode[0];
    if (first != 'r' && first != 'w' && first != 'a') {
        error = "invalid file mode";
        return std::string();
    }

    bool plus = false;
    for (size_t i = 1; i < mode.size(); ++i) {
        char c = mode[i];
        if (c == '+') {
            if (plus) {
                error = "invalid file mode";
                return std::string();
            }
            plus = true;
        } else if (c != 'b' && c != 't') {
            error = "invalid file mode";
            return std::string();
        }
    }

    std::string normalized;
    normalized += first;
    if (plus)
        normalized += '+';
    normalized += 'b';
    return normalized;
}
