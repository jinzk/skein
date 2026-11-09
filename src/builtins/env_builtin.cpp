#include "env_builtin.h"

#include "ast.h"
#include "platform.h"
#include "runtime_state.h"
#include "valuetype.h"

#include <string>
#include <utility>
#include <vector>

struct value_t *call_env_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
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
