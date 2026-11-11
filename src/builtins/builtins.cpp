#include "builtins.h"

#include "binding_registry.h"

#include "runtime_context.h"
#include "runtime_state.h"
#include "symbol.h"
#include "builtin_ids.h"
#include "compress_builtin.h"
#include "crypto_builtin.h"
#include "dict.h"
#include "encoding.h"
#include "env_builtin.h"
#include "eval_helpers.h"
#include "error.h"
#include "example_binding.h"
#include "fs_extra.h"
#include "generator.h"
#include "http.h"
#include "http2.h"
#include "websocket.h"
#include "builtins_io.h"
#include "json.h"
#include "net.h"
#include "result.h"
#include "task.h"
#ifdef SKEIN_ENABLE_TLS
#include "tls.h"
#endif
#ifdef SKEIN_ENABLE_FFI
#include "ffi_module.h"
#endif
#include "filetype.h"
#include "format.h"
#include "platform.h"
#include "process_builtin.h"
#include "process_spawn.h"
#include "random_builtin.h"
#include "regex_builtin.h"
#include "serialization.h"
#include "repl.h"
#include "stdio_stream.h"
#include "structtype.h"
#include "time_builtin.h"
#include "thread_channel.h"
#include "worker_pool.h"
#include "unicode.h"
#include "valuetype.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "builtins_internal.h"

#include "iter.h"

static struct value_t *module_load_builtin(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_string) {
        yyerror("std.plugin.load expects a string path");
        return NULL;
    }
    std::string import_path;
    std::string error;
    if (!skein::load_dynamic_module(
            ((struct string_value_t *)args[0])->value, import_path, error)) {
        yyerror("std.plugin.load: %s", error.c_str());
        return NULL;
    }
    return newstrval(import_path.c_str());
}

static struct value_t *module_unload_builtin(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_string) {
        yyerror("std.plugin.unload expects a string name");
        return NULL;
    }
    std::string error;
    if (!skein::unload_module(((struct string_value_t *)args[0])->value,
                              error)) {
        yyerror("std.plugin.unload: %s", error.c_str());
        return NULL;
    }
    return newnull();
}

static struct value_t *enum_tag_from_value(struct value_t *value) {
    if (!value || value->type != T_enum) {
        yyerror("std.enum.tag expects an enum value");
        return NULL;
    }
    struct enum_value_t *ev = (struct enum_value_t *)value;
    if (!ev->def || ev->variant < 0 ||
        (size_t)ev->variant >= ev->def->variants.size()) {
        yyerror("invalid enum value");
        return NULL;
    }
    const struct variant_def &variant =
        ev->def->variants[(size_t)ev->variant];
    std::string tag = ev->def->module_name + "." + ev->def->name + "." +
                      variant.name;
    return newstrval(tag.c_str());
}

/* Type hints accept both the source syntax (`module.Type`) and the
 * historical internal form (`module::Type`). */
static void split_type_hint_name(std::string &type_name,
                                 std::string &module_name) {
    size_t sep = type_name.rfind("::");
    size_t sep_len = 2;
    size_t dot = type_name.rfind('.');
    if (dot != std::string::npos &&
        (sep == std::string::npos || dot > sep)) {
        sep = dot;
        sep_len = 1;
    }
    if (sep == std::string::npos)
        return;
    module_name = type_name.substr(0, sep);
    type_name = type_name.substr(sep + sep_len);
}

static struct value_t *enum_tag_from_name(struct value_t *type_value,
                                          struct value_t *variant_value) {
    if (!type_value || type_value->type != T_string) {
        yyerror("std.enum.tag expects a type name string");
        return NULL;
    }
    if (!variant_value || variant_value->type != T_string) {
        yyerror("std.enum.tag expects a variant name string");
        return NULL;
    }

    std::string type_name = ((struct string_value_t *)type_value)->value;
    std::string variant_name =
        ((struct string_value_t *)variant_value)->value;
    std::string module_name;
    split_type_hint_name(type_name, module_name);

    enum_def *def = structtype_find_enum_by_name(
        module_name.empty() ? NULL : module_name.c_str(),
        type_name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", type_name.c_str());
        return NULL;
    }
    if (structtype_variant_index(def, variant_name) < 0) {
        yyerror("unknown enum variant %s.%s", def->name.c_str(),
                variant_name.c_str());
        return NULL;
    }

    std::string tag =
        def->module_name + "." + def->name + "." + variant_name;
    return newstrval(tag.c_str());
}

