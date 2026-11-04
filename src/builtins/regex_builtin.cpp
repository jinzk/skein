#include "regex_builtin.h"

#include "symbol.h"
#include "builtin_ids.h"
#include "valuetype.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct code_deleter {
    void operator()(pcre2_code *code) const {
        if (code)
            pcre2_code_free(code);
    }
};

struct match_data_deleter {
    void operator()(pcre2_match_data *data) const {
        if (data)
            pcre2_match_data_free(data);
    }
};

using code_ptr = std::unique_ptr<pcre2_code, code_deleter>;
using match_data_ptr = std::unique_ptr<pcre2_match_data, match_data_deleter>;

struct compiled_regex {
    code_ptr code;
    bool unicode = false;
};

std::string regex_error_message(int code) {
    PCRE2_UCHAR buffer[256];
    int length = pcre2_get_error_message(code, buffer, sizeof(buffer));
    if (length < 0)
        return "PCRE2 error " + std::to_string(code);
    return std::string((const char *)buffer, (size_t)length);
}

bool build_regex(const std::string &pattern, const std::string &flags,
                 compiled_regex &result, std::string &error) {
    uint32_t options = 0;
    bool unicode = false;
    for (char flag : flags) {
        if (flag == 'i') {
            options |= PCRE2_CASELESS;
        } else if (flag == 'm') {
            options |= PCRE2_MULTILINE;
        } else if (flag == 'u') {
            unicode = true;
        } else {
            error = "unknown regex flag '" + std::string(1, flag) + "'";
            return false;
        }
    }
    if (unicode)
        options |= PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C;

    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code *raw = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.data()),
        (PCRE2_SIZE)pattern.size(), options, &error_code, &error_offset,
        nullptr);
    if (!raw) {
        error = regex_error_message(error_code) + " at offset " +
                std::to_string((unsigned long long)error_offset);
        return false;
    }

    result.code.reset(raw);
    result.unicode = unicode;
    return true;
}

uint32_t capture_count(pcre2_code *code) {
    uint32_t count = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count);
    return count;
}

size_t utf8_advance(const std::string &text, size_t offset) {
    if (offset >= text.size())
        return text.size();
    unsigned char lead = (unsigned char)text[offset];
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0)
        length = 2;
    else if ((lead & 0xF0) == 0xE0)
        length = 3;
    else if ((lead & 0xF8) == 0xF0)
        length = 4;
    if (offset + length > text.size())
        length = 1;
    return offset + length;
}

size_t utf8_codepoint_offset(const std::string &text, size_t byte_offset) {
    size_t offset = 0;
    size_t codepoints = 0;
    while (offset < byte_offset && offset < text.size()) {
        offset = utf8_advance(text, offset);
        ++codepoints;
    }
    return codepoints;
}

struct value_t *match_dict(pcre2_match_data *data, const std::string &subject,
                           bool unicode, uint32_t groups) {
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data);
    size_t start = (size_t)ovector[0];
    size_t end = (size_t)ovector[1];

    struct dict_value_t *dict = newdictval();
    dict->payload->items.push_back(std::make_pair(
        newstrval("text"), newstrval(subject.substr(start, end - start).c_str())));
    dict->payload->items.push_back(std::make_pair(
        newstrval("start"),
        newintval((long long)(unicode ? utf8_codepoint_offset(subject, start)
                                      : start))));
    dict->payload->items.push_back(std::make_pair(
        newstrval("end"),
        newintval((long long)(unicode ? utf8_codepoint_offset(subject, end)
                                      : end))));

    struct list_value_t *captures = newlistval();
    for (uint32_t i = 1; i <= groups; ++i) {
        PCRE2_SIZE group_start = ovector[2 * i];
        if (group_start == PCRE2_UNSET) {
            captures->payload->items.push_back(newnull());
        } else {
            PCRE2_SIZE group_end = ovector[2 * i + 1];
            captures->payload->items.push_back(newstrval(
                subject.substr((size_t)group_start,
                               (size_t)(group_end - group_start))
                    .c_str()));
        }
    }
    dict->payload->items.push_back(
        std::make_pair(newstrval("groups"), (value_t *)captures));
    return (value_t *)dict;
}

