#include "package.h"

#include "std_namespace.h"

#include "binding_registry.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval.h"
#include "eval_context.h"
#include "parser.hpp"
#include "parser_lock.h"
#include "platform.h"
#include "repl.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "source_encoding.h"
#include "stdio_stream.h"
#include "symbol.h"
#include "unicode_ident.h"
#include "valuetype.h"

#include "std_fsm_source.inc"
#include "std_set_source.inc"
#include "std_deque_source.inc"
#include "std_heap_source.inc"
#include "std_route_source.inc"
#include "std_graph_source.inc"
#include "std_arena_source.inc"
#include "std_path_source.inc"

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

extern FILE *yyin;
extern int indent_count;
extern int yychar;
extern YYSTYPE yylval;
extern int yynerrs;
void parser_begin_nested_parse();
extern thread_local bool g_include_collect;
extern thread_local bool g_include_invalid;
extern thread_local std::vector<struct ast *> g_include_statements;
extern thread_local int g_include_function_depth;

namespace {

namespace fs = std::filesystem;

struct package_load_context {
    std::string expected_name;
    value_t *result;
};

struct resolved_package {
    fs::path directory;
    fs::path file;
    std::string key;
    std::string package;
    std::string module;
};

thread_local std::string main_source_path;
thread_local fs::path main_directory;
thread_local std::vector<std::string> search_roots;
thread_local std::string import_base;
thread_local fs::path project_root;
thread_local bool project_root_resolved = false;
thread_local std::vector<package_load_context> load_stack;
thread_local std::vector<std::string> loading_paths;
thread_local std::vector<std::string> include_stack;
thread_local std::unordered_set<std::string> include_once_paths;

struct embedded_package_source {
    const char *name;
    const char *source;
};

const embedded_package_source kEmbeddedPackages[] = {
    {"std.fsm", kStdFsmSource},
    {"std.set", kStdSetSource},
    {"std.deque", kStdDequeSource},
    {"std.heap", kStdHeapSource},
    {"std.route", kStdRouteSource},
    {"std.graph", kStdGraphSource},
    {"std.arena", kStdArenaSource},
    {"std.path", kStdPathSource},
};

const char *find_embedded_source(const std::string &dotted) {
    for (const auto &entry : kEmbeddedPackages) {
        if (dotted == entry.name)
            return entry.source;
    }
    return NULL;
}

bool is_identifier(const std::string &name) {
    return unicode_ident::is_identifier(name);
}

bool is_reserved_main_package(const std::string &dotted) {
    return dotted == "main" ||
           (dotted.size() > 5 && dotted.compare(0, 5, "main.") == 0);
}

std::vector<std::string> split_dotted(const std::string &path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t dot = path.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        parts.push_back(path.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

fs::path path_from_utf8(const std::string &text) {
    return fs::u8path(text);
}

std::string path_to_utf8(const fs::path &path) {
    return path.u8string();
}

std::string normalize_dir(const std::string &dir) {
    std::error_code ec;
    fs::path path = path_from_utf8(dir);
    if (path.is_relative())
        path = fs::absolute(path, ec);
    if (ec)
        return dir;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec)
        return path_to_utf8(canonical);
    return path_to_utf8(path.lexically_normal());
}

std::string canonical_file(const fs::path &file) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(file, ec);
    return ec ? path_to_utf8(file.lexically_normal())
              : path_to_utf8(canonical);
}

fs::path find_project_root(const fs::path &start) {    std::error_code ec;
    fs::path directory = start;
    if (directory.is_relative())
        directory = fs::absolute(directory, ec);
    if (!ec) {
        fs::path canonical = fs::weakly_canonical(directory, ec);
        if (!ec)
            directory = canonical;
    }
    if (ec)
        return start;

    for (;;) {
        std::error_code exists_ec;
        if (fs::exists(directory / ".skeinroot", exists_ec) && !exists_ec)
            return directory;
        fs::path parent = directory.parent_path();
        if (parent.empty() || parent == directory)
            break;
        directory = parent;
    }
    return start;
}

fs::path ensure_project_root() {
    if (!project_root_resolved) {
        fs::path start = path_from_utf8(import_base);
        if (start.empty())
            start = path_from_utf8(thread_cwd());
        project_root = find_project_root(start);
        project_root_resolved = true;
    }
    return project_root;
}

bool validate_package_parts(const std::string &dotted,
                            std::vector<std::string> &parts,
                            std::string &error) {
    parts = split_dotted(dotted);
    for (const std::string &part : parts) {
        if (!is_identifier(part)) {
            error = "invalid package path";
            return false;
        }
    }
    return true;
}

bool resolve_package_file(const fs::path &parent, const std::string &last,
                          resolved_package &resolved) {
    std::error_code ec;
    if (!fs::is_directory(parent, ec))
        return false;
    fs::path file = parent / path_from_utf8(last + ".skein");
    if (!fs::is_regular_file(file, ec))
        return false;
    resolved.directory = parent;
    resolved.file = file;
    resolved.key = canonical_file(file);
    return true;
}

std::string package_cache_key(const std::string &canonical_path) {
    return std::string("\x01pkg:") + canonical_path;
}

struct seeded_import_entry {
    std::string path;
    std::string base;
    value_t *namespace_value = nullptr;
};

std::vector<seeded_import_entry> &seeded_import_registry() {
    static thread_local std::vector<seeded_import_entry> entries;
    return entries;
}

value_t *take_seeded_import(const char *dotted_path, const char *base_dir) {
    if (!dotted_path)
        return NULL;
    std::string base = base_dir ? base_dir : "";
    for (const seeded_import_entry &entry : seeded_import_registry()) {
        if (entry.path == dotted_path && entry.base == base)
            return dupval(entry.namespace_value);
    }
    return NULL;
}

bool resolve_package(const std::string &dotted, resolved_package &resolved,
                     std::string &error) {
    std::vector<std::string> parts;
    if (!validate_package_parts(dotted, parts, error))
        return false;

    const std::string &last = parts.back();
    for (const std::string &root : search_roots) {
        fs::path parent = path_from_utf8(root);
        for (size_t i = 0; i + 1 < parts.size(); ++i)
            parent /= path_from_utf8(parts[i]);
        if (resolve_package_file(parent, last, resolved))
            return true;
    }

    error = "package not found";
    for (const std::string &root : search_roots) {
        fs::path parent = path_from_utf8(root);
        for (size_t i = 0; i + 1 < parts.size(); ++i)
            parent /= path_from_utf8(parts[i]);
        error += "\n  searched: ";
        error += path_to_utf8(parent / path_from_utf8(last + ".skein"));
    }
    return false;
}

bool resolve_package_at(const fs::path &base, const std::string &dotted,
                        resolved_package &resolved, std::string &error) {
    std::vector<std::string> parts;
    if (!validate_package_parts(dotted, parts, error))
        return false;

    fs::path parent = base;
    for (size_t i = 0; i + 1 < parts.size(); ++i)
        parent /= path_from_utf8(parts[i]);

    if (resolve_package_file(parent, parts.back(), resolved))
        return true;

    error = "package not found";
    error += "\n  searched: ";
    error += path_to_utf8(parent / path_from_utf8(parts.back() + ".skein"));
    return false;
}

/* --------------------------------------------------------------------------
 * Module v2 manifest
 * ------------------------------------------------------------------------ */

struct module_export_spec {
    bool is_package = false;
    std::string path;
    std::string alias;
};

struct module_manifest {
    std::string name;
    fs::path root;
    std::string source_path;
    std::vector<module_export_spec> exports;
};

struct module_manifest_cache_entry {
    std::string key;
    bool loaded = false;
    bool ok = false;
    std::string error;
    module_manifest manifest;
};

std::vector<std::shared_ptr<module_manifest_cache_entry>> &
module_manifest_cache() {
    static thread_local std::vector<
        std::shared_ptr<module_manifest_cache_entry>> entries;
    return entries;
}

enum class manifest_token_kind {
    Ident,
    Dot,
    LBrace,
    RBrace,
    Semi,
    End
};

struct manifest_token {
    manifest_token_kind kind = manifest_token_kind::End;
    std::string text;
    int line = 1;
    int column = 1;
};

bool manifest_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool manifest_ident_continue(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

std::vector<manifest_token> lex_module_manifest(const std::string &text,
                                                std::string &error) {
    std::vector<manifest_token> tokens;
    size_t i = 0;
    int line = 1;
    int column = 1;
    auto advance = [&](size_t count) {
        for (size_t n = 0; n < count; ++n) {
            if (i >= text.size())
                return;
            if (text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
            ++i;
        }
    };
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(1);
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n')
                advance(1);
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            advance(2);
            while (i + 1 < text.size() &&
                   !(text[i] == '*' && text[i + 1] == '/'))
                advance(1);
            if (i + 1 >= text.size()) {
                error = "unterminated block comment";
                return tokens;
            }
            advance(2);
            continue;
        }
        manifest_token token;
        token.line = line;
        token.column = column;
        if (manifest_ident_start(c)) {
            size_t start = i;
            while (i < text.size() &&
                   manifest_ident_continue((unsigned char)text[i]))
                advance(1);
            token.kind = manifest_token_kind::Ident;
            token.text.assign(text, start, i - start);
            tokens.push_back(std::move(token));
            continue;
        }
        switch (c) {
            case '.': token.kind = manifest_token_kind::Dot; break;
            case '{': token.kind = manifest_token_kind::LBrace; break;
            case '}': token.kind = manifest_token_kind::RBrace; break;
            case ';': token.kind = manifest_token_kind::Semi; break;
            default: {
                error = "unexpected character in module manifest";
                return tokens;
            }
        }
        token.text.assign(1, (char)c);
        tokens.push_back(std::move(token));
        advance(1);
    }
    manifest_token end;
    end.kind = manifest_token_kind::End;
    end.line = line;
    end.column = column;
    tokens.push_back(std::move(end));
    return tokens;
}

bool parse_module_manifest(const fs::path &root, const std::string &text,
                           module_manifest &manifest,
                           std::string &error) {
    std::vector<manifest_token> tokens = lex_module_manifest(text, error);
    if (!error.empty())
        return false;
    size_t pos = 0;
    auto current = [&]() -> const manifest_token & { return tokens[pos]; };
    auto consume = [&]() { ++pos; };
    auto fail = [&](const std::string &message) {
        const manifest_token &token = current();
        error = message + " at " + std::to_string(token.line) + ":" +
                std::to_string(token.column);
        return false;
    };
    auto expect_ident = [&](const char *name) {
        if (current().kind != manifest_token_kind::Ident ||
            current().text != name)
            return fail(std::string("expected '") + name + "'");
        consume();
        return true;
    };
    auto expect = [&](manifest_token_kind kind, const char *name) {
        if (current().kind != kind)
            return fail(std::string("expected '") + name + "'");
        consume();
        return true;
    };

    manifest.root = root;
    manifest.source_path = path_to_utf8(root / "module.skein");
    if (!expect_ident("module"))
        return false;
    if (current().kind != manifest_token_kind::Ident)
        return fail("expected module name");
    manifest.name = current().text;
    consume();
    if (!expect(manifest_token_kind::LBrace, "{"))
        return false;

    std::unordered_set<std::string> aliases;
    while (current().kind != manifest_token_kind::RBrace &&
           current().kind != manifest_token_kind::End) {
        if (!expect_ident("export"))
            return false;
        module_export_spec spec;
        if (current().kind == manifest_token_kind::Ident &&
            current().text == "package") {
            spec.is_package = true;
            consume();
        }
        if (current().kind != manifest_token_kind::Ident)
            return fail("expected export path");
        std::vector<std::string> parts;
        parts.push_back(current().text);
        consume();
        while (current().kind == manifest_token_kind::Dot) {
            consume();
            if (current().kind != manifest_token_kind::Ident)
                return fail("expected name after '.'");
            parts.push_back(current().text);
            consume();
        }
        if (spec.is_package) {
            spec.alias = parts.back();
        } else {
            if (parts.size() < 2)
                return fail("symbol export must be <package>.<symbol>");
            spec.alias = parts.back();
        }
        spec.path = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            spec.path += '.';
            spec.path += parts[i];
        }
        if (current().kind == manifest_token_kind::Ident &&
            current().text == "as") {
            consume();
            if (current().kind != manifest_token_kind::Ident)
                return fail("expected alias after 'as'");
            spec.alias = current().text;
            consume();
        }
        if (!aliases.insert(spec.alias).second) {
            error = "duplicate module export alias '" + spec.alias + "'";
            return false;
        }
        if (!expect(manifest_token_kind::Semi, ";"))
            return false;
        manifest.exports.push_back(std::move(spec));
    }
    if (!expect(manifest_token_kind::RBrace, "}"))
        return false;
    if (current().kind != manifest_token_kind::End)
        return fail("unexpected token after module manifest");
    return true;
}

