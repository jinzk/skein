#ifndef SKEIN_BUILTINS_TEXT_INTERNAL_H
#define SKEIN_BUILTINS_TEXT_INTERNAL_H

#include <cctype>
#include <string>

namespace skein {
namespace builtins_internal {

/* ASCII-only lowercasing used for header names and tokens.  Converting to
 * unsigned char first avoids passing a negative char to std::tolower. */
inline std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        value[i] = (char)std::tolower(c);
    }
    return value;
}

} // namespace builtins_internal
} // namespace skein

#endif
