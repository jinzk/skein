#include "workspace_index.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "tools/tool_analysis.h"

namespace skein {
namespace lsp {

namespace {

namespace fs = std::filesystem;

struct manifest_export {
    std::string path;  // e.g. "internal.helpers.normalize"
    std::string alias; // e.g. "normalize"
};

struct scanned_symbol {
    std::string name;
    std::string keyword;
    int line = 0;
    int character = 0;
};

bool starts_with(const std::string &text, const char *prefix) {
    const std::string p(prefix);
    return text.size() >= p.size() && text.compare(0, p.size(), p) == 0;
}

std::string percent_encode_path(const std::string &path) {
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == ':') {
            out.push_back((char)c);
        } else {
            static const char hex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::string path_to_uri(const fs::path &path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
        absolute = path;
    std::string text = absolute.generic_string();
    if (!starts_with(text, "/"))
        text = "/" + text;
    return "file://" + percent_encode_path(text);
}

bool read_file(const fs::path &path, std::string &text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text = buffer.str();
    return true;
}

/** Minimal tokenizer for module.skein manifests (mirrors package.cpp rules). */
std::vector<std::string> tokenize_manifest(const std::string &text) {
    std::vector<std::string> tokens;
    std::string current;
    auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0) {
            std::size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
            continue;
        }
        const unsigned char c = (unsigned char)text[i];
        if (std::isspace(c)) {
            flush();
            ++i;
            continue;
        }
        if (c == '{' || c == '}' || c == ';' || c == '.') {
            flush();
            tokens.emplace_back(1, (char)c);
            ++i;
            continue;
        }
        if (std::isalnum(c) || c == '_' || c >= 0x80) {
            current.push_back((char)c);
            ++i;
            continue;
        }
        flush();
        ++i;
    }
    flush();
    return tokens;
}

bool parse_module_manifest(const std::string &text, std::string &name,
                           std::vector<manifest_export> &exports) {
    const std::vector<std::string> tokens = tokenize_manifest(text);
    std::size_t pos = 0;
    auto ident = [&]() -> bool {
        if (pos >= tokens.size() || tokens[pos].empty() ||
            !std::isalpha((unsigned char)tokens[pos][0]))
            return false;
        name = tokens[pos++];
        return true;
    };
    if (pos >= tokens.size() || tokens[pos] != "module")
        return false;
    ++pos;
    if (!ident())
        return false;
    const std::string module_name = name;
    if (pos >= tokens.size() || tokens[pos] != "{")
        return false;
    ++pos;
    while (pos < tokens.size() && tokens[pos] != "}") {
        if (tokens[pos] != "export")
            return false;
        ++pos;
        bool is_package = false;
        if (pos < tokens.size() && tokens[pos] == "package") {
            is_package = true;
            ++pos;
        }
        std::string path;
        std::string last;
        if (!ident())
            return false;
        last = name;
        path = last;
        while (pos < tokens.size() && tokens[pos] == ".") {
            ++pos;
            if (!ident())
                return false;
            last = name;
            path += "." + last;
        }
        (void)is_package;
        std::string alias = last;
        if (pos < tokens.size() && tokens[pos] == "as") {
            ++pos;
            if (!ident())
                return false;
            alias = name;
        }
        if (pos >= tokens.size() || tokens[pos] != ";")
            return false;
        ++pos;
        exports.push_back({path, alias});
    }
    if (pos >= tokens.size() || tokens[pos] != "}")
        return false;
    name = module_name;
    return !exports.empty();
}

bool is_ident_byte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

/** Lightweight declaration scanner; enough for definition positions. */
std::vector<scanned_symbol> scan_symbols(const std::string &text) {
    static const std::vector<std::string> keywords = {
        "func", "struct", "enum", "trait", "impl", "let", "const",
        "package", "module", "extern",
    };
    std::vector<scanned_symbol> symbols;
    std::size_t pos = 0;
    int line = 0;
    while (pos <= text.size()) {
        std::size_t end = pos;
        while (end < text.size() && text[end] != '\r' && text[end] != '\n')
            ++end;
        const std::string raw = text.substr(pos, end - pos);

        std::size_t cursor = 0;
        while (cursor < raw.size() && std::isspace((unsigned char)raw[cursor]))
            ++cursor;
        if (raw.compare(cursor, 7, "export ") == 0) {
            cursor += 7;
            while (cursor < raw.size() && std::isspace((unsigned char)raw[cursor]))
                ++cursor;
        }

        for (const std::string &keyword : keywords) {
            if (raw.compare(cursor, keyword.size(), keyword) != 0)
                continue;
            std::size_t after = cursor + keyword.size();
            if (after < raw.size() && is_ident_byte((unsigned char)raw[after]))
                continue;
            while (after < raw.size() && std::isspace((unsigned char)raw[after]))
                ++after;
            const std::size_t name_start = after;
            while (after < raw.size() && is_ident_byte((unsigned char)raw[after]))
                ++after;
            if (after > name_start) {
                scanned_symbol symbol;
                symbol.name = raw.substr(name_start, after - name_start);
                symbol.keyword = keyword;
                symbol.line = line;
                symbol.character = (int)name_start;
                symbols.push_back(std::move(symbol));
            }
            break;
        }
        pos = end + 1;
        ++line;
    }
    return symbols;
}

/** Resolve an export path to its defining file and symbol name. */
bool resolve_export_file(const fs::path &module_root,
                         const std::string &export_path, fs::path &file,
                         std::string &symbol_name) {
    const std::size_t dot = export_path.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return false;
    const std::string symbol = export_path.substr(dot + 1);
    const std::string package = export_path.substr(0, dot);
    std::string relative;
    for (char c : package) {
        relative.push_back(c == '.' ? '/' : c);
    }
    file = module_root / (relative + ".skein");
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return false;
    symbol_name = symbol;
    return true;
}

void index_module(const fs::path &module_root,
                  const std::vector<std::string> &package_dirs,
                  std::vector<index_entry> &entries) {
    std::string manifest_text;
    if (!read_file(module_root / "module.skein", manifest_text))
        return;
    std::string module_name;
    std::vector<manifest_export> exports;
    if (!parse_module_manifest(manifest_text, module_name, exports))
        return;

    for (const manifest_export &spec : exports) {
        fs::path file;
        std::string symbol_name;
        if (!resolve_export_file(module_root, spec.path, file, symbol_name))
            continue;

        std::string text;
        if (!read_file(file, text))
            continue;

        // Collect type/callable metadata via the check-only front end.  The
        // source name is the absolute file path so relative imports resolve.
        analysis_options options;
        options.source_name = file.generic_string();
        options.encoding = "utf-8";
        options.package_dirs = package_dirs;
        options.typecheck_mode = skein::typecheck::Mode::Off;
        options.keep_going = true;
        options.suppress_text = true;
        analysis_result result = analyze_source_text(text, options);

        const analysis_symbol *type_info = nullptr;
        for (const analysis_symbol &symbol : result.symbols) {
            if (symbol.name == symbol_name) {
                type_info = &symbol;
                break;
            }
        }
        if (!type_info)
            continue;

        const std::vector<scanned_symbol> scanned = scan_symbols(text);
        const scanned_symbol *position = nullptr;
        for (const scanned_symbol &symbol : scanned) {
            if (symbol.name != symbol_name)
                continue;
            /* Prefer a real declaration over a same-named package statement. */
            if (symbol.keyword == "package" || symbol.keyword == "module") {
                if (!position)
                    position = &symbol;
                continue;
            }
            position = &symbol;
            break;
        }

        index_entry entry;
        entry.qualified_name = module_name + "." + spec.alias;
        entry.uri = path_to_uri(file);
        if (position) {
            entry.line = position->line;
            entry.character = position->character;
        }
        entry.type = type_info->type;
        entry.callable = type_info->callable;
        entries.push_back(std::move(entry));
    }
}

} // namespace

void workspace_index::rebuild(const std::vector<std::string> &roots) {
    entries_.clear();
    for (const std::string &root_text : roots) {
        std::error_code ec;
        fs::path root(root_text);
        if (!fs::is_directory(root, ec))
            continue;
        for (fs::recursive_directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            if (it->path().filename() != "module.skein")
                continue;
            index_module(it->path().parent_path(), roots, entries_);
        }
    }
}

std::vector<index_entry>
workspace_index::complete(const std::string &qualified_prefix) const {
    std::vector<index_entry> matches;
    const std::string prefix = qualified_prefix + ".";
    for (const index_entry &entry : entries_) {
        if (starts_with(entry.qualified_name, prefix.c_str()))
            matches.push_back(entry);
    }
    return matches;
}

std::vector<index_entry>
workspace_index::lookup(const std::string &qualified_name) const {
    std::vector<index_entry> matches;
    for (const index_entry &entry : entries_) {
        if (entry.qualified_name == qualified_name)
            matches.push_back(entry);
    }
    return matches;
}

} // namespace lsp
} // namespace skein
