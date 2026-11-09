#include "builtins_internal.h"
#include "runtime_state.h"

static bool collect_command_env(
    const std::vector<struct value_t *> &args, size_t index,
    const char *who,
    std::vector<std::pair<std::string, std::string>> &env) {
    if (index >= args.size())
        return true;

    struct value_t *value = args[index];
    if (value->type != T_dict) {
        yyerror("%s env must be a dict, got %s", who, typename_of(value));
        return false;
    }

    struct dict_value_t *dict = (struct dict_value_t *)value;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        struct value_t *key = dict->payload->items[i].first;
        struct value_t *val = dict->payload->items[i].second;
        if (key->type != T_string) {
            yyerror("%s env keys must be strings", who);
            return false;
        }
        if (val->type != T_string) {
            yyerror("%s env values must be strings", who);
            return false;
        }
        const char *name = ((struct string_value_t *)key)->value;
        if (!name[0] || std::strchr(name, '=') != NULL) {
            yyerror("invalid environment variable name");
            return false;
        }
        env.emplace_back(name,
                         ((struct string_value_t *)val)->value);
    }
    return true;
}

static std::string quote_shell(const std::string &text) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : text) {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out += "'";
    return out;
#endif
}

static std::string command_in_thread_cwd(const std::string &command) {
    if (thread_cwd_override().empty())
        return command;
    const std::string cwd = thread_cwd();
    if (cwd.empty())
        return command;
#ifdef _WIN32
    return "cd /d " + quote_shell(cwd) + " && " + command;
#else
    return "cd " + quote_shell(cwd) + " && " + command;
#endif
}