bool find_nested_module_manifest(const fs::path &root, fs::path &nested) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    if (ec)
        return false;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return false;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (it->path().filename() == "module.skein" &&
            it->path().parent_path() != root) {
            nested = it->path();
            return true;
        }
    }
    return false;
}

const module_manifest *load_module_manifest(const fs::path &root,
                                            std::string &error) {
    const std::string key = normalize_dir(path_to_utf8(root));
    for (const std::shared_ptr<module_manifest_cache_entry> &entry :
         module_manifest_cache()) {
        if (entry->key == key) {
            if (!entry->ok)
                error = entry->error;
            return entry->ok ? &entry->manifest : nullptr;
        }
    }
    std::shared_ptr<module_manifest_cache_entry> entry =
        std::make_shared<module_manifest_cache_entry>();
    entry->key = key;
    entry->loaded = true;
    const fs::path path = root / "module.skein";
    std::string source;
    if (!load_source_file(path_to_utf8(path), source_encoding_get(), source,
                          entry->error)) {
        entry->error = "cannot load module manifest " + path_to_utf8(path) +
                       ": " + entry->error;
        module_manifest_cache().push_back(std::move(entry));
        error = module_manifest_cache().back()->error;
        return nullptr;
    }
    entry->ok = parse_module_manifest(root, source, entry->manifest,
                                      entry->error);
    if (entry->ok) {
        const std::string directory_name =
            path_to_utf8(root.filename());
        if (entry->manifest.name != directory_name) {
            entry->ok = false;
            entry->error = "module name '" + entry->manifest.name +
                           "' does not match directory '" + directory_name +
                           "'";
        }
    }
    if (entry->ok) {
        std::error_code ec;
        const std::string &module_name = entry->manifest.name;
        if (module_name != "module" &&
            fs::is_regular_file(root /
                                    path_from_utf8(module_name + ".skein"),
                                ec)) {
            entry->ok = false;
            entry->error = "module root cannot contain package file '" +
                           module_name + ".skein'; move it into a package "
                           "directory";
        } else if (fs::is_regular_file(root / "index.skein", ec)) {
            entry->ok = false;
            entry->error = "module root cannot contain index.skein; move it "
                           "into a package directory";
        }
    }
    if (entry->ok) {
        fs::path nested;
        if (find_nested_module_manifest(root, nested)) {
            entry->ok = false;
            entry->error = "nested module.skein is not supported: " +
                           path_to_utf8(nested);
        }
    }
    if (!entry->ok)
        entry->error = entry->manifest.source_path + ": " + entry->error;
    const module_manifest *result = entry->ok ? &entry->manifest : nullptr;
    if (!entry->ok)
        error = entry->error;
    module_manifest_cache().push_back(std::move(entry));
    return result;
}

