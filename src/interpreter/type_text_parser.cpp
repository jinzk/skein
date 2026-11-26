#include "type_text_parser.h"

#include "static_type.h"
#include "symbol.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace skein {
namespace typecheck {
namespace {

class TypeTextParser {
public:
    TypeTextParser(std::string_view text, const char *source,
                   bool allow_named_generic)
        : text_(text),
          source_(source ? source : ""),
          allow_named_generic_(allow_named_generic) {}

    bool parse_list(std::vector<struct type_constraint *> &out,
                    std::string &error) {
        skip_space();
        if (at_end()) {
            error = "expected type argument";
            return false;
        }
        while (!at_end()) {
            struct type_constraint *type = NULL;
            if (!parse_type(type)) {
                error = error_;
                free_out(out);
                return false;
            }
            out.push_back(type);
            skip_space();
            if (at_end())
                break;
            if (!consume(','))
                return fail_and_free(out, error, "expected ',' or end of type list");
        }
        return true;
    }

private:
    bool at_end() const { return pos_ >= text_.size(); }

    char peek() const { return at_end() ? '\0' : text_[pos_]; }

    bool consume(char expected) {
        skip_space();
        if (peek() != expected)
            return false;
        ++pos_;
        return true;
    }

    void skip_space() {
        while (!at_end()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < text_.size() &&
                text_[pos_ + 1] == '/') {
                pos_ += 2;
                while (!at_end() && text_[pos_] != '\n')
                    ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < text_.size() &&
                text_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < text_.size() &&
                       !(text_[pos_] == '*' && text_[pos_ + 1] == '/'))
                    ++pos_;
                if (pos_ + 1 < text_.size())
                    pos_ += 2;
                continue;
            }
            break;
        }
    }

    static bool ident_start(unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c >= 0x80;
    }

    static bool ident_continue(unsigned char c) {
        return ident_start(c) || (c >= '0' && c <= '9');
    }

    bool parse_identifier(std::string &name) {
        skip_space();
        if (at_end() || !ident_start((unsigned char)text_[pos_]))
            return false;
        std::size_t start = pos_;
        while (!at_end() && ident_continue((unsigned char)text_[pos_]))
            ++pos_;
        name.assign(text_.substr(start, pos_ - start));
        return true;
    }

    bool match_word(const char *word) {
        skip_space();
        std::size_t length = std::strlen(word);
        if (pos_ + length > text_.size())
            return false;
        if (text_.substr(pos_, length) != word)
            return false;
        if (pos_ + length < text_.size() &&
            ident_continue((unsigned char)text_[pos_ + length]))
            return false;
        pos_ += length;
        return true;
    }

    bool consume_arrow() {
        skip_space();
        if (pos_ + 1 >= text_.size() ||
            text_[pos_] != '-' || text_[pos_ + 1] != '>')
            return false;
        pos_ += 2;
        return true;
    }

    bool parse_type(struct type_constraint *&out) {
        skip_space();
        if (peek() == '(')
            return parse_function_type(out);

        bool is_ref = false;
        bool is_weak = false;
        std::size_t save = pos_;
        if (match_word("ref"))
            is_ref = true;
        else if (match_word("weak"))
            is_weak = true;
        else
            pos_ = save;

        struct type_constraint *base = NULL;
        if (!parse_type_name(base))
            return false;
        if (is_ref)
            base = type_apply_prefix(TYPE_REF, base);
        else if (is_weak)
            base = type_apply_prefix(TYPE_WEAK, base);

        skip_space();
        while (!at_end() && peek() == '?') {
            ++pos_;
            base = type_apply_nullable(base);
            skip_space();
        }
        out = base;
        return true;
    }

    bool parse_function_type(struct type_constraint *&out) {
        if (!consume('('))
            return false;
        std::vector<struct type_constraint *> params;
        skip_space();
        if (!consume(')')) {
            while (true) {
                struct type_constraint *param = NULL;
                if (!parse_type(param)) {
                    free_types(params);
                    return false;
                }
                params.push_back(param);
                skip_space();
                if (consume(')'))
                    break;
                if (!consume(',')) {
                    free_types(params);
                    return fail("expected ',' or ')' in function type");
                }
            }
        }
        if (!consume_arrow()) {
            free_types(params);
            return fail("expected '->' in function type");
        }
        struct type_constraint *result = NULL;
        if (!parse_type(result)) {
            free_types(params);
            return false;
        }
        out = type_apply_function(params, result);
        return true;
    }

    bool parse_type_name(struct type_constraint *&out) {
        std::string first;
        if (!parse_identifier(first)) {
            return fail("expected type name");
        }

        std::string name = first;
        skip_space();
        while (peek() == '.') {
            ++pos_;
            std::string part;
            if (!parse_identifier(part))
                return fail("expected type name after '.'");
            name += ".";
            name += part;
            skip_space();
        }

        enum type_constraint_kind builtin = TYPE_INVALID;
        if (type_constraint_kind_for_builtin_name(name.c_str(), builtin))
            out = new_type_constraint(builtin);
        else
            out = new_named_type_constraint(name.c_str(), source_);

        skip_space();
        if (peek() != '[')
            return true;

        ++pos_;
        std::vector<struct type_constraint *> args;
        skip_space();
        if (consume(']')) {
            free_type_constraint(out);
            out = NULL;
            return fail("expected type argument after '['");
        }
        while (true) {
            struct type_constraint *arg = NULL;
            if (!parse_type(arg)) {
                free_type_constraint(out);
                free_types(args);
                return false;
            }
            args.push_back(arg);
            skip_space();
            if (consume(']'))
                break;
            if (!consume(',')) {
                free_type_constraint(out);
                free_types(args);
                return fail("expected ',' or ']' in type arguments");
            }
        }
        if (allow_named_generic_ && out->kind == TYPE_NAMED)
            out = type_apply_named_generic(out, args);
        else
            out = type_apply_generic_list(out, args);
        return true;
    }

    static void free_types(std::vector<struct type_constraint *> &types) {
        for (struct type_constraint *type : types)
            free_type_constraint(type);
        types.clear();
    }

    static void free_out(std::vector<struct type_constraint *> &out) {
        free_types(out);
    }

    bool fail_and_free(std::vector<struct type_constraint *> &out,
                       std::string &error, const char *message) {
        free_out(out);
        error = message;
        return false;
    }

    bool fail(const char *message) {
        if (error_.empty()) {
            error_ = message;
            error_pos_ = pos_;
        }
        return false;
    }

    std::string_view text_;
    const char *source_;
    bool allow_named_generic_ = false;
    std::size_t pos_ = 0;
    std::size_t error_pos_ = 0;
    std::string error_;
};

} // namespace

bool parse_type_list_text(const std::string &text, const char *source,
                          std::vector<struct type_constraint *> &out,
                          std::string &error, bool allow_named_generic) {
    TypeTextParser parser(text, source, allow_named_generic);
    return parser.parse_list(out, error);
}

} // namespace typecheck
} // namespace skein