static bool parse_decode_options(const char *function_name,
                                 struct value_t *options,
                                 bool &defaults) {
    defaults = false;
    if (!options || options->type != T_dict) {
        yyerror("%s expects an options dict", function_name);
        return false;
    }
    struct dict_value_t *dict = (struct dict_value_t *)options;
    for (auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string) {
            yyerror("%s options keys must be strings", function_name);
            return false;
        }
        const char *key = ((struct string_value_t *)entry.first)->value;
        if (std::strcmp(key, "defaults") != 0) {
            yyerror("%s: unknown option %s", function_name, key);
            return false;
        }
        if (!entry.second || entry.second->type != T_bool) {
            yyerror("%s: option defaults expects bool", function_name);
            return false;
        }
        defaults = ((struct bool_value_t *)entry.second)->value;
    }
    return true;
}

static enum_def *resolve_enum_type_hint(const char *text) {
    if (!text || !*text) {
        yyerror("std.enum.decode expects a non-empty type name");
        return NULL;
    }
    std::string type_name(text);
    std::string module_name;
    split_type_hint_name(type_name, module_name);
    enum_def *def = structtype_find_enum_by_name(
        module_name.empty() ? NULL : module_name.c_str(),
        type_name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", type_name.c_str());
        return NULL;
    }
    return def;
}

static struct value_t *struct_decode_builtin(
    const std::vector<struct value_t *> &args) {
    bool defaults = false;
    if (args.size() == 3 &&
        !parse_decode_options("std.struct.decode", args[2], defaults))
        return NULL;
    return struct_decode_value_ex(args[0], args[1], defaults);
}

static struct value_t *enum_decode_builtin(
    const std::vector<struct value_t *> &args) {
    enum_def *expected = NULL;
    bool defaults = false;

    if (args.size() >= 2 && args[1]->type != T_null) {
        if (args[1]->type == T_string) {
            expected = resolve_enum_type_hint(
                ((struct string_value_t *)args[1])->value);
            if (!expected)
                return NULL;
        } else if (args[1]->type == T_dict) {
            if (args.size() != 2) {
                yyerror("std.enum.decode expects a type name before options");
                return NULL;
            }
            if (!parse_decode_options("std.enum.decode", args[1], defaults))
                return NULL;
        } else {
            yyerror("std.enum.decode expects a type name, options dict, or null");
            return NULL;
        }
    }

    if (args.size() == 3) {
        if (args[1]->type != T_string && args[1]->type != T_null) {
            yyerror("std.enum.decode expects a type name or null");
            return NULL;
        }
        if (!parse_decode_options("std.enum.decode", args[2], defaults))
            return NULL;
    }

    return enum_decode_value_ex(args[0], expected, defaults);
}


bool builtins_register_modules(std::string &error) {
#ifdef SKEIN_ENABLE_FFI
    if (struct skein_context *context = skein_context_current())
        context->drain_background = ffi::background_drain;
#endif
    return skein::register_module(skein_example_module(), nullptr, error);
}