template <typename Callback>
bool for_each_match(pcre2_code *code, pcre2_match_data *data,
                    const std::string &subject, bool unicode, long long limit,
                    std::string &error, Callback callback) {
    PCRE2_SPTR subject_ptr =
        reinterpret_cast<PCRE2_SPTR>(subject.data());
    size_t offset = 0;
    uint32_t options = 0;
    long long count = 0;

    for (;;) {
        if (limit > 0 && count >= limit)
            break;

        int rc = pcre2_match(code, subject_ptr, (PCRE2_SIZE)subject.size(),
                             (PCRE2_SIZE)offset, options, data, nullptr);
        if (rc == PCRE2_ERROR_NOMATCH) {
            if (options != 0) {
                if (offset >= subject.size())
                    break;
                offset = unicode ? utf8_advance(subject, offset) : offset + 1;
                options = 0;
                continue;
            }
            break;
        }
        if (rc < 0) {
            error = regex_error_message(rc);
            return false;
        }

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data);
        size_t match_start = (size_t)ovector[0];
        size_t match_end = (size_t)ovector[1];
        callback(match_start, match_end);
        ++count;

        if (match_end == match_start) {
            if (match_end >= subject.size())
                break;
            offset = match_end;
            options = PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
        } else {
            offset = match_end;
            options = 0;
        }
    }
    return true;
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
    compiled_regex regex;
    std::string error;
    if (!build_regex(pattern, flags, regex, error)) {
        yyerror("%s: invalid pattern: %s", name, error.c_str());
        return NULL;
    }

    match_data_ptr data(
        pcre2_match_data_create_from_pattern(regex.code.get(), nullptr));
    if (!data) {
        yyerror("%s: out of memory", name);
        return NULL;
    }

    switch (functype) {
        case B_regex_match: {
            uint32_t match_options = PCRE2_ANCHORED;
#ifdef PCRE2_ENDANCHORED
            match_options |= PCRE2_ENDANCHORED;
#endif
            int rc = pcre2_match(
                regex.code.get(),
                reinterpret_cast<PCRE2_SPTR>(text.data()),
                (PCRE2_SIZE)text.size(), 0, match_options, data.get(), nullptr);
            if (rc == PCRE2_ERROR_NOMATCH)
                return newboolval(false);
            if (rc < 0) {
                yyerror("%s: %s", name, regex_error_message(rc).c_str());
                return NULL;
            }
            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data.get());
            return newboolval(ovector[0] == 0 &&
                              (size_t)ovector[1] == text.size());
        }
        case B_regex_search: {
            int rc = pcre2_match(regex.code.get(),
                                 reinterpret_cast<PCRE2_SPTR>(text.data()),
                                 (PCRE2_SIZE)text.size(), 0, 0, data.get(),
                                 nullptr);
            if (rc == PCRE2_ERROR_NOMATCH)
                return newnull();
            if (rc < 0) {
                yyerror("%s: %s", name, regex_error_message(rc).c_str());
                return NULL;
            }
            return match_dict(data.get(), text, regex.unicode,
                              capture_count(regex.code.get()));
        }
        case B_regex_find_all: {
            struct list_value_t *result = newlistval();
            uint32_t groups = capture_count(regex.code.get());
            bool ok = for_each_match(
                regex.code.get(), data.get(), text, regex.unicode, limit,
                error, [&](size_t, size_t) {
                    result->payload->items.push_back(
                        match_dict(data.get(), text, regex.unicode, groups));
                });
            if (!ok) {
                freeval((value_t *)result);
                yyerror("%s: %s", name, error.c_str());
                return NULL;
            }
            return (value_t *)result;
        }
        case B_regex_replace: {
            const std::string replacement =
                ((struct string_value_t *)args[2])->value;
            uint32_t options = PCRE2_SUBSTITUTE_GLOBAL |
                               PCRE2_SUBSTITUTE_UNSET_EMPTY |
                               PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
            PCRE2_SIZE output_length = text.size() + replacement.size() + 32;
            std::vector<PCRE2_UCHAR> output(output_length + 1);
            int rc = pcre2_substitute(
                regex.code.get(),
                reinterpret_cast<PCRE2_SPTR>(text.data()),
                (PCRE2_SIZE)text.size(), 0, options, data.get(), nullptr,
                reinterpret_cast<PCRE2_SPTR>(replacement.data()),
                (PCRE2_SIZE)replacement.size(), output.data(),
                &output_length);
            if (rc == PCRE2_ERROR_NOMEMORY) {
                output.resize(output_length + 1);
                rc = pcre2_substitute(
                    regex.code.get(),
                    reinterpret_cast<PCRE2_SPTR>(text.data()),
                    (PCRE2_SIZE)text.size(), 0, options, data.get(), nullptr,
                    reinterpret_cast<PCRE2_SPTR>(replacement.data()),
                    (PCRE2_SIZE)replacement.size(), output.data(),
                    &output_length);
            }
            if (rc < 0) {
                yyerror("%s: %s", name,
                        regex_error_message(rc).c_str());
                return NULL;
            }
            return newstrval(std::string((const char *)output.data(),
                                         (size_t)output_length)
                                 .c_str());
        }
        case B_regex_split: {
            if (text.empty())
                return (value_t *)newlistval();
            struct list_value_t *result = newlistval();
            size_t last_end = 0;
            bool matched = false;
            bool ok = for_each_match(
                regex.code.get(), data.get(), text, regex.unicode, 0, error,
                [&](size_t start, size_t end) {
                    matched = true;
                    result->payload->items.push_back(
                        newstrval(text.substr(last_end, start - last_end)
                                      .c_str()));
                    last_end = end;
                });
            if (!ok) {
                freeval((value_t *)result);
                yyerror("%s: %s", name, error.c_str());
                return NULL;
            }
            if (!matched) {
                result->payload->items.push_back(newstrval(text.c_str()));
            } else if (last_end < text.size()) {
                result->payload->items.push_back(
                    newstrval(text.substr(last_end).c_str()));
            }
            return (value_t *)result;
        }
        default:
            yyerror("unknown regex function");
            return NULL;
    }
}