bool find_module_root_at(const fs::path &search_root,
                         const std::string &dotted, fs::path &root,
                         std::string &name, std::string &error) {
    std::vector<std::string> parts = split_dotted(dotted);
    if (parts.empty()) {
        error = "empty package path";
        return false;
    }
    if (!is_identifier(parts[0])) {
        error = "invalid module path";
        return false;
    }
    fs::path candidate = search_root / path_from_utf8(parts[0]);
    std::error_code ec;
    if (fs::is_regular_file(candidate / "module.skein", ec)) {
        root = candidate;
        name = parts[0];
        return true;
    }
    return false;
}

bool find_module_root(const std::string &dotted, fs::path &root,
                      std::string &name, std::string &error) {
    for (const std::string &search_root : search_roots) {
        if (find_module_root_at(path_from_utf8(search_root), dotted, root,
                                name, error))
            return true;
    }
    return false;
}

bool is_loading(const std::string &key) {
    return std::find(loading_paths.begin(), loading_paths.end(), key) !=
           loading_paths.end();
}

bool path_relative_dotted(const fs::path &root, const fs::path &file,
                          std::string &dotted) {
    std::error_code ec;
    fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec)
        canonical_root = root.lexically_normal();
    ec.clear();
    fs::path canonical_path = fs::weakly_canonical(file, ec);
    if (ec)
        canonical_path = file.lexically_normal();
    fs::path relative = canonical_path.lexically_relative(canonical_root);
    if (relative.empty())
        return false;
    auto first = relative.begin();
    if (first != relative.end() && *first == "..")
        return false;
    relative.replace_extension();
    dotted.clear();
    for (const fs::path &part : relative) {
        const std::string text = path_to_utf8(part);
        if (text.empty() || text == "." || text == ".." ||
            !is_identifier(text))
            return false;
        if (!dotted.empty())
            dotted += ".";
        dotted += text;
    }
    return !dotted.empty();
}

bool find_enclosing_module(const fs::path &file, fs::path &root,
                           std::string &module_name, bool &found,
                           std::string &error) {
    found = false;
    std::error_code ec;
    fs::path directory = file.parent_path();
    if (directory.empty())
        return true;
    for (;;) {
        if (fs::is_regular_file(directory / "module.skein", ec)) {
            const module_manifest *manifest =
                load_module_manifest(directory, error);
            if (!manifest)
                return false;
            root = directory;
            module_name = manifest->name;
            found = true;
            return true;
        }
        fs::path parent = directory.parent_path();
        if (parent.empty() || parent == directory)
            break;
        directory = parent;
    }
    return true;
}

bool path_within_root(const fs::path &root, const fs::path &path) {
    std::error_code ec;
    fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec)
        canonical_root = root.lexically_normal();
    ec.clear();
    fs::path canonical_path = fs::weakly_canonical(path, ec);
    if (ec)
        canonical_path = path.lexically_normal();
    fs::path relative = canonical_path.lexically_relative(canonical_root);
    if (relative.empty())
        return false;
    auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

bool relative_import_allowed(const fs::path &base, std::string &error) {
    const std::string &source = diag_current_source_name();
    if (source.empty())
        return true;
    fs::path module_root;
    std::string module_name;
    bool found = false;
    if (!find_enclosing_module(path_from_utf8(source), module_root,
                               module_name, found, error))
        return false;
    if (!found)
        return true;
    if (!path_within_root(module_root, base)) {
        error = "relative import escapes module root";
        return false;
    }
    return true;
}

bool finalize_package_identity(resolved_package &resolved,
                               std::string &error) {
    fs::path module_root;
    std::string module_name;
    bool found = false;
    if (!find_enclosing_module(resolved.file, module_root, module_name, found,
                               error))
        return false;

    std::string relative;
    if (found) {
        if (!path_relative_dotted(module_root, resolved.file, relative)) {
            error = "cannot derive package path from " +
                    path_to_utf8(resolved.file);
            return false;
        }
        resolved.package = module_name + "." + relative;
        resolved.module = module_name;
        return true;
    }

    const fs::path root = ensure_project_root();
    if (!path_relative_dotted(root, resolved.file, relative)) {
        error = "ordinary packages must be under project_root: " +
                path_to_utf8(resolved.file);
        return false;
    }
    resolved.package = relative;
    resolved.module.clear();
    return true;
}

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name, value_t *&result,
                       std::string &error);
bool load_child_packages(const std::string &dotted,
                         const fs::path &package_file, value_t *namespace_value,
                         std::string &error);
value_t *import_embedded_package(const std::string &dotted,
                                 const char *source, std::string &error);
