#include "builtins_internal.h"

struct value_t *call_string_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_string_join: {
            if (args[0]->type != T_list || args[1]->type != T_string) {
                yyerror("std.string.join expects a list and a string separator");
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            const char *separator = ((struct string_value_t *)args[1])->value;
            std::string result;
            bool bad = false;
            for (size_t i = 0; i < list->payload->items.size(); ++i) {
                struct value_t *item = list->payload->items[i];
                if (item->type != T_string) {
                    yyerror("std.string.join expects string elements, got %s",
                            typename_of(item));
                    bad = true;
                    break;
                }
                if (i)
                    result += separator;
                result += ((struct string_value_t *)item)->value;
            }
            if (!bad)
                ret = newstrval(result.c_str());
            break;
        }
        case B_string_replace: {
            if (args[0]->type != T_string || args[1]->type != T_string ||
                args[2]->type != T_string) {
                yyerror("std.string.replace expects three strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *old_text = ((struct string_value_t *)args[1])->value;
            const char *new_text = ((struct string_value_t *)args[2])->value;
            if (!*old_text) {
                yyerror("std.string.replace search must not be empty");
                break;
            }
            std::string source(text);
            std::string old_value(old_text);
            std::string new_value(new_text);
            std::string result;
            size_t position = 0;
            for (;;) {
                size_t found = source.find(old_value, position);
                if (found == std::string::npos) {
                    result.append(source, position, std::string::npos);
                    break;
                }
                result.append(source, position, found - position);
                result += new_value;
                position = found + old_value.size();
            }
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_starts_with:
        case B_string_ends_with: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects two strings",
                        functype == B_string_starts_with
                            ? "std.string.starts_with"
                            : "std.string.ends_with");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *part = ((struct string_value_t *)args[1])->value;
            size_t text_len = std::strlen(text);
            size_t part_len = std::strlen(part);
            bool match = part_len <= text_len;
            if (match && functype == B_string_starts_with)
                match = std::memcmp(text, part, part_len) == 0;
            else if (match)
                match = std::memcmp(text + text_len - part_len, part,
                                    part_len) == 0;
            ret = newintval(match ? 1 : 0);
            break;
        }
        case B_string_contains: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("std.string.contains expects two strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            ret = newintval(std::strstr(text, needle) ? 1 : 0);
            break;
        }
        case B_string_repeat: {
            if (args[0]->type != T_string || args[1]->type != T_int) {
                yyerror("std.string.repeat expects a string and an integer");
                break;
            }
            long long count = ((struct int_value_t *)args[1])->value;
            if (count < 0) {
                yyerror("std.string.repeat count must be non-negative");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            std::string result;
            size_t length = std::strlen(text);
            result.reserve(length * (size_t)count);
            for (long long i = 0; i < count; ++i)
                result += text;
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_trim_start:
        case B_string_trim_end: {
            if (arg->type != T_string) {
                yyerror("%s expects a string",
                        functype == B_string_trim_start
                            ? "std.string.trim_start"
                            : "std.string.trim_end");
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            if (functype == B_string_trim_start) {
                while (begin < end && std::isspace((unsigned char)*begin))
                    ++begin;
            } else {
                while (end > begin && std::isspace((unsigned char)end[-1]))
                    --end;
            }
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
