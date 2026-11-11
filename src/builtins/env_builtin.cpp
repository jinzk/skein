#include "env_builtin.h"

#include "ast.h"
#include "platform.h"
#include "runtime_state.h"
#include "valuetype.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct value_t *call_env_builtin(
    int functype, const std::vector<struct value_t *> &args) {    switch (functype) {
        case B_env_get: {
            if (args.empty() || args.size() > 2) {
                yyerror("std.env.get expects 1 or 2 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_string) {
                yyerror("std.env.get expects a string name, got %s",
                        typename_of(args[0]));
                return NULL;
            }
            std::string value;
            if (thread_env_get(
                    ((struct string_value_t *)args[0])->value, value))
                return newstrval(value.c_str());
            if (args.size() == 2)
                return dupval(args[1]);
            return newnull();
        }
        case B_env_has: {
            if (args.size() != 1 || args[0]->type != T_string) {
                yyerror("std.env.has expects 1 string argument");
                return NULL;
            }
            std::string value;
            return newboolval(thread_env_get(
                ((struct string_value_t *)args[0])->value, value));
        }
        case B_env_set: {
            if (args.size() != 2 || args[0]->type != T_string ||
                args[1]->type != T_string) {
                yyerror("std.env.set expects 2 string arguments");
                return NULL;
            }
            const std::string &name =
                ((struct string_value_t *)args[0])->value;
            const std::string &value =
                ((struct string_value_t *)args[1])->value;
            if (!thread_env_valid_name(name) ||
                value.find('\0') != std::string::npos) {
                yyerror("std.env.set: invalid name or value");
                return NULL;
            }
            thread_env_set(name, value);
            return newnull();
        }
        case B_env_unset: {
            if (args.size() != 1 || args[0]->type != T_string) {
                yyerror("std.env.unset expects 1 string argument");
                return NULL;
            }
            const std::string &name =
                ((struct string_value_t *)args[0])->value;
            if (!thread_env_valid_name(name)) {
                yyerror("std.env.unset: invalid name");
                return NULL;
            }
            thread_env_unset(name);
            return newnull();
        }
        case B_env_all: {
            if (!args.empty()) {
                yyerror("std.env.all expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            std::vector<std::pair<std::string, std::string>> entries;
            thread_env_all(entries);
            struct dict_value_t *result = newdictval();
            for (const auto &entry : entries) {
                result->payload->items.push_back(std::make_pair(
                    newstrval(entry.first.c_str()),
                    newstrval(entry.second.c_str())));
            }
            return (value_t *)result;
        }
        default:
            yyerror("unknown environment function");
            return NULL;
    }
}

struct value_t *call_sys_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype == B_sys_exit) {
        if (args.size() > 1) {
            yyerror("std.sys.exit expects 0 or 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        long long code = 0;
        if (args.size() == 1) {
            if (args[0]->type != T_int) {
                yyerror("std.sys.exit expects an int exit code, got %s",
                        typename_of(args[0]));
                return NULL;
            }
            code = ((struct int_value_t *)args[0])->value;
            if (code < 0 || code > 255) {
                yyerror("std.sys.exit code must be between 0 and 255, got "
                        "%lld", code);
                return NULL;
            }
        }
        std::fflush(stdout);
        std::fflush(stderr);
        sys_exit_request((int)code);
        return newnull();
    }
    if (!args.empty()) {
        yyerror("std.sys functions expect 0 arguments, got %d",
                (int)args.size());
        return NULL;
    }
    switch (functype) {
        case B_sys_args: {
            struct list_value_t *result = newlistval();
            const std::vector<std::string> &values = sys_args();
            result->payload->items.reserve(values.size());
            for (const std::string &value : values)
                result->payload->items.push_back(newstrval(value.c_str()));
            return (value_t *)result;
        }
        case B_sys_script:
            return newstrval(sys_script().c_str());
        case B_sys_executable:
            return newstrval(sys_executable().c_str());
        default:
            yyerror("unknown sys function");
            return NULL;
    }
}
