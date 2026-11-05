#include "builtins_internal.h"

static size_t utf8_length_prefix(const char *s, size_t bytes) {
    size_t len = std::strlen(s);
    if (bytes > len)
        bytes = len;
    size_t pos = 0;
    size_t count = 0;
    while (pos < bytes) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++count;
    }
    return count;
}

static std::string utf8_substr(const char *s, size_t start, size_t count) {
    size_t len = std::strlen(s);
    size_t pos = 0;
    size_t current = 0;
    while (pos < len && current < start) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++current;
    }
    size_t begin = pos;
    size_t taken = 0;
    while (pos < len && taken < count) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++taken;
    }
    return std::string(s + begin, pos - begin);
}

struct value_t *call_text_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_split: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("split expects string text and separator, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *separator = ((struct string_value_t *)args[1])->value;
            if (!separator[0]) {
                yyerror("split separator must not be empty");
                break;
            }
            std::string input = text;
            std::string sep = separator;
            struct list_value_t *parts = newlistval();
            size_t position = 0;
            for (;;) {
                size_t found = input.find(sep, position);
                if (found == std::string::npos) {
                    parts->payload->items.push_back(
                        newstrval(input.substr(position).c_str()));
                    break;
                }
                parts->payload->items.push_back(
                    newstrval(input.substr(position, found - position).c_str()));
                position = found + sep.size();
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_split_ws: {
            if (arg->type != T_string) {
                yyerror("split_ws expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            struct list_value_t *parts = newlistval();
            const char *p = text;
            while (*p) {
                while (*p && std::isspace((unsigned char)*p))
                    ++p;
                if (!*p)
                    break;
                const char *start = p;
                while (*p && !std::isspace((unsigned char)*p))
                    ++p;
                std::string part(start, (size_t)(p - start));
                parts->payload->items.push_back(newstrval(part.c_str()));
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_trim: {
            if (arg->type != T_string) {
                yyerror("trim expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            while (begin < end && std::isspace((unsigned char)*begin))
                ++begin;
            while (end > begin && std::isspace((unsigned char)end[-1]))
                --end;
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
        case B_find: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("find expects string text and substring, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            const char *found = std::strstr(text, needle);
            if (!found) {
                ret = newintval(-1);
            } else {
                ret = newintval((long long)utf8_length_prefix(
                    text, (size_t)(found - text)));
            }
            break;
        }
        case B_substr: {
            if (arg->type != T_string) {
                yyerror("substr expects a string, got %s", typename_of(arg));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("substr start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("substr length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            std::string part = utf8_substr(
                ((struct string_value_t *)arg)->value, start, count);
            ret = newstrval(part.c_str());
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
