#include "std_namespace.h"

#include "builtin_ids.h"
#include "dict.h"
#include "stdio_stream.h"
#include "valuetype.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

namespace skein {
namespace stdlib {
namespace {

#include "std_container_adapter_source.inc"

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

value_t *build_namespace_impl() {
    struct dict_value_t *root = newdictval();
    root->payload->is_namespace = true;
    value_t *list_module = builtin_module({
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
    });
    value_t *fn_module = builtin_module({
        {"compose", B_fn_compose},
        {"pipe", B_fn_pipe},
        {"partial", B_fn_partial},
        {"identity", B_fn_identity},
        {"always", B_fn_always},
    });
    value_t *iter_module = builtin_module({
        {"map", B_iter_map},
        {"filter", B_iter_filter},
        {"take", B_iter_take},
        {"drop", B_iter_drop},
        {"enumerate", B_iter_enumerate},
        {"collect", B_iter_collect},
        {"for_each", B_iter_for_each},
    });
    value_t *dict_module = builtin_module({
        {"len", B_len},
        {"get", B_dict_get},
        {"has", B_dict_has},
        {"keys", B_dict_keys},
        {"values", B_dict_values},
        {"items", B_dict_items},
        {"delete", B_dict_delete},
        {"merge", B_dict_merge},
    });
    value_t *bytes_module = builtin_module({
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
    });
    value_t *lru_module = builtin_module({
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
    });
    auto copy_module = [](value_t *module) -> value_t * {
        value_t *copy = (value_t *)newdictval();
        ((dict_value_t *)copy)->payload->is_namespace = true;
        for (const auto &entry : ((dict_value_t *)module)->payload->items) {
            dict_append_item((dict_value_t *)copy, dupval(entry.first),
                             dupval(entry.second));
        }
        return copy;
    };
    struct dict_value_t *raw = newdictval();
    raw->payload->is_namespace = true;
    raw->payload->items.push_back(std::make_pair(
        newstrval("list"), copy_module(list_module)));
    raw->payload->items.push_back(std::make_pair(
        newstrval("dict"), copy_module(dict_module)));
    raw->payload->items.push_back(std::make_pair(
        newstrval("bytes"), copy_module(bytes_module)));
    raw->payload->items.push_back(std::make_pair(
        newstrval("lru"), copy_module(lru_module)));
    root->payload->items.push_back(std::make_pair(
        newstrval("list"), list_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("fn"), fn_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("iter"), iter_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("dict"), dict_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("bytes"), bytes_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("lru"), lru_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("raw"), (value_t *)raw));
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
        {"println", B_println},
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
        {"read_task", B_io_read_async},
        {"write_task", B_io_write_async},
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
            {"run_command", B_process_run_command},
            {"spawn_command", B_process_spawn_command},
            {"run_entry", B_process_run_entry},
            {"spawn_entry", B_process_spawn_entry},
            {"run_function", B_process_run_function},
            {"spawn_function", B_process_spawn_function},
            {"result", B_process_result},
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
            {"submit_bytes", B_worker_submit_bytes},
            {"call_bytes", B_worker_call_bytes},
            {"wait", B_worker_wait},
            {"shutdown", B_worker_shutdown},
            {"stats", B_worker_stats},
        })));
    value_t *message_namespace = builtin_module({
        {"from", B_thread_message_from},
    });
    value_t *thread_module = builtin_module({
        {"Channel", B_thread_channel},
        {"unbounded_channel", B_thread_unbounded_channel},
        {"pool", B_thread_pool},
        {"spawn_script", B_thread_spawn_script},
        {"spawn_source", B_thread_spawn_source},
        {"spawn_function", B_thread_spawn_function},
        {"spawn_entry", B_thread_spawn_entry},
        {"duplex", B_thread_duplex},
        {"spawn_script_with_channel", B_thread_spawn_script_with_channel},
        {"spawn_script_with_duplex", B_thread_spawn_script_with_duplex},
        {"debug_counts", B_thread_debug_counts},
        {"collect", B_thread_collect},
        {"select", B_thread_select},
        {"try_broadcast", B_thread_try_broadcast},
        {"broadcast", B_thread_broadcast},
        {"capabilities", B_thread_capabilities},
    });
    dict_append_item((struct dict_value_t *)thread_module,
                     newstrval("Message"), message_namespace);
    root->payload->items.push_back(std::make_pair(
        newstrval("thread"), thread_module));
    root->payload->items.push_back(std::make_pair(
        newstrval("entry"),
        builtin_module({
            {"encode", B_entry_encode},
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
        newstrval("meta"),
        builtin_module({
            {"type_info", B_meta_type_info},
            {"fields", B_meta_fields},
            {"variants", B_meta_variants},
            {"methods", B_meta_methods},
            {"traits", B_meta_traits},
            {"implements", B_meta_implements},
            {"has_type", B_meta_has_type},
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
            {"tcp_connect_task", B_net_tcp_connect_async},
            {"tcp_listen_task", B_net_tcp_listen_async},
            {"accept_task", B_net_accept_async},
            {"send_task", B_net_send_async},
            {"recv_task", B_net_recv_async},
            {"resolve_task", B_net_resolve_async},
            {"shutdown_task", B_net_shutdown_async},
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
        newstrval("plugin"),
        builtin_module({
            {"load", B_module_load},
            {"unload", B_module_unload},
        })));
    root->payload->items.push_back(std::make_pair(
        newstrval("task"),
        builtin_module({
            {"sleep", B_task_sleep},
            {"yield", B_task_yield},
            {"scope", B_task_scope},
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
            {"request_task", B_http_request_async},
            {"get_task", B_http_get_async},
            {"post_task", B_http_post_async},
            {"listen", B_http_listen},
            {"listen_tls", B_http_listen_tls},
            {"accept", B_http_accept},
            {"accept_task", B_http_accept_async},
            {"respond", B_http_respond},
            {"respond_task", B_http_respond_async},
            {"serve", B_http_serve},
            {"serve_processes", B_http_serve_processes},
            {"serve_threads", B_http_serve_threads},
            {"upgrade", B_http_upgrade},
            {"cookies", B_http_cookies},
            {"read_body", B_http_read_body},
            {"close", B_http_close},
            {"thread_capabilities", B_http_thread_capabilities},
        })));
#ifdef SKEIN_ENABLE_HTTP2
    root->payload->items.push_back(std::make_pair(
        newstrval("http2"),
        builtin_module({
            {"request", B_http2_request},
            {"get", B_http2_get},
            {"post", B_http2_post},
            {"request_task", B_http2_request_async},
            {"get_task", B_http2_get_async},
            {"post_task", B_http2_post_async},
            {"request_many", B_http2_request_many},
            {"request_many_task", B_http2_request_many_async},
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
            {"reused", B_tls_reused},
            {"peer_subject", B_tls_peer_subject},
            {"server_name", B_tls_server_name},
        })));
#endif
    root->payload->items.push_back(std::make_pair(
        newstrval("websocket"),
        builtin_module({
            {"connect", B_ws_connect},
            {"connect_task", B_ws_connect_async},
            {"accept", B_ws_accept},
            {"send", B_ws_send},
            {"send_task", B_ws_send_async},
            {"receive", B_ws_receive},
            {"receive_task", B_ws_receive_async},
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

} // namespace

value_t *build_namespace() { return build_namespace_impl(); }

struct std_adapter_binding {
    const char *module;
    const char *name;
    const char *export_name;
};

const std_adapter_binding kStdAdapterBindings[] = {
    {"list", "slice", "adapter_list_slice"},
    {"list", "sort", "adapter_list_sort"},
    {"list", "sort_by", "adapter_list_sort_by"},
    {"list", "is_sorted", "adapter_list_is_sorted"},
    {"list", "top_k", "adapter_list_top_k"},
    {"list", "binary_search", "adapter_list_binary_search"},
    {"list", "reduce", "adapter_list_reduce"},
    {"dict", "get", "adapter_dict_get"},
    {"lru", "get", "adapter_lru_get"},
    {"lru", "peek", "adapter_lru_peek"},
    {"bytes", "slice", "adapter_bytes_slice"},
};

value_t *take_adapter_export(dict_value_t *adapter, const char *name) {
    if (!adapter || !name)
        return NULL;
    for (const auto &entry : adapter->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((const string_value_t *)entry.first)->value,
                        name) == 0)
            return dupval(entry.second);
    }
    return NULL;
}

bool replace_module_function(dict_value_t *root, const char *module,
                             const char *name, value_t *wrapper) {
    if (!root || !module || !name || !wrapper)
        return false;
    for (auto &module_entry : root->payload->items) {
        if (module_entry.first->type != T_string ||
            std::strcmp(((const string_value_t *)module_entry.first)->value,
                        module) != 0)
            continue;
        if (module_entry.second->type != T_dict)
            return false;
        dict_value_t *functions = (dict_value_t *)module_entry.second;
        for (auto &entry : functions->payload->items) {
            if (entry.first->type == T_string &&
                std::strcmp(((const string_value_t *)entry.first)->value,
                            name) == 0) {
                freeval(entry.second);
                entry.second = wrapper;
                dict_invalidate_index(functions);
                return true;
            }
        }
        return false;
    }
    return false;
}

bool install_adapters(value_t *root, const adapter_parser &parse,
                      std::string &error) {
    value_t *adapter = NULL;
    if (!parse(kStdContainerAdapterSource, "<std.adapter>",
               "container_adapter", "std.adapter", adapter, error))
        return false;

    dict_value_t *adapter_dict = (dict_value_t *)adapter;
    for (const std_adapter_binding &binding : kStdAdapterBindings) {
        value_t *wrapper =
            take_adapter_export(adapter_dict, binding.export_name);
        if (!wrapper ||
            !replace_module_function((dict_value_t *)root, binding.module,
                                     binding.name, wrapper)) {
            if (wrapper)
                freeval(wrapper);
            freeval(adapter);
            error = std::string("std adapter could not install ") +
                    binding.module + "." + binding.name;
            return false;
        }
    }
    freeval(adapter);
    return true;
}

} // namespace stdlib
} // namespace skein
