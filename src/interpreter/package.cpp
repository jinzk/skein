#include "package.h"

#include "std_namespace.h"

#include "binding_registry.h"

#include "ast.h"
#include "diagnostics.h"
#include "eval.h"
#include "eval_context.h"
#include "parser.hpp"
#include "platform.h"
#include "repl.h"
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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

extern FILE *yyin;
extern int yychar;
extern YYSTYPE yylval;
extern int yynerrs;
void parser_begin_nested_parse();

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
    bool has_source;
};

std::string main_source_path;
std::vector<std::string> search_roots;
std::string import_base;
fs::path project_root;
std::vector<package_load_context> load_stack;
std::vector<std::string> loading_paths;

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

fs::path find_project_root(const fs::path &start) {
    std::error_code ec;
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

bool package_file_exists(const fs::path &dir, const std::string &name,
                         fs::path &file);
bool directory_has_package(const fs::path &directory, int depth = 0);

bool try_resolve_package_dir(const fs::path &directory,
                             const std::string &last,
                             resolved_package &resolved) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return false;

    fs::path file;
    bool has_source = package_file_exists(directory, last, file);
    if (!has_source && !directory_has_package(directory))
        return false;

    resolved.directory = directory;
    resolved.file = file;
    resolved.has_source = has_source;
    resolved.key = has_source ? canonical_file(file)
                              : canonical_file(directory);
    return true;
}

std::string package_cache_key(const std::string &canonical_path) {
    return std::string("\x01pkg:") + canonical_path;
}

bool package_file_exists(const fs::path &dir, const std::string &name,
                         fs::path &file) {
    std::error_code ec;
    fs::path direct = dir / path_from_utf8(name + ".skein");
    if (fs::is_regular_file(direct, ec))
        return file = direct, true;
    ec.clear();
    fs::path index = dir / "index.skein";
    if (fs::is_regular_file(index, ec))
        return file = index, true;

    fs::path single;
    bool found = false;
    bool multiple = false;
    fs::directory_iterator it(dir, ec);
    fs::directory_iterator end;
    if (ec)
        return false;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return false;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (it->path().extension() != ".skein")
            continue;
        if (found)
            multiple = true;
        else {
            found = true;
            single = it->path();
        }
    }
    if (found && !multiple)
        return file = single, true;
    return false;
}

bool directory_has_package(const fs::path &directory, int depth) {
    if (depth > 64)
        return false;
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    fs::directory_iterator end;
    if (ec)
        return false;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return false;
        if (!it->is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        std::string child = path_to_utf8(it->path().filename());
        if (!is_identifier(child))
            continue;
        fs::path child_file;
        if (package_file_exists(it->path(), child, child_file))
            return true;
        if (directory_has_package(it->path(), depth + 1))
            return true;
    }
    return false;
}

bool resolve_package(const std::string &dotted, resolved_package &resolved,
                     std::string &error) {
    std::vector<std::string> parts;
    if (!validate_package_parts(dotted, parts, error))
        return false;

    const std::string &last = parts.back();
    for (const std::string &root : search_roots) {
        fs::path directory = path_from_utf8(root);
        for (const std::string &part : parts)
            directory /= path_from_utf8(part);
        if (try_resolve_package_dir(directory, last, resolved))
            return true;
    }

    error = "package not found";
    for (const std::string &root : search_roots) {
        error += "\n  searched: ";
        error += root;
    }
    return false;
}

bool resolve_package_at(const fs::path &base, const std::string &dotted,
                        resolved_package &resolved, std::string &error) {
    std::vector<std::string> parts;
    if (!validate_package_parts(dotted, parts, error))
        return false;

    fs::path directory = base;
    for (const std::string &part : parts)
        directory /= path_from_utf8(part);

    if (try_resolve_package_dir(directory, parts.back(), resolved))
        return true;

    error = "package not found";
    error += "\n  searched: ";
    error += path_to_utf8(directory);
    return false;
}

bool is_loading(const std::string &key) {
    return std::find(loading_paths.begin(), loading_paths.end(), key) !=
           loading_paths.end();
}

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name,
                       const std::string &module_name, value_t *&result,
                       std::string &error);
bool load_child_packages(const std::string &dotted,
                         const fs::path &directory, value_t *namespace_value,
                         std::string &error);
value_t *import_embedded_package(const std::string &dotted,
                                 const char *source, std::string &error);
bool parse_package_source(const std::string &source_text,
                          const std::string &source_name,
                          const std::string &expected_name,
                          const std::string &module_name,
                          const std::string &base_dir, value_t *&result,
                          std::string &error);