bool parse_package_source(const std::string &source_text,
                          const std::string &source_name,
                          const std::string &expected_name,
                          const std::string &package_name,
                          const std::string &module_name,
                          const std::string &base_dir, value_t *&result,
                          std::string &error);

value_t *import_resolved(const std::string &dotted,
                         resolved_package &resolved,
                         std::string *package_out,
                         std::string &error) {
    if (!finalize_package_identity(resolved, error))
        return NULL;
    if (package_out)
        *package_out = resolved.package;
    if (is_loading(resolved.key)) {
        error = "cyclic package import: " + resolved.key;
        return NULL;
    }

    std::string hidden = package_cache_key(resolved.key);
    if (value_t *cached = lookup_global_symbol(hidden.c_str()))
        return dupval(cached);

    loading_paths.push_back(resolved.key);

    value_t *namespace_value = NULL;
    bool ok = load_package_file(
        resolved, split_dotted(dotted).back(), namespace_value, error);
    if (ok) {
        ok = load_child_packages(dotted, resolved.file,
                                 namespace_value, error);
    }

    loading_paths.pop_back();

    if (!ok) {
        freeval(namespace_value);
        return NULL;
    }

    save_global_symbol(hidden.c_str(), namespace_value);
    return dupval(namespace_value);
}

/* Load exactly one package file, without implicitly merging child packages. */
value_t *import_package_exact(const std::string &dotted,
                              resolved_package &resolved,
                              std::string *package_out,
                              std::string &error) {
    if (!finalize_package_identity(resolved, error))
        return NULL;
    if (package_out)
        *package_out = resolved.package;
    if (is_loading(resolved.key)) {
        error = "cyclic package import: " + resolved.key;
        return NULL;
    }
    std::string hidden = package_cache_key(resolved.key);
    if (value_t *cached = lookup_global_symbol(hidden.c_str()))
        return dupval(cached);

    loading_paths.push_back(resolved.key);
    value_t *namespace_value = NULL;
    bool ok = load_package_file(
        resolved, split_dotted(dotted).back(), namespace_value, error);
    loading_paths.pop_back();
    if (!ok) {
        freeval(namespace_value);
        return NULL;
    }
    save_global_symbol(hidden.c_str(), namespace_value);
    return dupval(namespace_value);
}

enum import_kind {
    IMPORT_ABSOLUTE,
    IMPORT_RELATIVE,
    IMPORT_PROJECT
};

struct import_spec {
    import_kind kind;
    int up_levels;
    std::string dotted;
};

bool parse_import_spec(const char *raw, import_spec &spec, std::string &error) {
    std::string path = raw ? raw : "";
    if (path.empty()) {
        error = "empty package path";
        return false;
    }

    size_t pos = 0;
    spec.kind = IMPORT_ABSOLUTE;
    spec.up_levels = 0;

    if (path[pos] == '@') {
        spec.kind = IMPORT_PROJECT;
        ++pos;
    } else if (path[pos] == '.') {
        spec.kind = IMPORT_RELATIVE;
        size_t start = pos;
        while (pos < path.size() && path[pos] == '.')
            ++pos;
        spec.up_levels = (int)(pos - start - 1);
    }

    if (pos >= path.size()) {
        error = "empty package path";
        return false;
    }
    spec.dotted = path.substr(pos);

    std::vector<std::string> parts;
    if (!validate_package_parts(spec.dotted, parts, error))
        return false;
    return true;
}

bool is_std_submodule(const std::string &dotted) {
    return dotted.size() > 4 && dotted.compare(0, 4, "std.") == 0;
}

value_t *dict_lookup_string(const dict_value_t *dict, const std::string &key) {
    if (!dict)
        return NULL;
    for (const auto &entry : dict->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((const string_value_t *)entry.first)->value,
                        key.c_str()) == 0)
            return entry.second;
    }
    return NULL;
}

value_t *import_std_submodule(const std::string &dotted,
                              const char *base_dir, std::string &error) {
    std::string std_error;
    value_t *root = package_import("std", base_dir, nullptr, std_error);
    if (!root) {
        error = std_error;
        return NULL;
    }

    value_t *current = root;
    size_t start = 4; /* skip "std." */
    while (start <= dotted.size()) {
        size_t end = dotted.find('.', start);
        if (end == std::string::npos)
            end = dotted.size();
        std::string part = dotted.substr(start, end - start);
        value_t *next = NULL;
        if (current->type == T_dict)
            next = dict_lookup_string((const dict_value_t *)current, part);
        if (!next) {
            error = "package not found: " + dotted;
            freeval(root);
            return NULL;
        }
        current = next;
        if (end == dotted.size())
            break;
        start = end + 1;
    }

    value_t *result = dupval(current);
    freeval(root);
    return result;
}

bool resolve_module_target(const fs::path &root,
                           const std::string &package_path,
                           resolved_package &resolved,
                           std::string &error) {
    return resolve_package_at(root, package_path, resolved, error);
}

value_t *import_module_root(const std::string &module_name,
                            const fs::path &root, std::string &error) {
    const std::string key = canonical_file(root);
    if (is_loading(key)) {
        error = "cyclic module import: " + module_name;
        return NULL;
    }
    const std::string hidden = package_cache_key(key);
    if (value_t *cached = lookup_global_symbol(hidden.c_str()))
        return dupval(cached);

    const module_manifest *manifest = load_module_manifest(root, error);
    if (!manifest)
        return NULL;
    if (manifest->name != module_name) {
        error = "module name '" + manifest->name +
                "' does not match import path '" + module_name + "'";
        return NULL;
    }

    loading_paths.push_back(key);
    struct dict_value_t *namespace_value = newdictval();
    namespace_value->payload->is_namespace = true;
    for (const module_export_spec &spec : manifest->exports) {
        std::string package_path = spec.path;
        std::string symbol;
        if (!spec.is_package) {
            size_t dot = package_path.rfind('.');
            if (dot == std::string::npos) {
                error = "invalid module export path: " + spec.path;
                freeval((value_t *)namespace_value);
                loading_paths.pop_back();
                return NULL;
            }
            symbol = package_path.substr(dot + 1);
            package_path.resize(dot);
        }

        resolved_package target;
        std::string target_error;
        if (!resolve_module_target(root, package_path, target, target_error)) {
            error = "module " + module_name + " export '" + spec.alias +
                    "': " + target_error;
            freeval((value_t *)namespace_value);
            loading_paths.pop_back();
            return NULL;
        }
        value_t *package_namespace = import_package_exact(
            module_name + "." + package_path, target, nullptr, target_error);
        if (!package_namespace) {
            error = "module " + module_name + " export '" + spec.alias +
                    "': " + target_error;
            freeval((value_t *)namespace_value);
            loading_paths.pop_back();
            return NULL;
        }

        if (spec.is_package) {
            if (dict_lookup_string(namespace_value, spec.alias)) {
                error = "duplicate module export alias '" + spec.alias + "'";
                freeval(package_namespace);
                freeval((value_t *)namespace_value);
                loading_paths.pop_back();
                return NULL;
            }
            namespace_value->payload->items.push_back(std::make_pair(
                newstrval(spec.alias.c_str()), package_namespace));
            continue;
        }

        value_t *exported = dict_lookup_string(
            (const dict_value_t *)package_namespace, symbol);
        if (!exported) {
            error = "module " + module_name + " export '" + spec.alias +
                    "': package '" + package_path + "' does not export '" +
                    symbol + "'";
            freeval(package_namespace);
            freeval((value_t *)namespace_value);
            loading_paths.pop_back();
            return NULL;
        }
        if (dict_lookup_string(namespace_value, spec.alias)) {
            error = "duplicate module export alias '" + spec.alias + "'";
            freeval(package_namespace);
            freeval((value_t *)namespace_value);
            loading_paths.pop_back();
            return NULL;
        }
        namespace_value->payload->items.push_back(std::make_pair(
            newstrval(spec.alias.c_str()), dupval(exported)));
        freeval(package_namespace);
    }
    loading_paths.pop_back();

    save_global_symbol(hidden.c_str(), (value_t *)namespace_value);
    return dupval((value_t *)namespace_value);
}