namespace {

struct slice_indices {
    long long start;
    long long stop;
    long long step;
};

bool slice_int_argument(const struct value_t *value, const char *name,
                        bool &present, long long &out) {
    present = false;
    out = 0;
    if (!value || value->type == T_null)
        return true;
    if (value->type != T_int) {
        yyerror("slice %s must be an integer or null, got %s", name,
                typename_of(value));
        return false;
    }
    present = true;
    out = ((const struct int_value_t *)value)->value;
    return true;
}

bool normalize_slice(long long length, bool start_present, long long start,
                     bool stop_present, long long stop, bool step_present,
                     long long step, slice_indices &out) {
    if (step_present && step == 0) {
        yyerror("slice step cannot be zero");
        return false;
    }
    if (!step_present)
        step = 1;

    const __int128 n = length;
    const __int128 lower = step > 0 ? 0 : -1;
    const __int128 upper = step > 0 ? n : n - 1;

    __int128 first;
    if (!start_present) {
        first = step > 0 ? lower : upper;
    } else {
        first = start;
        if (first < 0)
            first += n;
        if (first < lower)
            first = lower;
        else if (first > upper)
            first = upper;
    }

    __int128 last;
    if (!stop_present) {
        last = step > 0 ? upper : lower;
    } else {
        last = stop;
        if (last < 0)
            last += n;
        if (last < lower)
            last = lower;
        else if (last > upper)
            last = upper;
    }

    out.start = (long long)first;
    out.stop = (long long)last;
    out.step = step;
    return true;
}

template <typename Append>
void for_each_slice_index(const slice_indices &indices, Append append) {
    /* Use a wider accumulator so extreme step values cannot overflow the
     * loop index. */
    if (indices.step > 0) {
        for (__int128 i = indices.start; i < indices.stop; i += indices.step)
            append((long long)i);
    } else {
        for (__int128 i = indices.start; i > indices.stop; i += indices.step)
            append((long long)i);
    }
}

void utf8_slice_offsets(const std::string &text,
                        std::vector<size_t> &offsets) {
    offsets.clear();
    const size_t length = text.size();
    size_t pos = 0;
    while (pos < length) {
        offsets.push_back(pos);
        pos += utf8_valid_sequence_length(text.data() + pos, length - pos);
    }
    offsets.push_back(length);
}

struct value_t *call_slice_builtin(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 4) {
        yyerror("slice expects 4 arguments, got %d", (int)args.size());
        return NULL;
    }

    bool start_present = false;
    bool stop_present = false;
    bool step_present = false;
    long long start = 0;
    long long stop = 0;
    long long step = 1;
    if (!slice_int_argument(args[1], "start", start_present, start) ||
        !slice_int_argument(args[2], "stop", stop_present, stop) ||
        !slice_int_argument(args[3], "step", step_present, step))
        return NULL;

    struct value_t *container = args[0];

    if (container->type == T_list) {
        const struct list_value_t *source =
            (const struct list_value_t *)container;
        const long long length =
            (long long)source->payload->items.size();
        slice_indices indices;
        if (!normalize_slice(length, start_present, start, stop_present,
                             stop, step_present, step, indices))
            return NULL;

        struct list_value_t *result = newlistval();
        for_each_slice_index(indices, [&](long long index) {
            result->payload->items.push_back(
                dupval(source->payload->items[(size_t)index]));
        });
        return (struct value_t *)result;
    }

    if (container->type == T_string) {
        const std::string &text =
            ((const struct string_value_t *)container)->value;
        std::vector<size_t> offsets;
        utf8_slice_offsets(text, offsets);
        const long long length = (long long)(offsets.size() - 1);
        slice_indices indices;
        if (!normalize_slice(length, start_present, start, stop_present,
                             stop, step_present, step, indices))
            return NULL;

        std::string result;
        for_each_slice_index(indices, [&](long long index) {
            const size_t begin = offsets[(size_t)index];
            const size_t end = offsets[(size_t)index + 1];
            result.append(text.data() + begin, end - begin);
        });
        return newstrval(result.c_str());
    }

    if (container->type == T_bytes) {
        const std::string &data =
            ((const struct bytes_value_t *)container)->value;
        const long long length = (long long)data.size();
        slice_indices indices;
        if (!normalize_slice(length, start_present, start, stop_present,
                             stop, step_present, step, indices))
            return NULL;

        std::string result;
        for_each_slice_index(indices, [&](long long index) {
            result.push_back(data[(size_t)index]);
        });
        return newbytesval(std::move(result));
    }

    yyerror("slice expects list, string, or bytes, got %s",
            typename_of(container));
    return NULL;
}

} // namespace

