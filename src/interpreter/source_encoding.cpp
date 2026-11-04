#include "source_encoding.h"

#include "encoding.h"
#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::string configured_encoding;

struct bom_info {
    const char *name;
    size_t length;
};

bool detect_bom(const std::string &raw, bom_info &bom) {
    if (raw.size() >= 4 &&
        (unsigned char)raw[0] == 0x00 &&
        (unsigned char)raw[1] == 0x00 &&
        (unsigned char)raw[2] == 0xFE &&
        (unsigned char)raw[3] == 0xFF) {
        bom = {"utf-32be", 4};
        return true;
    }
    if (raw.size() >= 4 &&
        (unsigned char)raw[0] == 0xFF &&
        (unsigned char)raw[1] == 0xFE &&
        (unsigned char)raw[2] == 0x00 &&
        (unsigned char)raw[3] == 0x00) {
        bom = {"utf-32le", 4};
        return true;
    }
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        bom = {"utf-8", 3};
        return true;
    }
    if (raw.size() >= 2 &&
        (unsigned char)raw[0] == 0xFF &&
        (unsigned char)raw[1] == 0xFE) {
        bom = {"utf-16le", 2};
        return true;
    }
    if (raw.size() >= 2 &&
        (unsigned char)raw[0] == 0xFE &&
        (unsigned char)raw[1] == 0xFF) {
        bom = {"utf-16be", 2};
        return true;
    }
    return false;
}

bool bom_matches(const std::string &forced, const std::string &bom) {
    if (forced == bom)
        return true;
    if (forced == "utf-16")
        return bom == "utf-16le" || bom == "utf-16be";
    if (forced == "utf-32")
        return bom == "utf-32le" || bom == "utf-32be";
    return false;
}

}  // namespace

void source_encoding_set(const std::string &encoding) {
    configured_encoding = encoding;
}

const std::string &source_encoding_get() {
    return configured_encoding;
}

bool decode_source_bytes(const std::string &raw,
                         const std::string &forced_encoding,
                         std::string &text, std::string &error) {
    text.clear();
    error.clear();

    std::string forced_canonical;
    if (!forced_encoding.empty()) {
        forced_canonical = canonical_encoding_name(forced_encoding);
        if (forced_canonical.empty()) {
            error = "unknown source encoding \"" + forced_encoding + "\"";
            return false;
        }
    }

    bom_info bom{};
    const bool has_bom = detect_bom(raw, bom);
    if (has_bom && !forced_canonical.empty() &&
        !bom_matches(forced_canonical, bom.name)) {
        error = std::string("source BOM indicates ") + bom.name +
                ", but --encoding " + forced_encoding + " was given";
        return false;
    }

    const std::string encoding =
        has_bom ? std::string(bom.name)
                : (forced_canonical.empty() ? "utf-8" : forced_canonical);
    const std::string payload =
        has_bom ? raw.substr(bom.length) : raw;

    /* Let the lexer report exact line/column diagnostics for bad UTF-8.
     * Other encodings are converted here because the lexer only accepts
     * UTF-8. */
    if (encoding == "utf-8") {
        text = payload;
        return true;
    }

    if (!decode_to_utf8(payload, encoding, text, error)) {
        error = "cannot decode source as " + encoding + ": " + error;
        return false;
    }
    return true;
}

bool load_source_file(const std::string &path,
                      const std::string &forced_encoding,
                      std::string &text, std::string &error) {
    std::string raw;
    if (!platform_read_file(path, raw, error))
        return false;
    return decode_source_bytes(raw, forced_encoding, text, error);
}

FILE *source_text_file(const std::string &text, std::string &error) {
    error.clear();
    FILE *file = std::tmpfile();
    if (!file) {
        error = std::strerror(errno);
        return nullptr;
    }
#ifdef _WIN32
    if (_setmode(_fileno(file), _O_BINARY) == -1) {
        const int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return nullptr;
    }
#endif
    if (!text.empty() &&
        std::fwrite(text.data(), 1, text.size(), file) != text.size()) {
        const int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return nullptr;
    }
    if (std::fflush(file) != 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        const int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return nullptr;
    }
    return file;
}
