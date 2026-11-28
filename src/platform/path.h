#ifndef SKEIN_PLATFORM_PATH_H
#define SKEIN_PLATFORM_PATH_H

#include <filesystem>
#include <string>
#include <string_view>

namespace skein {

namespace fs = std::filesystem;

/* Convert UTF-8 text to a native filesystem path.  On Windows this must not
 * use fs::path(std::string), which interprets bytes using the active ANSI
 * code page. */
inline fs::path path_from_utf8(std::string_view text) {
    return fs::u8path(std::string(text));
}

/* Convert a native path back to UTF-8 text. */
inline std::string path_to_utf8(const fs::path &path) {
    return path.u8string();
}

/* UTF-8 text with generic ('/') separators, for URIs and serialized paths. */
inline std::string path_to_utf8_generic(const fs::path &path) {
    return path.generic_u8string();
}

} // namespace skein

#endif
