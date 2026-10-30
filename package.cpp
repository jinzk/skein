#include "package.h"

#include "ast.h"
#include "diagnostics.h"
#include "parser.hpp"
#include "platform.h"
#include "repl.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

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
std::vector<package_load_context> load_stack;
std::vector<std::string> loading_paths;

bool is_identifier(const std::string &name) {
    if (name.empty())
        return false;
    unsigned char first = (unsigned char)name[0];
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (size_t i = 1; i < name.size(); ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(std::isalnum(c) || c == '_'))
            return false;
    }
    return true;
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

std::string normalize_dir(const std::string &dir) {
    std::error_code ec;
    fs::path path(dir);
    if (path.is_relative())
        path = fs::absolute(path, ec);
    if (ec)
        return dir;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec)
        return canonical.string();
    return path.lexically_normal().string();
}

std::string canonical_file(const fs::path &file) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(file, ec);
    return ec ? file.lexically_normal().string() : canonical.string();
}

std::string package_cache_key(const std::string &canonical_path) {
    return std::string("\x01pkg:") + canonical_path;
}

bool package_file_exists(const fs::path &dir, const std::string &name,
                         fs::path &file) {
    std::error_code ec;
    fs::path direct = dir / (name + ".calc");
    if (fs::is_regular_file(direct, ec))
        return file = direct, true;
    ec.clear();
    fs::path index = dir / "index.calc";
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
        if (it->path().extension() != ".calc")
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

bool directory_has_package(const fs::path &directory, int depth = 0) {
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
        std::string child = it->path().filename().string();
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
    std::vector<std::string> parts = split_dotted(dotted);
    for (const std::string &part : parts) {
        if (!is_identifier(part)) {
            error = "invalid package path";
            return false;
        }
    }

    const std::string &last = parts.back();
    for (const std::string &root : search_roots) {
        fs::path directory(root);
        for (const std::string &part : parts)
            directory /= part;

        std::error_code ec;
        if (!fs::is_directory(directory, ec))
            continue;

        fs::path file;
        bool has_source = package_file_exists(directory, last, file);
        if (!has_source && !directory_has_package(directory))
            continue;
        resolved.directory = directory;
        resolved.file = file;
        resolved.has_source = has_source;
        resolved.key = has_source ? canonical_file(file)
                                  : canonical_file(directory);
        return true;
    }

    error = "package not found";
    for (const std::string &root : search_roots) {
        error += "\n  searched: ";
        error += root;
    }
    return false;
}

bool is_loading(const std::string &key) {
    return std::find(loading_paths.begin(), loading_paths.end(), key) !=
           loading_paths.end();
}

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name, value_t *&result,
                       std::string &error);
bool load_child_packages(const std::string &dotted,
                         const fs::path &directory, value_t *namespace_value,
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
                                namespace_value, error);
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

value_t *builtin_module(
    std::initializer_list<std::pair<const char *, int>> functions) {
    struct dict_value_t *module = newdictval();
    module->payload->items.reserve(functions.size());
    for (const auto &entry : functions) {
        module->payload->items.push_back(std::make_pair(
            newstrval(entry.first), newbuiltinval(entry.second)));
    }
    return (value_t *)module;
}