value_t *import_resolved(const std::string &dotted,
                         const resolved_package &resolved,
                         std::string &error) {
    if (is_loading(resolved.key)) {
        error = "cyclic package import: " + resolved.key;
        return NULL;
    }

    std::string hidden = package_cache_key(resolved.key);
    if (value_t *cached = lookup_global_symbol(hidden.c_str()))
        return dupval(cached);

    loading_paths.push_back(resolved.key);

    value_t *namespace_value = NULL;
    bool ok = load_package_file(resolved, split_dotted(dotted).back(),
                                dotted, namespace_value, error);
    if (ok) {
        ok = load_child_packages(dotted, resolved.directory,
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
    value_t *root = package_import("std", base_dir, std_error);
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

value_t *package_import_impl(const char *dotted_path, const char *base_dir,
                             std::string &error) {
    import_spec spec;
    if (!parse_import_spec(dotted_path, spec, error))
        return NULL;

    /* `main` is the entry script's package and is never importable. */
    if (is_reserved_main_package(spec.dotted)) {
        error = "package name \"main\" is reserved for the entry script";
        return NULL;
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
                                const std::string &module_name,
                                value_t *&result, std::string &parse_error) {
            return parse_package_source(source, source_name, expected_name,
                                        module_name, import_base, result,
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
        if (!resolve_package(spec.dotted, resolved, resolve_error)) {
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
            base = project_root;
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
        }

        if (base.empty()) {
            error = "no project root for import";
            return NULL;
        }
        if (!resolve_package_at(base, spec.dotted, resolved, error))
            return NULL;
    }

    return import_resolved(spec.dotted, resolved, error);
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

bool load_child_packages(const std::string &dotted, const fs::path &directory,
                         value_t *namespace_value, std::string &error) {
    if (!namespace_value || namespace_value->type != T_dict) {
        error = "package namespace is not a dict";
        return false;
    }

    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> children;
    fs::directory_iterator it(directory, ec);
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
        if (!it->is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        std::string child = path_to_utf8(it->path().filename());
        if (!is_identifier(child))
            continue;
        fs::path child_file;
        if (!package_file_exists(it->path(), child, child_file) &&
            !directory_has_package(it->path()))
            continue;
        children.push_back(std::make_pair(child, it->path()));
    }
    std::sort(children.begin(), children.end());

    struct dict_value_t *dict = (struct dict_value_t *)namespace_value;
    for (const auto &entry : children) {
        const std::string &child = entry.first;
        const fs::path &child_directory = entry.second;
        if (namespace_has_key(namespace_value, child)) {
            error = "package export conflicts with child package: " + child;
            return false;
        }

        std::string child_path = dotted + "." + child;
        resolved_package child_resolved;
        child_resolved.directory = child_directory;
        child_resolved.has_source =
            package_file_exists(child_resolved.directory, child,
                                child_resolved.file);
        child_resolved.key = child_resolved.has_source
                                 ? canonical_file(child_resolved.file)
                                 : canonical_file(child_resolved.directory);
        value_t *child_namespace =
            import_resolved(child_path, child_resolved, error);
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

bool parse_package_source(const std::string &source_text,
                          const std::string &source_name,
                          const std::string &expected_name,
                          const std::string &module_name,
                          const std::string &base_dir, value_t *&result,
                          std::string &error) {
    FILE *file = source_text_file(source_text, error);
    if (!file) {
        error = "cannot open package source " + source_name + ": " + error;
        return false;
    }

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

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name,
                       const std::string &module_name, value_t *&result,
                       std::string &error) {
    if (!resolved.has_source) {
        struct dict_value_t *namespace_value = newdictval();
        namespace_value->payload->is_namespace = true;
        result = (value_t *)namespace_value;
        return true;
    }

    const std::string package_path = path_to_utf8(resolved.file);
    std::string source_text;
    if (!load_source_file(package_path, source_encoding_get(),
                          source_text, error)) {
        error = "cannot load package file " + package_path + ": " + error;
        return false;
    }
    return parse_package_source(source_text, package_path, expected_name,
                                module_name, path_to_utf8(resolved.directory),
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
        source, "<" + dotted + ">", split_dotted(dotted).back(), dotted,
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

void package_configure(const std::string &source_path,
                       const std::vector<std::string> &cli_dirs) {
    search_roots.clear();
    main_source_path.clear();
    import_base.clear();
    project_root.clear();

    fs::path source = path_from_utf8(source_path);
    std::error_code ec;
    std::string main_directory;

    if (!source_path.empty() && source_path != "-") {
        if (source.is_relative())
            source = fs::absolute(source, ec);
        if (!ec) {
            main_source_path = path_to_utf8(source.lexically_normal());
            fs::path directory = source.parent_path();
            if (!directory.empty()) {
                main_directory = normalize_dir(path_to_utf8(directory));
                search_roots.push_back(main_directory);
            }
        }
    }

    if (search_roots.empty()) {
        std::error_code cwd_error;
        fs::path cwd = fs::current_path(cwd_error);
        main_directory =
            normalize_dir(cwd_error ? "." : path_to_utf8(cwd));
        search_roots.push_back(main_directory);
    }

    import_base = main_directory;
    project_root = find_project_root(path_from_utf8(import_base));

    for (const std::string &dir : cli_dirs) {
        if (!dir.empty())
            search_roots.push_back(normalize_dir(dir));
    }

    std::string env;
    if (platform_get_env("SKEIN_PACKAGE_PATH", env) && !env.empty()) {
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
                        std::string &error) {
    return package_import_impl(dotted_path, base_dir, error);
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
