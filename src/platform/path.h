#ifndef SKEIN_PLATFORM_PATH_H
#define SKEIN_PLATFORM_PATH_H

#include <filesystem>
#include <string>
#include <string_view>

namespace skein {

namespace fs = std::filesystem;

/* Validate UTF-8 text before handing it to narrow external APIs.  Embedded
 * NUL bytes are rejected as well. */
inline bool is_valid_utf8(std::string_view text) {
    if (text.find('\0') != std::string_view::npos)
        return false;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned char lower = 0x80;
        unsigned char upper = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            if (lead == 0xE0)
                lower = 0xA0;
            if (lead == 0xED)
                upper = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            if (lead == 0xF0)
                lower = 0x90;
            if (lead == 0xF4)
                upper = 0x8F;
        } else {
            return false;
        }
        if (i + extra >= text.size())
            return false;
        const unsigned char first = static_cast<unsigned char>(text[i + 1]);
        if (first < lower || first > upper)
            return false;
        for (std::size_t j = 2; j <= extra; ++j) {
            const unsigned char byte = static_cast<unsigned char>(text[i + j]);
            if (byte < 0x80 || byte > 0xBF)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

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
