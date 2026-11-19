#include "unicode.h"

#include "symbol.h"
#include "builtin_ids.h"
#include "valuetype.h"

#include "utf8proc.h"
#include "unicode_case_data.inc"
#include "unicode_quickcheck.inc"

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

enum class QuickCheck {
    Yes,
    No,
    Maybe,
};

template <std::size_t N>
QuickCheck lookup_quickcheck(const UnicodeQuickCheckRange (&ranges)[N],
                             uint32_t codepoint) {
    std::size_t low = 0;
    std::size_t high = N;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (ranges[middle].last < codepoint)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < N && ranges[low].first <= codepoint)
        return ranges[low].value == 1 ? QuickCheck::No : QuickCheck::Maybe;
    return QuickCheck::Yes;
}

QuickCheck codepoint_quickcheck(uint32_t codepoint, NormalizationForm form) {
    switch (form) {
        case NormalizationForm::Nfc:
            return lookup_quickcheck(NFC_QC_RANGES, codepoint);
        case NormalizationForm::Nfd:
            return lookup_quickcheck(NFD_QC_RANGES, codepoint);
        case NormalizationForm::Nfkc:
            return lookup_quickcheck(NFKC_QC_RANGES, codepoint);
        case NormalizationForm::Nfkd:
            return lookup_quickcheck(NFKD_QC_RANGES, codepoint);
    }
    return QuickCheck::Maybe;
}

QuickCheck quick_check_text(const std::string &input, NormalizationForm form) {
    bool maybe = false;
    std::size_t pos = 0;
    int last_combining_class = 0;
    while (pos < input.size()) {
        std::size_t consumed = 0;
        unsigned int codepoint = 0;
        if (!utf8_decode_one(input.data() + pos, input.size() - pos, consumed,
                             codepoint))
            return QuickCheck::Maybe;
        const QuickCheck value = codepoint_quickcheck(codepoint, form);
        if (value == QuickCheck::No)
            return QuickCheck::No;
        if (value == QuickCheck::Maybe)
            maybe = true;
        const utf8proc_property_t *property =
            utf8proc_get_property((utf8proc_int32_t)codepoint);
        const int combining_class = property ? property->combining_class : 0;
        if (combining_class != 0 && last_combining_class > combining_class)
            return QuickCheck::No;
        last_combining_class = combining_class;
        pos += consumed;
    }
    return maybe ? QuickCheck::Maybe : QuickCheck::Yes;
}

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
    if (input.find('\0') != std::string::npos) {
        error = "normalized text contains NUL";
        return false;
    }
    if (quick_check_text(input, form) == QuickCheck::Yes) {
        output = input;
        return true;
    }
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

template <std::size_t N>
bool in_case_property(const UnicodeCasePropertyRange (&ranges)[N],
                      uint32_t codepoint) {
    std::size_t low = 0;
    std::size_t high = N;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (ranges[middle].last < codepoint)
            low = middle + 1;
        else
            high = middle;
    }
    return low < N && ranges[low].first <= codepoint;
}

bool is_cased_codepoint(uint32_t codepoint) {
    return in_case_property(UNICODE_CASE_CASED, codepoint);
}

bool is_case_ignorable_codepoint(uint32_t codepoint) {
    return in_case_property(UNICODE_CASE_IGNORABLE, codepoint);
}

const UnicodeCaseSpecial *find_case_special(uint32_t codepoint) {
    const std::size_t count =
        sizeof(UNICODE_CASE_SPECIAL) / sizeof(UNICODE_CASE_SPECIAL[0]);
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (UNICODE_CASE_SPECIAL[middle].codepoint < codepoint)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < count && UNICODE_CASE_SPECIAL[low].codepoint == codepoint)
        return &UNICODE_CASE_SPECIAL[low];
    return nullptr;
}

/* Final_Sigma: the sigma is preceded by a cased letter (skipping
 * case-ignorable characters) and is not followed by one. */