bool resolve_module_public_package(const std::string &module_name,
                                   const std::string &alias,
                                   const fs::path &root,
                                   resolved_package &target,
                                   std::string &package_path,
                                   std::string &error) {
    const module_manifest *manifest = load_module_manifest(root, error);
    if (!manifest)
        return false;
    for (const module_export_spec &spec : manifest->exports) {
        if (!spec.is_package || spec.alias != alias)
            continue;
        std::string target_error;
        if (!resolve_module_target(root, spec.path, target, target_error)) {
            error = "module " + module_name + " package '" + alias +
                    "': " + target_error;
            return false;
        }
        package_path = spec.path;
        return true;
    }
    error = "module " + module_name + " does not export package '" + alias +
            "'";
    return false;
}

value_t *import_module_public_package(const std::string &module_name,
                                      const std::string &alias,
                                      const fs::path &root,
                                      std::string *package_out,
                                      std::string &error) {
    resolved_package target;
    std::string package_path;
    if (!resolve_module_public_package(module_name, alias, root, target,
                                       package_path, error))
        return NULL;
    std::string target_error;
    value_t *result = import_package_exact(
        module_name + "." + package_path, target, package_out, target_error);
    if (!result)
        error = "module " + module_name + " package '" + alias +
                "': " + target_error;
    return result;
}

value_t *package_import_impl(const char *dotted_path, const char *base_dir,
                             std::string *package_out, std::string &error) {
    if (value_t *seeded = take_seeded_import(dotted_path, base_dir))
        return seeded;
    import_spec spec;
    if (!parse_import_spec(dotted_path, spec, error))
        return NULL;

    /* `main` is the entry script's package and is never importable. */
    if (is_reserved_main_package(spec.dotted)) {
        error = "package name \"main\" is reserved for the entry script";
        return NULL;
    }

    /*
     * A top-level directory with module.skein is a module boundary.  Imports
     * from outside the module may only use the public namespace or a package
     * alias declared in the manifest; internal packages keep using normal
     * physical imports.
     */
    if ((spec.kind == IMPORT_ABSOLUTE || spec.kind == IMPORT_PROJECT) &&
        spec.dotted != "std") {
        fs::path module_root;
        std::string module_name;
        std::string module_error;
        bool module_found = false;
        if (spec.kind == IMPORT_PROJECT) {
            module_found = find_module_root_at(
                ensure_project_root(), spec.dotted, module_root, module_name,
                module_error);
        } else {
            module_found = find_module_root(spec.dotted, module_root,
                                             module_name, module_error);
        }
        if (module_found) {
            std::vector<std::string> parts = split_dotted(spec.dotted);
            if (parts.size() == 1)
                return import_module_root(module_name, module_root, error);
            if (parts.size() == 2)
                return import_module_public_package(
                    module_name, parts[1], module_root, package_out, error);
            error = "module member is not exported: " + spec.dotted;
            return NULL;
        }
    }

    /* Only a bare `std` refers to the built-in standard library. */
    if (spec.kind == IMPORT_ABSOLUTE && spec.dotted == "std") {
        std::string hidden = package_cache_key("builtin:std");
        if (value_t *cached = lookup_global_symbol(hidden.c_str()))
            return dupval(cached);
        value_t *namespace_value = skein::stdlib::build_namespace();
        save_global_symbol(hidden.c_str(), namespace_value);

        auto parse_adapter = [](const std::string &source,
                                const std::string &source_name,
                                const std::string &expected_name,
                                const std::string &package_name,
                                value_t *&result, std::string &parse_error) {
            return parse_package_source(source, source_name, expected_name,
                                        package_name, "", import_base, result,
                                        parse_error);
        };
        std::string adapter_error;
        if (!skein::stdlib::install_adapters(namespace_value, parse_adapter,
                                             adapter_error)) {
            error = "cannot load std container adapter: " + adapter_error;
            save_global_symbol(hidden.c_str(), newnull());
            return NULL;
        }
        return dupval(namespace_value);
    }

    if (spec.kind == IMPORT_ABSOLUTE) {
        if (const char *source = find_embedded_source(spec.dotted)) {
            if (skein::module_registered(spec.dotted)) {
                error = "ambiguous import: " + spec.dotted +
                        " is both a package and a native module";
                return NULL;
            }
            return import_embedded_package(spec.dotted, source, error);
        }
    }

    resolved_package resolved;
    if (spec.kind == IMPORT_ABSOLUTE) {
        std::string resolve_error;
        if (!resolve_package_at(main_directory, spec.dotted, resolved,
                                resolve_error)) {
            if (skein::module_registered(spec.dotted))
                return skein::import_module(spec.dotted, error);
            if (is_std_submodule(spec.dotted))
                return import_std_submodule(spec.dotted, base_dir, error);
            error = resolve_error;
            return NULL;
        }
        if (skein::module_registered(spec.dotted)) {
            error = "ambiguous import: " + spec.dotted +
                    " is both a package and a native module";
            return NULL;
        }
    } else {
        fs::path base;
        if (spec.kind == IMPORT_PROJECT) {
            base = ensure_project_root();
        } else {
            std::string lexical_base =
                (base_dir && *base_dir) ? base_dir : import_base;
            if (lexical_base.empty()) {
                error = "no base directory for relative import";
                return NULL;
            }
            base = path_from_utf8(lexical_base);
            for (int i = 0; i < spec.up_levels; ++i) {
                fs::path parent = base.parent_path();
                if (parent.empty() || parent == base) {
                    error = "relative import escapes filesystem root";
                    return NULL;
                }
                base = parent;
            }
            if (!relative_import_allowed(base, error))
                return NULL;
        }

        if (base.empty()) {
            error = "no project root for import";
            return NULL;
        }
        if (!resolve_package_at(base, spec.dotted, resolved, error))
            return NULL;
    }

    return import_resolved(spec.dotted, resolved, package_out, error);
}