static void merge_thread_env(
    std::vector<std::pair<std::string, std::string>> &env) {
    std::vector<std::pair<std::string, std::string>> overrides;
    thread_env_overrides(overrides);
    for (const auto &override : overrides) {
        bool replaced = false;
        for (auto &entry : env) {
            if (entry.first == override.first) {
                entry.second = override.second;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            env.push_back(override);
    }
}

bool get_file_resource(struct value_t *value,
                              std::shared_ptr<file_resource> &file,
                              const char *who) {
    if (!is_file_value(value)) {
        yyerror("%s expects a file, got %s", who, typename_of(value));
        return false;
    }
    file = file_value(value);
    if (!file) {
        yyerror("%s expects a file", who);
        return false;
    }
    return true;
}

static void erase_last_utf8_character(std::string &line) {
    if (line.empty())
        return;
    size_t pos = line.size() - 1;
    while (pos > 0 && (((unsigned char)line[pos]) & 0xC0) == 0x80)
        --pos;
    line.erase(pos);
}

static bool read_console_line(const char *prompt, std::string &line,
                              bool &at_eof) {
    line.clear();
    at_eof = false;

    if (prompt) {
        std::fputs(prompt, stdout);
        std::fflush(stdout);
    }

    for (;;) {
        int c = std::fgetc(stdin);
        if (c == EOF) {
            if (g_interrupted) {
                std::clearerr(stdin);
                return false;
            }
            if (std::ferror(stdin)) {
                std::clearerr(stdin);
                yyerror("input: read error");
                return false;
            }
            at_eof = true;
            return true;
        }
        if (c == '\n')
            return true;
        if (c == '\r')
            continue;
        if (c == '\b' || c == 0x7f) {
            erase_last_utf8_character(line);
            continue;
        }
        line.push_back((char)c);
    }
}


struct value_t *call_file_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_command: {
            if (arg->type != T_string) {
                yyerror("command expects a string, got %s", typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command", env))
                    break;
                merge_thread_env(env);
                std::string output;
                int status = 0;
                if (!platform_run_command(
                        command_in_thread_cwd(
                            ((struct string_value_t *)arg)->value),
                        env, output, status))
                    yyerror("command failed to start");
                else
                    ret = newstrval(output.c_str());
            }
            break;
        }
        case B_command_status: {
            if (arg->type != T_string) {
                yyerror("command_status expects a string, got %s",
                        typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command_status", env))
                    break;
                merge_thread_env(env);
                int status = 0;
                if (!platform_run_command_status(
                        command_in_thread_cwd(
                            ((struct string_value_t *)arg)->value),
                        env, status))
                    yyerror("command_status failed to start");
                else
                    ret = newintval(status);
            }
            break;
        }
        case B_read_text: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("std.fs.read_text expects a string path and "
                        "encoding, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string errors = "strict";
            if (args.size() == 3) {
                if (args[2]->type != T_string) {
                    yyerror("std.fs.read_text expects an errors string, got %s",
                            typename_of(args[2]));
                    break;
                }
                errors = ((struct string_value_t *)args[2])->value;
            }
            std::string data;
            std::string error;
            const std::string path = thread_resolve_path(
                ((struct string_value_t *)args[0])->value);
            if (!platform_read_file(path, data, error)) {
                yyerror("std.fs.read_text: %s", error.c_str());
                break;
            }
            std::string text;
            if (!decode_text(
                    data, ((struct string_value_t *)args[1])->value, errors,
                    text, error)) {
                yyerror("std.fs.read_text: %s", error.c_str());
                break;
            }
            ret = newstrval(text.c_str());
            break;
        }
        case B_write_text: {
            if (args[0]->type != T_string || args[1]->type != T_string ||
                args[2]->type != T_string) {
                yyerror("std.fs.write_text expects a string path, text and "
                        "encoding, got %s, %s and %s",
                        typename_of(args[0]), typename_of(args[1]),
                        typename_of(args[2]));
                break;
            }
            std::string errors = "strict";
            if (args.size() == 4) {
                if (args[3]->type != T_string) {
                    yyerror("std.fs.write_text expects an errors string, got %s",
                            typename_of(args[3]));
                    break;
                }
                errors = ((struct string_value_t *)args[3])->value;
            }
            std::string data;
            std::string error;
            if (!encode_text(
                    ((struct string_value_t *)args[1])->value,
                    ((struct string_value_t *)args[2])->value, errors, data,
                    error)) {
                yyerror("std.fs.write_text: %s", error.c_str());
                break;
            }
            if (!platform_write_file(
                    thread_resolve_path(
                        ((struct string_value_t *)args[0])->value),
                    data, false, error)) {
                yyerror("std.fs.write_text: %s", error.c_str());
                break;
            }
            ret = newnull();
            break;
        }
        case B_read_file: {
            if (arg->type != T_string) {
                yyerror("read_file expects a string path, got %s",
                        typename_of(arg));
            } else {
                std::string data;
                std::string error;
                if (!platform_read_file(
                        thread_resolve_path(
                            ((struct string_value_t *)arg)->value),
                        data, error))
                    yyerror("read_file: %s", error.c_str());
                else if (data.find('\0') != std::string::npos)
                    yyerror("read_file: file contains NUL byte");
                else
                    ret = newstrval(data.c_str());
            }
            break;
        }
        case B_write_file:
        case B_append_file: {
            const char *name =
                functype == B_write_file ? "write_file" : "append_file";
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects a string path and data, got %s and %s",
                        name, typename_of(args[0]), typename_of(args[1]));
            } else {
                std::string error;
                if (!platform_write_file(
                        thread_resolve_path(
                            ((struct string_value_t *)args[0])->value),
                        ((struct string_value_t *)args[1])->value,
                        functype == B_append_file, error))
                    yyerror("%s: %s", name, error.c_str());
                else
                    ret = newnull();
            }
            break;
        }
        case B_file_exists:
            if (arg->type != T_string) {
                yyerror("file_exists expects a string path, got %s",
                        typename_of(arg));
            } else {
                ret = newintval(platform_file_exists(
                                    thread_resolve_path(
                                        ((struct string_value_t *)arg)
                                            ->value))
                                    ? 1
                                    : 0);
            }
            break;
        case B_file_open: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("file_open expects a string path and mode, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string mode_error;
            std::string mode = normalize_file_mode(
                ((struct string_value_t *)args[1])->value, mode_error);
            if (mode.empty()) {
                yyerror("file_open: %s", mode_error.c_str());
                break;
            }
            std::string error;
            std::string path = thread_resolve_path(
                ((struct string_value_t *)args[0])->value);
            platform_file_ptr file = platform_file_open(path, mode, error);
            if (!file)
                yyerror("file_open: %s", error.c_str());
            else
                ret = new_file_value(file, path);
            break;
        }
        case B_file_read: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_read"))
                break;
            size_t limit = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((struct int_value_t *)args[1])->value < 0) {
                    yyerror("file_read size must be a non-negative integer");
                    break;
                }
                limit = (size_t)((struct int_value_t *)args[1])->value;
            }
            std::string data;
            std::string error;
            if (!platform_file_read(file->file, limit, data, error)) {
                yyerror("file_read: %s", error.c_str());
            } else if (data.find('\0') != std::string::npos) {
                yyerror("file_read: file contains NUL byte");
            } else {
                ret = newstrval(data.c_str());
            }
            break;
        }
        case B_file_write: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_write"))
                break;
            if (args[1]->type != T_string) {
                yyerror("file_write expects string data, got %s",
                        typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_file_write(
                    file->file,
                    ((struct string_value_t *)args[1])->value, error))
                yyerror("file_write: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_seek: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_seek"))
                break;
            if (args[1]->type != T_int) {
                yyerror("file_seek offset must be an integer");
                break;
            }
            int whence = 0;
            if (args.size() == 3) {
                if (args[2]->type != T_int) {
                    yyerror("file_seek whence must be an integer");
                    break;
                }
                long long value = ((struct int_value_t *)args[2])->value;
                if (value < 0 || value > 2) {
                    yyerror("file_seek whence must be 0, 1, or 2");
                    break;
                }
                whence = (int)value;
            }
            std::string error;
            if (!platform_file_seek(
                    file->file,
                    ((struct int_value_t *)args[1])->value, whence, error))
                yyerror("file_seek: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_tell: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_tell"))
                break;
            std::string error;
            long long position = platform_file_tell(file->file, error);
            if (position < 0)
                yyerror("file_tell: %s", error.c_str());
            else
                ret = newintval(position);
            break;
        }
        case B_file_eof: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_eof"))
                break;
            if (!platform_file_is_open(file->file)) {
                yyerror("file_eof: file is closed");
                break;
            }
            ret = newintval(platform_file_eof(file->file) ? 1 : 0);
            break;
        }
        case B_file_close: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_close"))
                break;
            std::string error;
            platform_file_close(file->file, error);
            if (!error.empty())
                yyerror("file_close: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_flush: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_flush"))
                break;
            std::string error;
            if (!platform_file_flush(file->file, error))
                yyerror("file_flush: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_input: {
            const char *prompt = NULL;
            if (!args.empty()) {
                if (args[0]->type != T_string) {
                    yyerror("input prompt must be a string, got %s",
                            typename_of(args[0]));
                    break;
                }
                prompt = ((struct string_value_t *)args[0])->value;
            }
            std::string line;
            bool at_eof = false;
            if (!read_console_line(prompt, line, at_eof))
                break;
            if (at_eof && line.empty()) {
                ret = newnull();
            } else if (line.find('\0') != std::string::npos) {
                yyerror("input: line contains NUL byte");
            } else {
                ret = newstrval(line.c_str());
            }
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