bool final_sigma(const std::vector<uint32_t> &codepoints, std::size_t index) {
    bool preceded_by_cased = false;
    for (std::size_t i = index; i > 0; --i) {
        const uint32_t codepoint = codepoints[i - 1];
        if (is_case_ignorable_codepoint(codepoint))
            continue;
        preceded_by_cased = is_cased_codepoint(codepoint);
        break;
    }
    if (!preceded_by_cased)
        return false;
    for (std::size_t i = index + 1; i < codepoints.size(); ++i) {
        const uint32_t codepoint = codepoints[i];
        if (is_case_ignorable_codepoint(codepoint))
            continue;
        return !is_cased_codepoint(codepoint);
    }
    return true;
}

}  // namespace

bool case_map_text(const std::string &input, bool upper, std::string &output,
                   std::string &error) {
    if (input.find('\0') != std::string::npos) {
        error = "text contains NUL";
        return false;
    }
    std::vector<uint32_t> codepoints;
    codepoints.reserve(input.size());
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::size_t consumed = 0;
        unsigned int codepoint = 0;
        if (!utf8_decode_one(input.data() + offset, input.size() - offset,
                             consumed, codepoint)) {
            error = utf8proc_errmsg(UTF8PROC_ERROR_INVALIDUTF8);
            return false;
        }
        offset += consumed;
        codepoints.push_back(codepoint);
    }

    output.clear();
    output.reserve(input.size());
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const uint32_t codepoint = codepoints[index];
        const UnicodeCaseSpecial *special = find_case_special(codepoint);
        const uint32_t *mapping = nullptr;
        std::size_t length = 0;
        if (special) {
            mapping = upper ? special->upper : special->lower;
            length = upper ? special->upper_length : special->lower_length;
        }
        if (mapping && length > 0) {
            bool encoded = true;
            for (std::size_t i = 0; i < length && encoded; ++i)
                encoded = utf8_encode(mapping[i], output);
            if (!encoded) {
                error = "cannot encode case-mapped text";
                return false;
            }
            continue;
        }
        uint32_t mapped = codepoint;
        if (!upper && codepoint == 0x03A3 && final_sigma(codepoints, index))
            mapped = 0x03C2;
        else if (upper)
            mapped = (uint32_t)utf8proc_toupper((utf8proc_int32_t)codepoint);
        else
            mapped = (uint32_t)utf8proc_tolower((utf8proc_int32_t)codepoint);
        if (!utf8_encode(mapped, output)) {
            error = "cannot encode case-mapped text";
            return false;
        }
    }
    return true;
}

bool case_fold_text(const std::string &input, std::string &output,
                    std::string &error) {
    if (input.find('\0') != std::string::npos) {
        error = "text contains NUL";
        return false;
    }
    if (input.empty()) {
        output.clear();
        return true;
    }
    utf8proc_uint8_t *result = nullptr;
    const utf8proc_ssize_t length = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t *>(input.data()),
        (utf8proc_ssize_t)input.size(), &result,
        (utf8proc_option_t)UTF8PROC_CASEFOLD);
    if (length < 0) {
        error = utf8proc_errmsg(length);
        return false;
    }
    output.assign(reinterpret_cast<const char *>(result), (size_t)length);
    std::free(result);
    if (output.find('\0') != std::string::npos) {
        error = "folded text contains NUL";
        return false;
    }
    return true;
}

namespace {

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

    if (functype == B_unicode_casefold) {
        if (args.size() != 1) {
            yyerror("std.unicode.casefold expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        if (args[0]->type != T_string) {
            yyerror("std.unicode.casefold expects a string, got %s",
                    typename_of(args[0]));
            return NULL;
        }
        const std::string input =
            ((struct string_value_t *)args[0])->value;
        std::string folded;
        std::string error;
        if (!case_fold_text(input, folded, error)) {
            yyerror("std.unicode.casefold: %s", error.c_str());
            return NULL;
        }
        return newstrval(folded.c_str());
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
    if (input.find('\0') != std::string::npos) {
        yyerror("%s: normalized text contains NUL", name);
        return NULL;
    }
    if (!is_normalize) {
        const QuickCheck quick = quick_check_text(input, form);
        if (quick == QuickCheck::Yes)
            return newboolval(true);
        if (quick == QuickCheck::No)
            return newboolval(false);
    }
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
