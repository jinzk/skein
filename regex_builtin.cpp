#include "regex_builtin.h"

#include "ast.h"
#include "valuetype.h"

#include <regex>
#include <string>
#include <vector>

namespace {

bool build_regex(const std::string &pattern, const std::string &flags,
                 std::regex &result, std::string &error) {
    std::regex::flag_type options = std::regex::ECMAScript;
    for (char flag : flags) {
        if (flag == 'i') {
            options |= std::regex::icase;
        } else if (flag == 'm') {
            options |= std::regex_constants::multiline;
        } else {
            error = "unknown regex flag '" + std::string(1, flag) + "'";
            return false;
        }
    }
    try {
        result = std::regex(pattern, options);
    } catch (const std::regex_error &exc) {
        error = exc.what();
        return false;
    }
    return true;
}

struct value_t *match_dict(const std::smatch &match) {
    struct dict_value_t *dict = newdictval();
    dict->payload->items.push_back(std::make_pair(
        newstrval("text"), newstrval(match.str(0).c_str())));
    dict->payload->items.push_back(std::make_pair(
        newstrval("start"), newintval((long long)match.position(0))));
    dict->payload->items.push_back(std::make_pair(
        newstrval("end"),
        newintval((long long)(match.position(0) + match.length(0)))));

    struct list_value_t *groups = newlistval();
    for (size_t i = 1; i < match.size(); ++i) {
        if (match.length(i) >= 0)
            groups->payload->items.push_back(
                newstrval(match.str(i).c_str()));
        else
            groups->payload->items.push_back(newnull());
    }
    dict->payload->items.push_back(std::make_pair(
        newstrval("groups"), (value_t *)groups));
    return (value_t *)dict;
}

const char *regex_name(int functype) {
    switch (functype) {
        case B_regex_match: return "std.regex.match";
        case B_regex_search: return "std.regex.search";
        case B_regex_find_all: return "std.regex.find_all";
        case B_regex_replace: return "std.regex.replace";
        case B_regex_split: return "std.regex.split";
        default: return "std.regex";
    }
}

}  // namespace

struct value_t *call_regex_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    const char *name = regex_name(functype);

    size_t minimum = 2;
    size_t maximum = 3;
    if (functype == B_regex_find_all) {
        maximum = 4;
    } else if (functype == B_regex_replace) {
        minimum = 3;
        maximum = 4;
    } else if (functype == B_regex_split) {
        maximum = 3;
    }

    if (args.size() < minimum || args.size() > maximum) {
        yyerror("%s expects %d to %d arguments, got %d", name,
                (int)minimum, (int)maximum, (int)args.size());
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("%s expects a pattern string, got %s", name,
                typename_of(args[0]));
        return NULL;
    }
    if (args[1]->type != T_string) {
        yyerror("%s expects a text string, got %s", name,
                typename_of(args[1]));
        return NULL;
    }

    std::string flags;
    long long limit = 0;
    if (functype == B_regex_replace) {
        if (args[2]->type != T_string) {
            yyerror("%s expects a replacement string, got %s", name,
                    typename_of(args[2]));
            return NULL;
        }
        if (args.size() == 4) {
            if (args[3]->type != T_string) {
                yyerror("%s expects a flags string, got %s", name,
                        typename_of(args[3]));
                return NULL;
            }
            flags = ((struct string_value_t *)args[3])->value;
        }
    } else if (functype == B_regex_find_all && args.size() >= 3) {
        size_t flags_index = 2;
        if (args[2]->type == T_int) {
            if (args.size() != 3 ||
                ((struct int_value_t *)args[2])->value < 0) {
                yyerror("%s limit must be a non-negative integer", name);
                return NULL;
            }
            limit = ((struct int_value_t *)args[2])->value;
        } else {
            if (args[2]->type != T_string) {
                yyerror("%s expects a flags string or limit, got %s", name,
                        typename_of(args[2]));
                return NULL;
            }
            flags = ((struct string_value_t *)args[2])->value;
            flags_index = 3;
        }
        if (args.size() == 4) {
            if (flags_index != 3 || args[3]->type != T_int ||
                ((struct int_value_t *)args[3])->value < 0) {
                yyerror("%s limit must be a non-negative integer", name);
                return NULL;
            }
            limit = ((struct int_value_t *)args[3])->value;
        }
    } else if (args.size() >= 3) {
        if (args[2]->type != T_string) {
            yyerror("%s expects a flags string, got %s", name,
                    typename_of(args[2]));
            return NULL;
        }
        flags = ((struct string_value_t *)args[2])->value;
    }

    const std::string pattern = ((struct string_value_t *)args[0])->value;
    const std::string text = ((struct string_value_t *)args[1])->value;
    std::regex regex;
    std::string error;
    if (!build_regex(pattern, flags, regex, error)) {
        yyerror("%s: invalid pattern: %s", name, error.c_str());
        return NULL;
    }

    switch (functype) {
        case B_regex_match: {
            try {
                return newboolval(std::regex_match(text, regex));
            } catch (const std::regex_error &exc) {
                yyerror("%s: %s", name, exc.what());
                return NULL;
            }
        }
        case B_regex_search: {
            std::smatch match;
            try {
                if (!std::regex_search(text, match, regex))
                    return newnull();
            } catch (const std::regex_error &exc) {
                yyerror("%s: %s", name, exc.what());
                return NULL;
            }
            return match_dict(match);
        }
        case B_regex_find_all: {
            struct list_value_t *result = newlistval();
            try {
                for (std::sregex_iterator it(text.begin(), text.end(), regex),
                         end;
                     it != end; ++it) {
                    if (limit > 0 &&
                        (long long)result->payload->items.size() >= limit)
                        break;
                    result->payload->items.push_back(
                        match_dict(*it));
                }
            } catch (const std::regex_error &exc) {
                freeval((value_t *)result);
                yyerror("%s: %s", name, exc.what());
                return NULL;
            }
            return (value_t *)result;
        }
        case B_regex_replace: {
            const std::string replacement =
                ((struct string_value_t *)args[2])->value;
            try {
                return newstrval(
                    std::regex_replace(text, regex, replacement).c_str());
            } catch (const std::regex_error &exc) {
                yyerror("%s: %s", name, exc.what());
                return NULL;
            }
        }
        case B_regex_split: {
            struct list_value_t *result = newlistval();
            try {
                for (std::sregex_token_iterator
                         it(text.begin(), text.end(), regex, -1),
                         end;
                     it != end; ++it) {
                    result->payload->items.push_back(
                        newstrval(it->str().c_str()));
                }
            } catch (const std::regex_error &exc) {
                freeval((value_t *)result);
                yyerror("%s: %s", name, exc.what());
                return NULL;
            }
            return (value_t *)result;
        }
        default:
            yyerror("unknown regex function");
            return NULL;
    }
}