bool namespace_has_key(value_t *namespace_value, const std::string &key) {
    if (!namespace_value || namespace_value->type != T_dict)
        return false;
    struct dict_value_t *dict = (struct dict_value_t *)namespace_value;
    for (auto &entry : dict->payload->items) {
        if (entry.first && entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key.c_str()) == 0)
            return true;
    }
    return false;
}

bool load_child_packages(const std::string &dotted,
                         const fs::path &package_file,
                         value_t *namespace_value, std::string &error) {
    if (!namespace_value || namespace_value->type != T_dict) {
        error = "package namespace is not a dict";
        return false;
    }

    fs::path child_root = package_file.parent_path() / package_file.stem();
    std::error_code ec;
    if (!fs::is_directory(child_root, ec))
        return true;

    std::vector<std::string> children;
    fs::directory_iterator it(child_root, ec);
    fs::directory_iterator end;
    if (ec) {
        error = "cannot scan package directory: " + ec.message();
        return false;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot scan package directory: " + ec.message();
            return false;
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (it->path().extension() != ".skein")
            continue;
        std::string child = path_to_utf8(it->path().stem());
        if (!is_identifier(child))
            continue;
        children.push_back(child);
    }
    std::sort(children.begin(), children.end());

    struct dict_value_t *dict = (struct dict_value_t *)namespace_value;
    for (const std::string &child : children) {
        if (namespace_has_key(namespace_value, child)) {
            error = "package export conflicts with child package: " + child;
            return false;
        }

        resolved_package child_resolved;
        if (!resolve_package_file(child_root, child, child_resolved))
            continue;

        std::string child_path = dotted + "." + child;
        value_t *child_namespace =
            import_resolved(child_path, child_resolved, nullptr, error);
        if (!child_namespace)
            return false;

        dict->payload->items.push_back(
            std::make_pair(newstrval(child.c_str()), child_namespace));
    }
    return true;
}

void restore_parser_globals(int saved_char, const YYSTYPE &saved_lval,
                            int saved_errs) {
    yychar = saved_char;
    yylval = saved_lval;
    yynerrs = saved_errs;
}

bool evaluate_include_source_impl(const char *path, std::string &error) {
    if (!path || !*path) {
        error = "empty include path";
        return false;
    }

    fs::path requested = path_from_utf8(path);
    if (requested.is_relative()) {
        const std::string &source = diag_current_source_name();
        fs::path base = source.empty()
                            ? path_from_utf8(thread_cwd())
                            : path_from_utf8(source).parent_path();
        requested = base / requested;
    }
    std::error_code ec;
    if (!fs::is_regular_file(requested, ec)) {
        error = "include file not found: " + std::string(path);
        return false;
    }
    const std::string canonical = canonical_file(requested);
    const std::string key = diag_current_package_name() + '\x1f' +
                            diag_current_module_name() + '\x1f' + canonical;
    if (include_once_paths.count(key) != 0)
        return true;
    if (std::find(include_stack.begin(), include_stack.end(), canonical) !=
        include_stack.end()) {
        error = "cyclic include: " + canonical;
        return false;
    }
    if (include_stack.size() >= 64) {
        error = "include depth limit exceeded";
        return false;
    }

    std::string source_text;
    if (!load_source_file(canonical, source_encoding_get(), source_text,
                          error)) {
        error = "cannot load include file " + canonical + ": " + error;
        return false;
    }
    FILE *file = source_text_file(source_text, error);
    if (!file) {
        error = "cannot open include file " + canonical + ": " + error;
        return false;
    }

    std::lock_guard<std::recursive_mutex> parse_lock(parser_mutex());
    parser_lock_depth_guard depth_guard;

    int saved_char = yychar;
    YYSTYPE saved_lval = yylval;
    int saved_errs = yynerrs;
    FILE *saved_yyin = yyin;
    int saved_indent = indent_count;
    scanner_checkpoint *scan_cp = scanner_checkpoint_save();
    diag_source_checkpoint diag_cp;
    diag_save_source(diag_cp);

    bool saved_interactive = g_interactive;
    bool saved_invalid = g_statement_invalid;
    int saved_runtime_errors = active_eval_context().runtime_error_count;
    int errors_before = diag_error_count();
    std::string saved_import_base = import_base;
    int saved_function_depth = g_include_function_depth;

    include_stack.push_back(canonical);
    g_include_collect = true;
    g_include_invalid = false;
    g_include_statements.clear();
    g_include_function_depth = 0;
    g_interactive = false;
    g_statement_invalid = false;
    parser_begin_nested_parse();
    yyin = file;
    indent_count = 0;
    reset_scanner_state(file);
    diag_start_source();
    diag_set_source_name(canonical);
    import_base = normalize_dir(
        path_to_utf8(path_from_utf8(canonical).parent_path()));

    int status = yyparse();
    g_include_collect = false;
    std::fclose(file);

    bool ok = status == 0 && !g_include_invalid &&
              diag_error_count() == errors_before;
    if (ok) {
        for (struct ast *statement : g_include_statements) {
            struct value_t *value = eval(statement);
            freeval(value);
            if (diag_error_count() != errors_before) {
                ok = false;
                break;
            }
        }
    }
    for (struct ast *statement : g_include_statements)
        treefree(statement);
    g_include_statements.clear();
    if (ok)
        include_once_paths.insert(key);
    else if (error.empty())
        error = "include failed";

    import_base = saved_import_base;
    indent_count = saved_indent;
    yyin = saved_yyin;
    g_include_function_depth = saved_function_depth;
    scanner_checkpoint_restore(scan_cp);
    diag_restore_source(diag_cp);
    restore_parser_globals(saved_char, saved_lval, saved_errs);
    g_interactive = saved_interactive;
    g_statement_invalid = saved_invalid;
    active_eval_context().runtime_error_count = saved_runtime_errors;

    include_stack.pop_back();
    return ok;
}