value_t *build_std_namespace() {
    struct dict_value_t *root = newdictval();
    root->payload->items.push_back(std::make_pair(
        newstrval("list"),
        builtin_module({
            {"len", B_len},
            {"slice", B_list_slice},
            {"contains", B_list_contains},
            {"index", B_list_index},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("dict"),
        builtin_module({
            {"len", B_len},
            {"get", B_dict_get},
            {"has", B_dict_has},
            {"keys", B_dict_keys},
            {"values", B_dict_values},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("string"),
        builtin_module({
            {"len", B_len},
            {"find", B_find},
            {"substr", B_substr},
            {"split", B_split},
            {"split_ws", B_split_ws},
            {"trim", B_trim},
            {"join", B_string_join},
            {"replace", B_string_replace},
            {"starts_with", B_string_starts_with},
            {"ends_with", B_string_ends_with},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("bytes"),
        builtin_module({
            {"len", B_bytes_len},
            {"get", B_bytes_get},
            {"slice", B_bytes_slice},
            {"concat", B_bytes_concat},
            {"hex", B_bytes_hex},
            {"from_hex", B_bytes_from_hex},
            {"to_int", B_bytes_to_int},
            {"from_int", B_int_to_bytes},
        })));
    return (value_t *)root;
}

value_t *package_import_impl(const char *dotted_path, std::string &error) {
    if (!dotted_path || !*dotted_path) {
        error = "empty package path";
        return NULL;
    }

    if (std::strcmp(dotted_path, "std") == 0) {
        std::string hidden = package_cache_key("builtin:std");
        if (value_t *cached = lookup_global_symbol(hidden.c_str()))
            return dupval(cached);
        value_t *namespace_value = build_std_namespace();
        save_global_symbol(hidden.c_str(), namespace_value);
        return dupval(namespace_value);
    }

    resolved_package resolved;
    if (!resolve_package(dotted_path, resolved, error))
        return NULL;
    return import_resolved(dotted_path, resolved, error);
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
        std::string child = it->path().filename().string();
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

bool load_package_file(const resolved_package &resolved,
                       const std::string &expected_name, value_t *&result,
                       std::string &error) {
    if (!resolved.has_source) {
        result = (value_t *)newdictval();
        return true;
    }

    FILE *file = platform_open_file(resolved.file.string(), "rb");
    if (!file) {
        error = "cannot open package file: " + resolved.file.string();
        return false;
    }

    int saved_char = yychar;
    YYSTYPE saved_lval = yylval;
    int saved_errs = yynerrs;
    scanner_checkpoint *scan_cp = scanner_checkpoint_save();
    diag_source_checkpoint diag_cp;
    diag_save_source(diag_cp);

    bool saved_interactive = g_interactive;
    int saved_runtime_errors = g_runtime_error_count;
    bool saved_invalid = g_statement_invalid;
    int errors_before = diag_error_count();

    g_interactive = false;
    g_statement_invalid = false;
    ++g_package_loading;
    parser_begin_nested_parse();
    yyin = file;
    reset_scanner_state(file);
    diag_start_source();
    diag_set_source_name(resolved.file.string());
    pushscope_isolated();

    package_load_context context;
    context.expected_name = expected_name;
    context.result = NULL;
    load_stack.push_back(context);

    int status = yyparse();
    result = load_stack.back().result;
    load_stack.pop_back();

    popscope();
    std::fclose(file);

    scanner_checkpoint_restore(scan_cp);
    diag_restore_source(diag_cp);
    restore_parser_globals(saved_char, saved_lval, saved_errs);
    g_interactive = saved_interactive;
    --g_package_loading;
    g_runtime_error_count = saved_runtime_errors;
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

} // namespace

void package_configure(const std::string &source_path,
                       const std::vector<std::string> &cli_dirs) {
    search_roots.clear();
    main_source_path.clear();

    fs::path source(source_path);
    std::error_code ec;
    if (!source_path.empty() && source_path != "-") {
        if (source.is_relative())
            source = fs::absolute(source, ec);
        if (!ec) {
            main_source_path = source.lexically_normal().string();
            fs::path directory = source.parent_path();
            if (!directory.empty())
                search_roots.push_back(normalize_dir(directory.string()));
        }
    }

    if (search_roots.empty()) {
        std::error_code cwd_error;
        fs::path cwd = fs::current_path(cwd_error);
        search_roots.push_back(
            normalize_dir(cwd_error ? "." : cwd.string()));
    }

    for (const std::string &dir : cli_dirs) {
        if (!dir.empty())
            search_roots.push_back(normalize_dir(dir));
    }

    const char *env = std::getenv("CALC_PACKAGE_PATH");
    if (env && *env) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::string value(env);
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

value_t *package_import(const char *dotted_path, std::string &error) {
    return package_import_impl(dotted_path, error);
}

value_t *package_build_namespace(scope_t *scope, std::string &error) {
    if (!scope) {
        error = "missing package scope";
        return NULL;
    }

    struct dict_value_t *dict = newdictval();
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

    if (!main_source_path.empty()) {
        fs::path source(main_source_path);
        std::string folder = source.parent_path().filename().string();
        if (!folder.empty() && folder != name) {
            error = "package name \"" + std::string(name) +
                    "\" must match folder \"" + folder + "\"";
            return false;
        }
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
