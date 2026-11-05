#include "builtins_internal.h"

struct value_t *call_dict_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_dict_get:
        case B_dict_has: {
            if (args[0]->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_get ? "std.dict.get"
                                               : "std.dict.has",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)args[0];
            if (!dict_key_allowed(args[1])) {
                yyerror("%s: %s, got %s",
                        functype == B_dict_get ? "std.dict.get"
                                               : "std.dict.has",
                        dict_key_error(), typename_of(args[1]));
                break;
            }
            int found_index = dict_find_index(dict, args[1]);
            struct value_t *found =
                found_index >= 0
                    ? dict->payload->items[(size_t)found_index].second
                    : NULL;
            if (functype == B_dict_has) {
                ret = newintval(found ? 1 : 0);
            } else if (found) {
                ret = dupval(found);
            } else if (args.size() == 3) {
                ret = dupval(args[2]);
            } else {
                ret = newnull();
            }
            break;
        }
        case B_dict_keys:
        case B_dict_values: {
            if (arg->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_keys ? "std.dict.keys"
                                                : "std.dict.values",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items)
                out->payload->items.push_back(dupval(
                    functype == B_dict_keys ? entry.first : entry.second));
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_items: {
            if (arg->type != T_dict) {
                yyerror("std.dict.items expects a dict, got %s",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items) {
                struct list_value_t *pair = newlistval();
                pair->payload->items.push_back(dupval(entry.first));
                pair->payload->items.push_back(dupval(entry.second));
                out->payload->items.push_back((struct value_t *)pair);
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_delete: {
            if (args[0]->type != T_dict) {
                yyerror("std.dict.delete expects a dict, got %s",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *source = (struct dict_value_t *)args[0];
            if (!dict_key_allowed(args[1])) {
                yyerror("std.dict.delete: %s, got %s", dict_key_error(),
                        typename_of(args[1]));
                break;
            }
            int remove_index = dict_find_index(source, args[1]);
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(source->payload->items.size());
            for (size_t i = 0; i < source->payload->items.size(); ++i) {
                if ((int)i == remove_index)
                    continue;
                dict_append_item(out,
                                 dupval(source->payload->items[i].first),
                                 dupval(source->payload->items[i].second));
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_merge: {
            if (args[0]->type != T_dict || args[1]->type != T_dict) {
                yyerror("std.dict.merge expects two dicts");
                break;
            }
            struct dict_value_t *left = (struct dict_value_t *)args[0];
            struct dict_value_t *right = (struct dict_value_t *)args[1];
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(left->payload->items.size() +
                                        right->payload->items.size());
            for (auto &entry : left->payload->items) {
                if (!dict_key_allowed(entry.first)) {
                    yyerror("std.dict.merge: %s, got %s", dict_key_error(),
                            typename_of(entry.first));
                    freeval((struct value_t *)out);
                    return NULL;
                }
                dict_append_item(out, dupval(entry.first),
                                 dupval(entry.second));
            }
            for (auto &entry : right->payload->items) {
                if (!dict_key_allowed(entry.first)) {
                    yyerror("std.dict.merge: %s, got %s", dict_key_error(),
                            typename_of(entry.first));
                    freeval((struct value_t *)out);
                    return NULL;
                }
                int found = dict_find_index(out, entry.first);
                if (found >= 0) {
                    freeval(out->payload->items[(size_t)found].second);
                    out->payload->items[(size_t)found].second =
                        dupval(entry.second);
                } else {
                    dict_append_item(out, dupval(entry.first),
                                     dupval(entry.second));
                }
            }
            ret = (struct value_t *)out;
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
