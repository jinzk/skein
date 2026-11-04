#include "unicode.h"

#include "symbol.h"
#include "builtin_ids.h"
#include "valuetype.h"

#include "utf8proc.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

enum class NormalizationForm {
    Nfc,
    Nfd,
    Nfkc,
    Nfkd,
};

static std::string normalize_form_name(const std::string &name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        normalized.push_back((char)c);
    }
    return normalized;
}

static bool parse_form(const std::string &name, NormalizationForm &form) {
    const std::string normalized = normalize_form_name(name);
    if (normalized == "nfc") {
        form = NormalizationForm::Nfc;
        return true;
    }
    if (normalized == "nfd") {
        form = NormalizationForm::Nfd;
        return true;
    }
    if (normalized == "nfkc") {
        form = NormalizationForm::Nfkc;
        return true;
    }
    if (normalized == "nfkd") {
        form = NormalizationForm::Nfkd;
        return true;
    }
    return false;
}

static utf8proc_option_t options_for(NormalizationForm form) {
    int options = UTF8PROC_STABLE;
    switch (form) {
        case NormalizationForm::Nfc:
            options |= UTF8PROC_COMPOSE;
            break;
        case NormalizationForm::Nfd:
            options |= UTF8PROC_DECOMPOSE;
            break;
        case NormalizationForm::Nfkc:
            options |= UTF8PROC_COMPOSE | UTF8PROC_COMPAT;
            break;
        case NormalizationForm::Nfkd:
            options |= UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT;
            break;
    }
    return (utf8proc_option_t)options;
}

static bool normalize_text(const std::string &input, NormalizationForm form,
                           std::string &output, std::string &error) {
    utf8proc_uint8_t *result = nullptr;
    const utf8proc_ssize_t length = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t *>(input.data()),
        (utf8proc_ssize_t)input.size(), &result, options_for(form));
    if (length < 0) {
        error = utf8proc_errmsg(length);
        return false;
    }
    if (length > 0)
        output.assign(reinterpret_cast<const char *>(result), (size_t)length);
    std::free(result);
    if (output.find('\0') != std::string::npos) {
        error = "normalized text contains NUL";
        return false;
    }
    return true;
}

static bool read_form_argument(const std::vector<struct value_t *> &args,
                               const char *name, NormalizationForm &form) {
    if (args[1]->type != T_string) {
        yyerror("%s expects a normalization form string, got %s", name,
                typename_of(args[1]));
        return false;
    }
    const std::string form_name =
        ((struct string_value_t *)args[1])->value;
    if (!parse_form(form_name, form)) {
        yyerror("%s: unknown normalization form \"%s\"", name,
                form_name.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool normalize_nfc(const std::string &input, std::string &output,
                   std::string &error) {
    return normalize_text(input, NormalizationForm::Nfc, output, error);
}

bool utf8_encode(unsigned int codepoint, std::string &out) {
    if (codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return false;
    if (codepoint <= 0x7F) {
        out.push_back((char)codepoint);
    } else if (codepoint <= 0x7FF) {
        out.push_back((char)(0xC0 | (codepoint >> 6)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back((char)(0xE0 | (codepoint >> 12)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (codepoint >> 18)));
        out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (codepoint & 0x3F)));
    }
    return true;
}

bool utf8_decode_one(const char *data, size_t size, size_t &consumed,
                     unsigned int &codepoint) {
    consumed = 0;
    codepoint = 0;
    if (!data || size == 0)
        return false;

    unsigned char c = (unsigned char)data[0];
    size_t length = 0;
    unsigned int value = 0;
    unsigned int minimum = 0;

    if (c < 0x80) {
        length = 1;
        value = c;
        minimum = 0;
    } else if ((c & 0xE0) == 0xC0) {
        length = 2;
        value = c & 0x1F;
        minimum = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
        value = c & 0x0F;
        minimum = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
        value = c & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }

    if (size < length)
        return false;
    for (size_t i = 1; i < length; ++i) {
        unsigned char next = (unsigned char)data[i];
        if ((next & 0xC0) != 0x80)
            return false;
        value = (value << 6) | (next & 0x3F);
    }

    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
        return false;

    consumed = length;
    codepoint = value;
    return true;
}

struct value_t *call_unicode_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype == B_unicode_version) {
        if (!args.empty()) {
            yyerror("std.unicode.version expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        return newstrval(utf8proc_unicode_version());
    }

    const bool is_normalize = functype == B_unicode_normalize;
    const char *name = is_normalize ? "std.unicode.normalize"
                                    : "std.unicode.is_normalized";
    if (args.size() != 2) {
        yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("%s expects a string, got %s", name, typename_of(args[0]));
        return NULL;
    }

    NormalizationForm form;
    if (!read_form_argument(args, name, form))
        return NULL;

    const std::string input = ((struct string_value_t *)args[0])->value;
    std::string normalized;
    std::string error;
    if (!normalize_text(input, form, normalized, error)) {
        yyerror("%s: %s", name, error.c_str());
        return NULL;
    }

    if (!is_normalize)
        return newboolval(input == normalized);
    return newstrval(normalized.c_str());
}
