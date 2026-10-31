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
std::string import_base;
fs::path project_root;
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
        if (fs::exists(directory / ".calcroot", exists_ec) && !exists_ec)
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
    std::vector<std::string> parts;
    if (!validate_package_parts(dotted, parts, error))
        return false;

    const std::string &last = parts.back();
    for (const std::string &root : search_roots) {
        fs::path directory(root);
        for (const std::string &part : parts)
            directory /= part;
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
        directory /= part;

    if (try_resolve_package_dir(directory, parts.back(), resolved))
        return true;

    error = "package not found";
    error += "\n  searched: ";
    error += directory.string();
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
            {"reverse", B_list_reverse},
            {"sort", B_list_sort},
            {"append", B_list_append},
            {"map", B_list_map},
            {"filter", B_list_filter},
            {"reduce", B_list_reduce},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("dict"),
        builtin_module({
            {"len", B_len},
            {"get", B_dict_get},
            {"has", B_dict_has},
            {"keys", B_dict_keys},
            {"values", B_dict_values},
            {"items", B_dict_items},
            {"delete", B_dict_delete},
            {"merge", B_dict_merge},
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
            {"contains", B_string_contains},
            {"repeat", B_string_repeat},
            {"trim_start", B_string_trim_start},
            {"trim_end", B_string_trim_end},
            {"format", B_format},
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
    root->payload->items.push_back(std::make_pair(
        newstrval("math"),
        builtin_module({
            {"sqrt", B_sqrt},
            {"exp", B_exp},
            {"log", B_log},
            {"abs", B_abs},
            {"sin", B_sin},
            {"cos", B_cos},
            {"tan", B_tan},
            {"asin", B_asin},
            {"acos", B_acos},
            {"atan", B_atan},
            {"atan2", B_atan2},
            {"pow", B_pow},
            {"floor", B_floor},
            {"ceil", B_ceil},
            {"round", B_round},
            {"trunc", B_trunc},
            {"log2", B_log2},
            {"log10", B_log10},
            {"hypot", B_hypot},
            {"min", B_min},
            {"max", B_max},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("io"),
        builtin_module({
            {"print", B_print},
            {"format", B_format},
            {"input", B_input},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("fs"),
        builtin_module({
            {"read_file", B_read_file},
            {"write_file", B_write_file},
            {"append_file", B_append_file},
            {"exists", B_file_exists},
            {"open", B_file_open},
            {"read", B_file_read},
            {"write", B_file_write},
            {"seek", B_file_seek},
            {"tell", B_file_tell},
            {"eof", B_file_eof},
            {"close", B_file_close},
            {"flush", B_file_flush},
            {"read_bytes", B_read_bytes},
            {"write_bytes", B_write_bytes},
            {"append_bytes", B_append_bytes},
            {"file_read_bytes", B_file_read_bytes},
            {"file_write_bytes", B_file_write_bytes},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("process"),
        builtin_module({
            {"command", B_command},
            {"status", B_command_status},
            {"command_status", B_command_status},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("type"),
        builtin_module({
            {"kind_of", B_kind_of},
            {"name", B_type_name},
            {"module", B_type_module},
            {"source", B_type_source},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("net"),
        builtin_module({
            {"tcp_connect", B_net_tcp_connect},
            {"tcp_listen", B_net_tcp_listen},
            {"accept", B_net_accept},
            {"send", B_net_send},
            {"recv", B_net_recv},
            {"close", B_net_close},
            {"resolve", B_net_resolve},
            {"shutdown", B_net_shutdown},
            {"local_addr", B_net_local_addr},
            {"peer_addr", B_net_peer_addr},
        })));
#ifdef CALC_ENABLE_FFI
    root->payload->items.push_back(std::make_pair(
        newstrval("ffi"),
        builtin_module({
            {"open", B_ffi_open},
            {"close", B_ffi_close},
            {"call", B_ffi_call},
            {"buffer", B_ffi_buffer},
            {"buffer_len", B_ffi_buffer_len},
            {"buffer_bytes", B_ffi_buffer_bytes},
            {"buffer_free", B_ffi_buffer_free},
        })));
#endif
    root->payload->items.push_back(std::make_pair(
        newstrval("task"),
        builtin_module({
            {"sleep", B_task_sleep},
            {"yield", B_task_yield},
            {"wait", B_task_wait},
            {"wait_all", B_task_wait_all},
            {"cancel", B_task_cancel},
            {"done", B_task_done},
        })));
    return (value_t *)root;
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

value_t *package_import_impl(const char *dotted_path, const char *base_dir,
                             std::string &error) {
    import_spec spec;
    if (!parse_import_spec(dotted_path, spec, error))
        return NULL;

    /* Only a bare `std` refers to the built-in standard library. */
    if (spec.kind == IMPORT_ABSOLUTE && spec.dotted == "std") {
        std::string hidden = package_cache_key("builtin:std");
        if (value_t *cached = lookup_global_symbol(hidden.c_str()))
            return dupval(cached);
        value_t *namespace_value = build_std_namespace();
        save_global_symbol(hidden.c_str(), namespace_value);
        return dupval(namespace_value);
    }

    resolved_package resolved;
    if (spec.kind == IMPORT_ABSOLUTE) {
        if (!resolve_package(spec.dotted, resolved, error))
            return NULL;
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
            base = fs::path(lexical_base);
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
                       const std::string &expected_name,
                       const std::string &module_name, value_t *&result,
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
    std::string saved_import_base = import_base;

    import_base = normalize_dir(resolved.directory.string());
    g_interactive = false;
    g_statement_invalid = false;
    ++g_package_loading;
    parser_begin_nested_parse();
    yyin = file;
    reset_scanner_state(file);
    diag_start_source();
    diag_set_source_name(resolved.file.string());
    diag_set_module_name(module_name);
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
    import_base = saved_import_base;
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
    import_base.clear();
    project_root.clear();

    fs::path source(source_path);
    std::error_code ec;
    std::string main_directory;

    if (!source_path.empty() && source_path != "-") {
        if (source.is_relative())
            source = fs::absolute(source, ec);
        if (!ec) {
            main_source_path = source.lexically_normal().string();
            fs::path directory = source.parent_path();
            if (!directory.empty()) {
                main_directory = normalize_dir(directory.string());
                search_roots.push_back(main_directory);
            }
        }
    }

    if (search_roots.empty()) {
        std::error_code cwd_error;
        fs::path cwd = fs::current_path(cwd_error);
        main_directory =
            normalize_dir(cwd_error ? "." : cwd.string());
        search_roots.push_back(main_directory);
    }

    import_base = main_directory;
    project_root = find_project_root(fs::path(import_base));

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