bool parse_package_source(const std::string &source_text,
                          const std::string &source_name,
                          const std::string &expected_name,
                          const std::string &package_name,
                          const std::string &module_name,
                          const std::string &base_dir, value_t *&result,
                          std::string &error) {
    FILE *file = source_text_file(source_text, error);
    if (!file) {
        error = "cannot open package source " + source_name + ": " + error;
        return false;
    }

    std::lock_guard<std::recursive_timed_mutex> parse_gate(
        package_parse_gate());
    std::lock_guard<std::recursive_mutex> parse_lock(parser_mutex());
    parser_lock_depth_guard depth_guard;

    int saved_char = yychar;
    YYSTYPE saved_lval = yylval;
    int saved_errs = yynerrs;
    scanner_checkpoint *scan_cp = scanner_checkpoint_save();
    diag_source_checkpoint diag_cp;
    diag_save_source(diag_cp);

    bool saved_interactive = g_interactive;
    int saved_runtime_errors = active_eval_context().runtime_error_count;
    bool saved_invalid = g_statement_invalid;
    int errors_before = diag_error_count();
    std::string saved_import_base = import_base;

    if (!base_dir.empty())
        import_base = normalize_dir(base_dir);
    g_interactive = false;
    g_statement_invalid = false;
    ++g_package_loading;
    parser_begin_nested_parse();
    yyin = file;
    reset_scanner_state(file);
    diag_start_source();
    diag_set_source_name(source_name);
    diag_set_package_name(package_name);
    diag_set_module_name(module_name);
    eval_push_isolated_scope();

    package_load_context context;
    context.expected_name = expected_name;
    context.result = NULL;
    load_stack.push_back(context);

    int status = yyparse();
    result = load_stack.back().result;
    load_stack.pop_back();

    eval_popscope();
    std::fclose(file);

    scanner_checkpoint_restore(scan_cp);
    diag_restore_source(diag_cp);
    restore_parser_globals(saved_char, saved_lval, saved_errs);
    import_base = saved_import_base;
    g_interactive = saved_interactive;
    --g_package_loading;
    active_eval_context().runtime_error_count = saved_runtime_errors;
    g_statement_invalid = saved_invalid;

    if (status != 0 || diag_error_count() != errors_before || !result) {
        freeval(result);
        result = NULL;
        if (error.empty())
            error = "package failed to load";
        return false;
    }
    return true;
}

bool parse_worker_source_impl(const std::string &path, std::string &error) {
    std::unique_lock<std::recursive_timed_mutex> parse_gate(
        package_parse_gate(), std::defer_lock);
    if (!parse_gate.try_lock_for(std::chrono::milliseconds(10000))) {
        error = "worker script parser is busy";
        return false;
    }
    std::string source;
    if (!load_source_file(path, source_encoding_get(), source, error)) {
        error = "cannot load worker script " + path + ": " + error;
        return false;
    }
    FILE *file = source_text_file(source, error);
    if (!file) {
        error = "cannot open worker script " + path + ": " + error;
        return false;
    }

    std::lock_guard<std::recursive_mutex> parse_lock(parser_mutex());
    parser_lock_depth_guard depth_guard;

    int saved_char = yychar;
    YYSTYPE saved_lval = yylval;
    int saved_errs = yynerrs;
    FILE *saved_yyin = yyin;
    int saved_indent = indent_count;
    scanner_checkpoint *scan_cp = scanner_checkpoint_save();
    diag_source_checkpoint diag_cp;
    diag_save_source(diag_cp);

    bool saved_interactive = g_interactive;
    int saved_runtime_errors = active_eval_context().runtime_error_count;
    bool saved_invalid = g_statement_invalid;
    int errors_before = diag_error_count();
    std::string saved_import_base = import_base;

    g_interactive = false;
    g_statement_invalid = false;
    ++g_package_loading;
    parser_begin_nested_parse();
    yyin = file;
    reset_scanner_state(file);
    diag_start_source();
    diag_set_source_name(path);
    diag_set_package_name("worker");

    int status = yyparse();

    scanner_checkpoint_restore(scan_cp);
    yyin = saved_yyin;
    indent_count = saved_indent;
    diag_restore_source(diag_cp);
    restore_parser_globals(saved_char, saved_lval, saved_errs);
    import_base = saved_import_base;
    g_interactive = saved_interactive;
    --g_package_loading;
    active_eval_context().runtime_error_count = saved_runtime_errors;
    g_statement_invalid = saved_invalid;
    std::fclose(file);

    if (status != 0 || diag_error_count() != errors_before) {
        if (error.empty())
            error = "worker script failed to parse";
        return false;
    }
    return true;
}

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name, value_t *&result,
                       std::string &error) {
    const std::string package_path = path_to_utf8(resolved.file);
    std::string source_text;
    if (!load_source_file(package_path, source_encoding_get(),
                          source_text, error)) {
        error = "cannot load package file " + package_path + ": " + error;
        return false;
    }
    return parse_package_source(source_text, package_path, expected_name,
                                resolved.package, resolved.module,
                                path_to_utf8(resolved.directory),
                                result, error);
}

value_t *import_embedded_package(const std::string &dotted,
                                 const char *source, std::string &error) {
    if (!source || !*source) {
        error = "embedded package has no source: " + dotted;
        return NULL;
    }
    std::string key = "builtin:" + dotted;
    if (is_loading(key)) {
        error = "cyclic package import: " + key;
        return NULL;
    }
    std::string hidden = package_cache_key(key);
    if (value_t *cached = lookup_global_symbol(hidden.c_str()))
        return dupval(cached);

    loading_paths.push_back(key);
    value_t *namespace_value = NULL;
    bool ok = parse_package_source(
        source, "<" + dotted + ">", split_dotted(dotted).back(), dotted, "",
        import_base, namespace_value, error);
    loading_paths.pop_back();

    if (!ok) {
        freeval(namespace_value);
        return NULL;
    }
    save_global_symbol(hidden.c_str(), namespace_value);
    return dupval(namespace_value);
}

} // namespace

bool evaluate_include_source(const char *path, std::string &error) {
    return evaluate_include_source_impl(path, error);
}

bool parse_worker_source(const std::string &path, std::string &error) {
    return parse_worker_source_impl(path, error);
}

