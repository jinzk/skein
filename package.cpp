#include "package.h"

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
    module->payload->is_namespace = true;
    module->payload->items.reserve(functions.size());
    for (const auto &entry : functions) {
        module->payload->items.push_back(std::make_pair(
            newstrval(entry.first), newbuiltinval(entry.second)));
    }
    return (value_t *)module;
}

value_t *build_std_namespace() {
    struct dict_value_t *root = newdictval();
    root->payload->is_namespace = true;
    root->payload->items.push_back(std::make_pair(
        newstrval("list"),
        builtin_module({
            {"len", B_len},
            {"slice", B_list_slice},
            {"contains", B_list_contains},
            {"index", B_list_index},
            {"reverse", B_list_reverse},
            {"sort", B_list_sort},
            {"sort_by", B_list_sort_by},
            {"sort_desc", B_list_sort_desc},
            {"is_sorted", B_list_is_sorted},
            {"append", B_list_append},
            {"map", B_list_map},
            {"filter", B_list_filter},
            {"reduce", B_list_reduce},
            {"unique", B_list_unique},
            {"unique_by", B_list_unique_by},
            {"group_by", B_list_group_by},
            {"chunk", B_list_chunk},
            {"window", B_list_window},
            {"zip", B_list_zip},
            {"top_k", B_list_top_k},
            {"binary_search", B_list_binary_search},
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
        newstrval("json"),
        builtin_module({
            {"parse", B_json_parse},
            {"try_parse", B_json_try_parse},
            {"stringify", B_json_stringify},
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
            {"to_string", B_bytes_to_string},
            {"from_string", B_bytes_from_string},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("encoding"),
        builtin_module({
            {"decode", B_encoding_decode},
            {"encode", B_encoding_encode},
            {"supports", B_encoding_supports},
            {"base64_encode", B_encoding_base64_encode},
            {"base64_decode", B_encoding_base64_decode},
            {"url_encode", B_encoding_url_encode},
            {"url_decode", B_encoding_url_decode},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("unicode"),
        builtin_module({
            {"normalize", B_unicode_normalize},
            {"is_normalized", B_unicode_is_normalized},
            {"version", B_unicode_version},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("regex"),
        builtin_module({
            {"match", B_regex_match},
            {"search", B_regex_search},
            {"find_all", B_regex_find_all},
            {"replace", B_regex_replace},
            {"split", B_regex_split},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("time"),
        builtin_module({
            {"now", B_time_now},
            {"now_ms", B_time_now_ms},
            {"monotonic_ms", B_time_monotonic_ms},
            {"utc_offset", B_time_utc_offset},
            {"format", B_time_format},
            {"parse", B_time_parse},
            {"sleep", B_time_sleep},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("random"),
        builtin_module({
            {"seed", B_random_seed},
            {"int", B_random_int},
            {"float", B_random_float},
            {"bytes", B_random_bytes},
            {"choice", B_random_choice},
            {"shuffle", B_random_shuffle},
            {"uuid", B_random_uuid},
        })));
    value_t *math_module = builtin_module({
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
        {"sign", B_math_sign},
        {"clamp", B_math_clamp},
        {"fmod", B_math_fmod},
        {"mod", B_math_mod},
        {"is_nan", B_math_is_nan},
        {"is_inf", B_math_is_inf},
        {"is_finite", B_math_is_finite},
        {"cbrt", B_math_cbrt},
        {"gcd", B_math_gcd},
        {"lcm", B_math_lcm},
        {"degrees", B_math_degrees},
        {"radians", B_math_radians},
    });
    dict_value_t *math_dict = (dict_value_t *)math_module;
    math_dict->payload->items.push_back(std::make_pair(
        newstrval("pi"), newfloatval(3.14159265358979323846)));
    math_dict->payload->items.push_back(std::make_pair(
        newstrval("e"), newfloatval(2.71828182845904523536)));
    math_dict->payload->items.push_back(std::make_pair(
        newstrval("tau"), newfloatval(6.28318530717958647692)));
    root->payload->items.push_back(std::make_pair(
        newstrval("math"), math_module));
    value_t *io_module = (value_t *)builtin_module({
        {"print", B_print},
        {"format", B_format},
        {"input", B_input},
        {"read", B_io_read},
        {"write", B_io_write},
        {"write_all", B_io_write_all},
        {"flush", B_io_flush},
        {"close", B_io_close},
        {"eof", B_io_eof},
        {"readable", B_io_readable},
        {"writable", B_io_writable},
        {"seekable", B_io_seekable},
        {"seek", B_io_seek},
        {"tell", B_io_tell},
        {"read_exact", B_io_read_exact},
        {"read_all", B_io_read_all},
        {"read_line", B_io_read_line},
        {"copy", B_io_copy},
        {"buffered", B_io_buffered},
        {"read_async", B_io_read_async},
        {"write_async", B_io_write_async},
    });
    dict_value_t *io_dict = (dict_value_t *)io_module;
    io_dict->payload->items.push_back(std::make_pair(
        newstrval("stdin"),
        new_stdio_stream_value(stdin, "stdin", true, false)));
    io_dict->payload->items.push_back(std::make_pair(
        newstrval("stdout"),
        new_stdio_stream_value(stdout, "stdout", false, true)));
    io_dict->payload->items.push_back(std::make_pair(
        newstrval("stderr"),
        new_stdio_stream_value(stderr, "stderr", false, true)));
    root->payload->items.push_back(std::make_pair(
        newstrval("io"), io_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("fs"),
        builtin_module({
            {"read_file", B_read_file},
            {"write_file", B_write_file},
            {"append_file", B_append_file},
            {"read_text", B_read_text},
            {"write_text", B_write_text},
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
            {"listdir", B_fs_listdir},
            {"mkdir", B_fs_mkdir},
            {"remove", B_fs_remove},
            {"rename", B_fs_rename},
            {"stat", B_fs_stat},
            {"cwd", B_fs_cwd},
            {"chdir", B_fs_chdir},
            {"path_join", B_fs_path_join},
            {"path_basename", B_fs_path_basename},
            {"path_dirname", B_fs_path_dirname},
            {"path_extname", B_fs_path_extname},
            {"path_abs", B_fs_path_abs},
            {"path_normalize", B_fs_path_normalize},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("env"),
        builtin_module({
            {"get", B_env_get},
            {"has", B_env_has},
            {"set", B_env_set},
            {"unset", B_env_unset},
            {"all", B_env_all},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("crypto"),
        builtin_module({
            {"sha256", B_crypto_sha256},
            {"hmac_sha256", B_crypto_hmac_sha256},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("compress"),
        builtin_module({
            {"gzip", B_compress_gzip},
            {"gunzip", B_compress_gunzip},
            {"zlib", B_compress_zlib},
            {"unzlib", B_compress_unzlib},
            {"deflate", B_compress_deflate},
            {"inflate", B_compress_inflate},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("process"),
        builtin_module({
            {"command", B_command},
            {"status", B_command_status},
            {"command_status", B_command_status},
            {"run", B_process_run},
            {"spawn", B_process_spawn},
            {"wait", B_process_wait},
            {"kill", B_process_kill},
            {"close", B_process_close},
            {"pid", B_process_pid},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("worker"),
        builtin_module({
            {"pool", B_worker_pool},
            {"call", B_worker_call},
            {"map", B_worker_map},
            {"submit", B_worker_submit},
            {"wait", B_worker_wait},
            {"shutdown", B_worker_shutdown},
            {"stats", B_worker_stats},
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
        newstrval("enum"),
        builtin_module({
            {"decode", B_enum_decode},
            {"tag", B_enum_tag},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("struct"),
        builtin_module({
            {"decode", B_struct_decode},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("lru"),
        builtin_module({
            {"new", B_lru_new},
            {"get", B_lru_get},
            {"peek", B_lru_peek},
            {"put", B_lru_put},
            {"delete", B_lru_delete},
            {"has", B_lru_has},
            {"keys", B_lru_keys},
            {"values", B_lru_values},
            {"len", B_lru_len},
            {"capacity", B_lru_capacity},
            {"clear", B_lru_clear},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("deque"),
        builtin_module({
            {"new", B_deque_new},
            {"from_list", B_deque_from_list},
            {"size", B_deque_size},
            {"push_front", B_deque_push_front},
            {"push_back", B_deque_push_back},
            {"peek_front", B_deque_peek_front},
            {"peek_back", B_deque_peek_back},
            {"pop_front", B_deque_pop_front},
            {"pop_back", B_deque_pop_back},
            {"to_list", B_deque_to_list},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("heap"),
        builtin_module({
            {"new", B_heap_new},
            {"from_list", B_heap_from_list},
            {"size", B_heap_size},
            {"peek", B_heap_peek},
            {"push", B_heap_push},
            {"pop", B_heap_pop},
            {"to_list", B_heap_to_list},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("set"),
        builtin_module({
            {"new", B_set_new},
            {"from_list", B_set_from_list},
            {"add", B_set_add},
            {"remove", B_set_remove},
            {"has", B_set_has},
            {"size", B_set_size},
            {"values", B_set_values},
            {"union", B_set_union},
            {"intersect", B_set_intersect},
            {"difference", B_set_difference},
            {"is_subset", B_set_is_subset},
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
            {"tcp_connect_async", B_net_tcp_connect_async},
            {"tcp_listen_async", B_net_tcp_listen_async},
            {"accept_async", B_net_accept_async},
            {"send_async", B_net_send_async},
            {"recv_async", B_net_recv_async},
            {"resolve_async", B_net_resolve_async},
            {"shutdown_async", B_net_shutdown_async},
        })));
#ifdef SKEIN_ENABLE_FFI
    root->payload->items.push_back(std::make_pair(
        newstrval("ffi"),
        builtin_module({
            {"open", B_ffi_open},
            {"close", B_ffi_close},
            {"call", B_ffi_call},
            {"call_async", B_ffi_call_async},
            {"call_address", B_ffi_call_address},
            {"call_variadic", B_ffi_call_variadic},
            {"buffer", B_ffi_buffer},
            {"buffer_len", B_ffi_buffer_len},
            {"buffer_bytes", B_ffi_buffer_bytes},
            {"buffer_free", B_ffi_buffer_free},
            {"struct", B_ffi_struct},
            {"pointer", B_ffi_pointer},
            {"array", B_ffi_array},
            {"signature", B_ffi_signature},
            {"new", B_ffi_new},
            {"alloc", B_ffi_alloc},
            {"ptr", B_ffi_ptr},
            {"deref", B_ffi_deref},
            {"offset", B_ffi_offset},
            {"cast", B_ffi_cast},
            {"get", B_ffi_get},
            {"set", B_ffi_set},
            {"to_dict", B_ffi_to_dict},
            {"sizeof", B_ffi_sizeof},
            {"alignof", B_ffi_alignof},
            {"offsetof", B_ffi_offsetof},
            {"address_of", B_ffi_address_of},
            {"callback", B_ffi_callback},
            {"manifest", B_ffi_manifest},
            {"constant", B_ffi_constant},
            {"symbol", B_ffi_symbol},
            {"string", B_ffi_string},
            {"variable", B_ffi_variable},
            {"set_variable", B_ffi_set_variable},
            {"retain", B_ffi_retain},
            {"errno", B_ffi_errno},
            {"set_errno", B_ffi_set_errno},
            {"type", B_ffi_type_lookup},
            {"function", B_ffi_function},
            {"declare", B_ffi_declare},
        })));
#endif
    root->payload->items.push_back(std::make_pair(
        newstrval("module"),
        builtin_module({
            {"load", B_module_load},
            {"unload", B_module_unload},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("task"),
        builtin_module({
            {"sleep", B_task_sleep},
            {"yield", B_task_yield},
            {"wait", B_task_wait},
            {"wait_all", B_task_wait_all},
            {"cancel", B_task_cancel},
            {"done", B_task_done},
            {"catch", B_task_catch},
            {"try_wait", B_task_try_wait},
            {"try_wait_all", B_task_try_wait_all},
            {"retry", B_task_retry},
            {"timeout", B_task_timeout},
            {"rate_limit", B_task_rate_limit},
            {"acquire", B_task_acquire},
            {"bounded_parallel_map", B_task_bounded_parallel_map},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("generator"),
        builtin_module({
            {"next", B_generator_next},
            {"done", B_generator_done},
            {"close", B_generator_close},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("result"),
        builtin_module({
            {"ok", B_result_ok},
            {"err", B_result_err},
            {"is_ok", B_result_is_ok},
            {"is_err", B_result_is_err},
            {"unwrap", B_result_unwrap},
            {"unwrap_or", B_result_unwrap_or},
            {"value", B_result_value},
            {"error", B_result_error},
            {"capture", B_result_capture},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("http"),
        builtin_module({
            {"request", B_http_request},
            {"get", B_http_get},
            {"post", B_http_post},
            {"request_async", B_http_request_async},
            {"get_async", B_http_get_async},
            {"post_async", B_http_post_async},
            {"listen", B_http_listen},
            {"listen_tls", B_http_listen_tls},
            {"accept", B_http_accept},
            {"accept_async", B_http_accept_async},
            {"respond", B_http_respond},
            {"respond_async", B_http_respond_async},
            {"serve", B_http_serve},
            {"upgrade", B_http_upgrade},
            {"cookies", B_http_cookies},
            {"read_body", B_http_read_body},
            {"close", B_http_close},
        })));
#ifdef SKEIN_ENABLE_HTTP2
    root->payload->items.push_back(std::make_pair(
        newstrval("http2"),
        builtin_module({
            {"request", B_http2_request},
            {"get", B_http2_get},
            {"post", B_http2_post},
            {"request_async", B_http2_request_async},
            {"get_async", B_http2_get_async},
            {"post_async", B_http2_post_async},
            {"request_many", B_http2_request_many},
            {"request_many_async", B_http2_request_many_async},
            {"listen", B_http2_listen},
            {"listen_tls", B_http2_listen_tls},
            {"serve", B_http2_serve},
            {"close", B_http_close},
        })));
#endif
#ifdef SKEIN_ENABLE_TLS
    root->payload->items.push_back(std::make_pair(
        newstrval("tls"),
        builtin_module({
            {"client", B_tls_client},
            {"server", B_tls_server},
            {"alpn", B_tls_alpn},
        })));
#endif
    root->payload->items.push_back(std::make_pair(
        newstrval("websocket"),
        builtin_module({
            {"connect", B_ws_connect},
            {"connect_async", B_ws_connect_async},
            {"accept", B_ws_accept},
            {"send", B_ws_send},
            {"send_async", B_ws_send_async},
            {"receive", B_ws_receive},
            {"receive_async", B_ws_receive_async},
            {"ping", B_ws_ping},
            {"close", B_ws_close},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("error"),
        builtin_module({
            {"new", B_error_new},
            {"kind", B_error_kind},
            {"message", B_error_message},
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
        value_t *namespace_value = build_std_namespace();
        save_global_symbol(hidden.c_str(), namespace_value);
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