struct value_t *call_builtin(int functype,
                             const std::vector<struct value_t *> &args) {
    if (functype == B_slice)
        return call_slice_builtin(args);
    if (functype >= B_meta_type_info && functype <= B_meta_has_type)
        return call_meta_builtin(functype, args);
    if (functype >= B_fn_compose && functype <= B_fn_always)
        return call_fn_builtin(functype, args);
    if (functype >= B_iter_map && functype <= B_iter_for_each)
        return call_iter_builtin(functype, args);
    if (functype == B_ffi_bind) {
#ifdef SKEIN_ENABLE_FFI
        return call_ffi_builtin(functype, args);
#else
        yyerror("ffi bindings require an FFI-enabled build");
        return NULL;
#endif
    }
    if (functype == B_module_load)
        return module_load_builtin(args);
    if (functype == B_module_unload)
        return module_unload_builtin(args);
    if (functype >= B_net_tcp_connect && functype <= B_net_shutdown_async)
        return call_net_builtin(functype, args);
#ifdef SKEIN_ENABLE_FFI
    if (functype >= B_ffi_open && functype <= B_ffi_declare)
        return call_ffi_builtin(functype, args);
#endif
    if (functype >= B_task_sleep && functype <= B_task_bounded_parallel_map)
        return call_task_builtin(functype, args);
    if (functype >= B_generator_next && functype <= B_generator_close)
        return call_generator_builtin(functype, args);
    if (functype >= B_result_ok && functype <= B_result_capture)
        return call_result_builtin(functype, args);
    if (functype >= B_error_new && functype <= B_error_message)
        return call_error_builtin(functype, args);
    if (functype >= B_io_read && functype <= B_io_write_async)
        return call_io_builtin(functype, args);
    if (functype >= B_json_parse && functype <= B_json_stringify)
        return call_json_builtin(functype, args);
    if (functype >= B_encoding_decode && functype <= B_encoding_supports)
        return call_encoding_builtin(functype, args);
    if (functype >= B_encoding_base64_encode &&
        functype <= B_encoding_url_decode)
        return call_encoding_builtin(functype, args);
    if (functype >= B_unicode_normalize && functype <= B_unicode_version)
        return call_unicode_builtin(functype, args);
    if (functype >= B_regex_match && functype <= B_regex_split)
        return call_regex_builtin(functype, args);
    if (functype >= B_time_now && functype <= B_time_sleep)
        return call_time_builtin(functype, args);
    if (functype >= B_random_seed && functype <= B_random_uuid)
        return call_random_builtin(functype, args);
    if (functype >= B_fs_listdir && functype <= B_fs_path_normalize)
        return call_fs_extra_builtin(functype, args);
    if (functype >= B_env_get && functype <= B_env_all)
        return call_env_builtin(functype, args);
    if (functype == B_process_run_command)
        return call_process_builtin(functype, args);
    if ((functype >= B_process_spawn_command &&
         functype <= B_process_result) ||
        functype == B_process_run_function ||
        functype == B_process_spawn_function)
        return call_process_spawn_builtin(functype, args);
    if (functype >= B_worker_pool && functype <= B_worker_stats)
        return call_worker_builtin(functype, args);
    if (functype >= B_thread_message_from &&
        functype <= B_thread_capabilities)
        return call_thread_channel_builtin(functype, args);
    if (functype >= B_crypto_sha256 && functype <= B_crypto_hmac_sha256)
        return call_crypto_builtin(functype, args);
    if (functype >= B_compress_gzip && functype <= B_compress_inflate)
        return call_compress_builtin(functype, args);
    if (functype >= B_http_request &&
        functype <= B_http_thread_capabilities)
        return call_http_builtin(functype, args);
#ifdef SKEIN_ENABLE_HTTP2
    if (functype >= B_http2_request && functype <= B_http2_serve)
        return call_http2_builtin(functype, args);
#endif
#ifdef SKEIN_ENABLE_TLS
    if (functype >= B_tls_client && functype <= B_tls_alpn)
        return call_tls_builtin(functype, args);
#endif
    if (functype >= B_ws_connect && functype <= B_ws_close)
        return call_websocket_builtin(functype, args);

    if (functype == B_list || functype == B_dict) {
        if (!args.empty()) {
            yyerror("built-in function expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        return (functype == B_list)
                   ? (struct value_t *)newlistval()
                   : (struct value_t *)newdictval();
    }

    if (functype == B_format || functype == B_print ||
        functype == B_println) {
        bool is_print = functype == B_print || functype == B_println;
        bool add_newline = functype == B_println;
        const char *name = functype == B_format
                               ? "format"
                               : (functype == B_println ? "println" : "print");
        struct value_t *ret = NULL;
        if (functype == B_format && args.empty()) {
            yyerror("format expects at least 1 argument");
        } else if (is_print && args.empty()) {
            if (add_newline) {
                std::lock_guard<std::mutex> lock(stdio_output_mutex());
                std::printf("\n");
            }
            ret = newnull();
        } else if (is_print && args.size() == 1) {
            std::lock_guard<std::mutex> lock(stdio_output_mutex());
            printvalue(args[0]);
            if (add_newline)
                std::printf("\n");
            ret = newnull();
        } else if (args[0]->type != T_string) {
            yyerror("%s first argument must be a string, got %s", name,
                    typename_of(args[0]));
        } else {
            std::vector<struct value_t *> fmt_args(args.begin() + 1, args.end());
            std::string out;
            std::string error;
            if (!format_values(((struct string_value_t *)args[0])->value,
                               fmt_args, out, error)) {
                yyerror("%s", error.c_str());
            } else if (functype == B_format) {
                ret = newstrval(out.c_str());
            } else {
                std::lock_guard<std::mutex> lock(stdio_output_mutex());
                std::printf("%s%s", out.c_str(), add_newline ? "\n" : "");
                ret = newnull();
            }
        }
        if (is_print && g_interactive) {
            std::lock_guard<std::mutex> lock(stdio_output_mutex());
            std::fflush(stdout);
        }
        return ret;
    }

    if (functype == B_command || functype == B_command_status) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d",
                    functype == B_command ? "command" : "command_status",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_write_file || functype == B_append_file) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_write_file ? "write_file" : "append_file",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_open || functype == B_file_write) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_file_open ? "file_open" : "file_write",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_read_text) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.fs.read_text expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_write_text) {
        if (args.size() < 3 || args.size() > 4) {
            yyerror("std.fs.write_text expects 3 or 4 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_seek) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("file_seek expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_input) {
        if (args.size() > 1) {
            yyerror("input expects 0 or 1 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_split || functype == B_find) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_split ? "split" : "find",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_substr) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("substr expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_get || functype == B_bytes_concat ||
               functype == B_write_bytes || functype == B_append_bytes ||
               functype == B_file_write_bytes) {
        if (args.size() != 2) {
            const char *name =
                functype == B_bytes_get ? "bytes_get" :
                functype == B_bytes_concat ? "bytes_concat" :
                functype == B_write_bytes ? "write_bytes" :
                functype == B_append_bytes ? "append_bytes" :
                "file_write_bytes";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("bytes_slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_to_int) {
        if (args.size() < 3 || args.size() > 5) {
            yyerror("bytes_to_int expects 3 to 5 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_int_to_bytes) {
        if (args.size() != 3) {
            yyerror("int_to_bytes expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read_bytes) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read_bytes expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_range) {
        if (args.empty() || args.size() > 3) {
            yyerror("range expects 1 to 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_contains || functype == B_list_index ||
               functype == B_dict_has || functype == B_string_join ||
               functype == B_string_starts_with ||
               functype == B_string_ends_with) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_contains ? "std.list.contains" :
                functype == B_list_index ? "std.list.index" :
                functype == B_dict_has ? "std.dict.has" :
                functype == B_string_join ? "std.string.join" :
                functype == B_string_starts_with ? "std.string.starts_with" :
                "std.string.ends_with";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_dict_get) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.dict.get expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_string_replace) {
        if (args.size() != 3) {
            yyerror("std.string.replace expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_append || functype == B_dict_delete ||
               functype == B_dict_merge || functype == B_string_contains ||
               functype == B_string_repeat || functype == B_list_map ||
               functype == B_list_filter) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_append ? "std.list.append" :
                functype == B_dict_delete ? "std.dict.delete" :
                functype == B_dict_merge ? "std.dict.merge" :
                functype == B_string_contains ? "std.string.contains" :
                functype == B_string_repeat ? "std.string.repeat" :
                functype == B_list_map ? "std.list.map" : "std.list.filter";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_reduce) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.reduce expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_sin || functype == B_cos ||
               functype == B_tan || functype == B_asin ||
               functype == B_acos || functype == B_atan ||
               functype == B_floor || functype == B_ceil ||
               functype == B_round || functype == B_trunc ||
               functype == B_log2 || functype == B_log10) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_atan2 || functype == B_pow ||
               functype == B_hypot) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_sign ||
               functype == B_math_is_nan ||
               functype == B_math_is_inf ||
               functype == B_math_is_finite ||
               functype == B_math_cbrt ||
               functype == B_math_degrees ||
               functype == B_math_radians) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_fmod ||
               functype == B_math_mod ||
               functype == B_math_gcd ||
               functype == B_math_lcm) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_clamp) {
        if (args.size() != 3) {
            yyerror("%s expects 3 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_min || functype == B_max) {
        if (args.empty()) {
            yyerror("%s expects at least 1 argument", math_builtin_name(functype));
            return NULL;
        }
    } else if (functype == B_enum_decode) {
        if (args.empty() || args.size() > 3) {
            yyerror("std.enum.decode expects 1 to 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_struct_decode) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.struct.decode expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_enum_tag) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.enum.tag expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_sort) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.list.sort expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_sort_by) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.sort_by expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_is_sorted) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.list.is_sorted expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_unique ||
               functype == B_list_unique_by) {
        size_t expected = functype == B_list_unique ? 1 : 2;
        if (args.size() != expected) {
            yyerror("%s expects %d argument%s, got %d",
                    functype == B_list_unique ? "std.list.unique"
                                              : "std.list.unique_by",
                    (int)expected, expected == 1 ? "" : "s",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_group_by ||
               functype == B_list_chunk) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_list_group_by ? "std.list.group_by"
                                                : "std.list.chunk",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_window) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.window expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_zip) {
        if (args.size() < 2) {
            yyerror("std.list.zip expects at least 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_top_k ||
               functype == B_list_binary_search) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("%s expects 2 or 3 arguments, got %d",
                    functype == B_list_top_k ? "std.list.top_k"
                                             : "std.list.binary_search",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_new) {
        if (args.size() != 1) {
            yyerror("std.lru.new expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_get || functype == B_lru_peek) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("%s expects 2 or 3 arguments, got %d",
                    functype == B_lru_get ? "std.lru.get" : "std.lru.peek",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_put) {
        if (args.size() != 3) {
            yyerror("std.lru.put expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_delete || functype == B_lru_has) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_lru_delete ? "std.lru.delete"
                                             : "std.lru.has",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_lru_keys && functype <= B_lru_clear) {
        if (args.size() != 1) {
            yyerror("std.lru function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_deque_new) {
        if (!args.empty()) {
            yyerror("std.deque.new expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_deque_push_front ||
               functype == B_deque_push_back) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_deque_push_front
                        ? "std.deque.push_front"
                        : "std.deque.push_back",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_deque_from_list &&
               functype <= B_deque_to_list) {
        if (args.size() != 1) {
            yyerror("std.deque function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_heap_new) {
        if (args.size() != 1) {
            yyerror("std.heap.new expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_heap_from_list || functype == B_heap_push) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_heap_from_list ? "std.heap.from_list"
                                                 : "std.heap.push",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_heap_size && functype <= B_heap_to_list) {
        if (args.size() != 1) {
            yyerror("std.heap function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_new) {
        if (!args.empty()) {
            yyerror("std.set.new expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_from_list || functype == B_set_size ||
               functype == B_set_values) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    functype == B_set_from_list ? "std.set.from_list"
                    : functype == B_set_size ? "std.set.size"
                                             : "std.set.values",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_add || functype == B_set_remove ||
               functype == B_set_has || functype == B_set_union ||
               functype == B_set_intersect || functype == B_set_difference ||
               functype == B_set_is_subset) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_set_add ? "std.set.add"
                    : functype == B_set_remove ? "std.set.remove"
                    : functype == B_set_has ? "std.set.has"
                    : functype == B_set_union ? "std.set.union"
                    : functype == B_set_intersect ? "std.set.intersect"
                    : functype == B_set_difference ? "std.set.difference"
                                                   : "std.set.is_subset",
                    (int)args.size());
            return NULL;
        }
    } else if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        return NULL;
    }

    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;

    if (functype >= B_sqrt && functype <= B_math_radians)
        return call_math_builtin(functype, args);
    if (functype >= B_list_slice && functype <= B_list_binary_search)
        return call_list_builtin(functype, args);
    if (functype >= B_dict_get && functype <= B_dict_merge)
        return call_dict_builtin(functype, args);
    if (functype >= B_string_join && functype <= B_string_trim_end)
        return call_string_builtin(functype, args);
    if (functype >= B_int && functype <= B_type_source)
        return call_convert_builtin(functype, args);
    if ((functype >= B_command && functype <= B_input) ||
        (functype >= B_read_text && functype <= B_write_text))
        return call_file_builtin(functype, args);
    if (functype >= B_split && functype <= B_substr)
        return call_text_builtin(functype, args);
    if (functype >= B_bytes_from_string && functype <= B_file_write_bytes)
        return call_bytes_builtin(functype, args);
    if (functype >= B_lru_new && functype <= B_lru_clear)
        return call_lru_builtin(functype, args);
    if (functype >= B_deque_new && functype <= B_deque_to_list)
        return call_deque_builtin(functype, args);
    if (functype >= B_heap_new && functype <= B_heap_to_list)
        return call_heap_builtin(functype, args);
    if (functype >= B_set_new && functype <= B_set_is_subset)
        return call_set_builtin(functype, args);

    switch (functype) {
        case B_enum_decode:
            ret = enum_decode_builtin(args);
            break;
        case B_enum_tag:
            if (args.size() == 1)
                ret = enum_tag_from_value(arg);
            else
                ret = enum_tag_from_name(args[0], args[1]);
            break;
        case B_struct_decode:
            ret = struct_decode_builtin(args);
            break;

        case B_len:
            if (arg->type == T_string) {
                ret = newintval((long long)utf8_length(
                    ((struct string_value_t *)arg)->value));
            } else if (arg->type == T_char || arg->type == T_byte) {
                ret = newintval(1);
            } else if (arg->type == T_bytes) {
                ret = newintval((long long)((struct bytes_value_t *)arg)->value.size());
            } else if (arg->type == T_list) {
                ret = newintval((long long)((struct list_value_t *)arg)->payload->items.size());
            } else if (arg->type == T_dict) {
                ret = newintval((long long)((struct dict_value_t *)arg)->payload->items.size());
            } else if (arg->type == T_deque) {
                struct deque_value_t *deque = (struct deque_value_t *)arg;
                ret = newintval((long long)(deque->payload->front_size +
                                            deque->payload->back_size));
            } else if (arg->type == T_heap) {
                ret = newintval(
                    (long long)((struct heap_value_t *)arg)->payload->size);
            } else if (arg->type == T_set) {
                ret = newintval(
                    (long long)((struct set_value_t *)arg)->payload->size);
            } else {
                yyerror("len expects string, bytes, list, dict, deque, heap, or set, got %s",
                        typename_of(arg));
            }
            break;
        case B_range: {
            long long start = 0;
            long long length = 0;
            long long step = 1;

            if (args.size() == 1) {
                if (args[0]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[0])->value;
            } else {
                if (args[0]->type != T_int) {
                    yyerror("range start must be an integer");
                    break;
                }
                start = ((struct int_value_t *)args[0])->value;
                if (args[1]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[1])->value;
                if (args.size() == 3) {
                    if (args[2]->type != T_int) {
                        yyerror("range step must be an integer");
                        break;
                    }
                    step = ((struct int_value_t *)args[2])->value;
                }
            }

            if (length < 0) {
                yyerror("range length must be non-negative");
                break;
            }
            if (step == 0) {
                yyerror("range step must not be zero");
                break;
            }

            struct list_value_t *list = newlistval();
            list->payload->items.reserve((size_t)length);
            for (long long i = 0; i < length; ++i) {
                __int128 value = (__int128)start + (__int128)i * step;
                if (value < LLONG_MIN || value > LLONG_MAX) {
                    yyerror("range value overflow");
                    freeval((struct value_t *)list);
                    list = NULL;
                    break;
                }
                list->payload->items.push_back(newintval((long long)value));
            }
            if (list)
                ret = (struct value_t *)list;
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