void package_configure(const std::string &source_path,
                       const std::vector<std::string> &cli_dirs) {
    search_roots.clear();
    main_source_path.clear();
    import_base.clear();
    project_root.clear();
    project_root_resolved = false;
    main_directory.clear();

    fs::path source = path_from_utf8(source_path);
    std::error_code ec;

    if (!source_path.empty() && source_path != "-") {
        if (source.is_relative())
            source = fs::absolute(source, ec);
        if (!ec) {
            main_source_path = path_to_utf8(source.lexically_normal());
            fs::path directory = source.parent_path();
            if (!directory.empty()) {
                main_directory =
                    path_from_utf8(normalize_dir(path_to_utf8(directory)));
                search_roots.push_back(path_to_utf8(main_directory));
            }
        }
    }

    if (main_directory.empty()) {
        const std::string cwd = thread_cwd();
        main_directory =
            path_from_utf8(normalize_dir(cwd.empty() ? "." : cwd));
        search_roots.push_back(path_to_utf8(main_directory));
    }

    import_base = path_to_utf8(main_directory);

    for (const std::string &dir : cli_dirs) {
        if (!dir.empty())
            search_roots.push_back(normalize_dir(dir));
    }

    std::string env;
    if (thread_env_get("SKEIN_PACKAGE_PATH", env) && !env.empty()) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::string value = env;
        size_t start = 0;
        for (;;) {
            size_t end = value.find(separator, start);
            std::string dir = end == std::string::npos
                                  ? value.substr(start)
                                  : value.substr(start, end - start);
            if (!dir.empty())
                search_roots.push_back(normalize_dir(dir));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
}

bool package_is_loading() {
    return !load_stack.empty();
}

const std::string &package_current_import_base() {
    return import_base;
}

value_t *package_import(const char *dotted_path, const char *base_dir,
                        std::string *package_out, std::string &error) {
    return package_import_impl(dotted_path, base_dir, package_out, error);
}

bool package_import_cache_key(const char *dotted_path, const char *base_dir,
                              std::string &key, std::string &error) {
    import_spec spec;
    if (!parse_import_spec(dotted_path, spec, error))
        return false;
    if (is_reserved_main_package(spec.dotted)) {
        error = "package name \"main\" is reserved for the entry script";
        return false;
    }
    if ((spec.kind == IMPORT_ABSOLUTE || spec.kind == IMPORT_PROJECT) &&
        spec.dotted != "std") {
        fs::path module_root;
        std::string module_name;
        std::string module_error;
        bool module_found = false;
        if (spec.kind == IMPORT_PROJECT) {
            module_found = find_module_root_at(
                ensure_project_root(), spec.dotted, module_root, module_name,
                module_error);
        } else {
            module_found = find_module_root(spec.dotted, module_root,
                                             module_name, module_error);
        }
        if (module_found) {
            std::vector<std::string> parts = split_dotted(spec.dotted);
            if (parts.size() == 1) {
                key = canonical_file(module_root);
                return true;
            }
            if (parts.size() == 2) {
                resolved_package target;
                std::string package_path;
                if (!resolve_module_public_package(
                        module_name, parts[1], module_root, target,
                        package_path, error))
                    return false;
                key = target.key;
                return true;
            }
            error = "module member is not exported: " + spec.dotted;
            return false;
        }
    }
    if (spec.kind == IMPORT_ABSOLUTE && spec.dotted == "std") {
        key = "builtin:std";
        return true;
    }
    if (spec.kind == IMPORT_ABSOLUTE) {
        if (find_embedded_source(spec.dotted)) {
            if (skein::module_registered(spec.dotted)) {
                error = "ambiguous import: " + spec.dotted +
                        " is both a package and a native module";
                return false;
            }
            key = "builtin:" + spec.dotted;
            return true;
        }
    }

    resolved_package resolved;
    if (spec.kind == IMPORT_ABSOLUTE) {
        std::string resolve_error;
        if (!resolve_package_at(main_directory, spec.dotted, resolved,
                                resolve_error)) {
            if (skein::module_registered(spec.dotted)) {
                error = "native module imports cannot be transferred";
                return false;
            }
            if (is_std_submodule(spec.dotted)) {
                error = "std submodule imports cannot be transferred";
                return false;
            }
            error = resolve_error;
            return false;
        }
        if (skein::module_registered(spec.dotted)) {
            error = "ambiguous import: " + spec.dotted +
                    " is both a package and a native module";
            return false;
        }
    } else {
        fs::path base;
        if (spec.kind == IMPORT_PROJECT) {
            base = ensure_project_root();
        } else {
            std::string lexical_base =
                (base_dir && *base_dir) ? base_dir : import_base;
            if (lexical_base.empty()) {
                error = "no base directory for relative import";
                return false;
            }
            base = path_from_utf8(lexical_base);
            for (int i = 0; i < spec.up_levels; ++i) {
                fs::path parent = base.parent_path();
                if (parent.empty() || parent == base) {
                    error = "relative import escapes filesystem root";
                    return false;
                }
                base = parent;
            }
            if (!relative_import_allowed(base, error))
                return false;
        }
        if (base.empty()) {
            error = "no project root for import";
            return false;
        }
        if (!resolve_package_at(base, spec.dotted, resolved, error))
            return false;
    }
    key = resolved.key;
    return true;
}

void package_seed_import(const std::string &key, value_t *namespace_value) {
    std::string hidden = package_cache_key(key);
    save_global_symbol(hidden.c_str(), namespace_value);
}

void package_seed_import_path(const char *dotted_path, const char *base_dir,
                              value_t *namespace_value) {
    if (!dotted_path) {
        freeval(namespace_value);
        return;
    }
    std::string base = base_dir ? base_dir : "";
    for (seeded_import_entry &entry : seeded_import_registry()) {
        if (entry.path == dotted_path && entry.base == base) {
            freeval(entry.namespace_value);
            entry.namespace_value = namespace_value;
            return;
        }
    }
    seeded_import_entry entry;
    entry.path = dotted_path;
    entry.base = std::move(base);
    entry.namespace_value = namespace_value;
    seeded_import_registry().push_back(std::move(entry));
}

void package_clear_import_seeds() {
    for (seeded_import_entry &entry : seeded_import_registry())
        freeval(entry.namespace_value);
    seeded_import_registry().clear();
}

value_t *package_build_namespace(scope_t *scope, std::string &error) {
    if (!scope) {
        error = "missing package scope";
        return NULL;
    }

    struct dict_value_t *dict = newdictval();
    dict->payload->is_namespace = true;
    for (struct symbol *sym : scope->syms.small) {
        if (!sym || !sym->exported || !sym->val)
            continue;
        dict->payload->items.push_back(
            std::make_pair(newstrval(sym->name), dupval(sym->val)));
    }
    return (value_t *)dict;
}

bool package_publish(const char *name, value_t *namespace_value,
                     std::string &error) {
    if (!name || !namespace_value) {
        error = "invalid package namespace";
        return false;
    }

    if (!load_stack.empty()) {
        package_load_context &context = load_stack.back();
        if (std::strcmp(name, "main") == 0) {
            error = "package name \"main\" is reserved for the entry script";
            return false;
        }
        if (context.expected_name != name) {
            error = "package name \"" + std::string(name) +
                    "\" must match folder \"" + context.expected_name + "\"";
            return false;
        }
        if (context.result) {
            error = "multiple package declarations for " + std::string(name);
            return false;
        }
        context.result = namespace_value;
        return true;
    }

    if (!main_source_path.empty() && std::strcmp(name, "main") != 0) {
        error = "entry package must be \"main\"; got \"" +
                std::string(name) + "\"";
        return false;
    }

    if (isrepeated(name)) {
        error = "package name already exists: " + std::string(name);
        return false;
    }

    struct symbol *sym = newsym(name);
    sym->val = namespace_value;
    savesym(sym);
    return true;
}
