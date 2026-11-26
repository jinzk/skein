#include "type_descriptor.h"

#include <cctype>
#include <cstring>

namespace {

class descriptor_parser {
public:
    descriptor_parser(const std::string &text, std::string &error)
        : text_(text), error_(error) {}

    bool parse(type_descriptor &out) {
        skip_space();
        if (!parse_type(out))
            return false;
        skip_space();
        if (pos_ != text_.size())
            return fail("unexpected trailing characters in type descriptor");
        return true;
    }

private:
    bool parse_type(type_descriptor &out) {
        skip_space();
        out = type_descriptor();
        if (peek() == '(')
            return fail("function types are not supported in type descriptors");
        if (!parse_name(out.name, out.package))
            return false;
        skip_space();
        if (consume('[')) {
            skip_space();
            if (consume(']'))
                return fail("empty type argument list");
            for (;;) {
                type_descriptor_ptr arg = std::make_shared<type_descriptor>();
                if (!parse_type(*arg))
                    return false;
                out.args.push_back(std::move(arg));
                skip_space();
                if (consume(']'))
                    break;
                if (!consume(','))
                    return fail("expected ',' or ']' in type descriptor");
            }
        }
        skip_space();
        if (consume('?'))
            out.nullable = true;
        return true;
    }

    bool parse_name(std::string &name, std::string &package) {
        skip_space();
        std::string path;
        if (!parse_identifier(path))
            return false;
        while (true) {
            skip_space();
            if (peek() != '.')
                break;
            ++pos_;
            skip_space();
            std::string part;
            if (!parse_identifier(part))
                return false;
            path += '.';
            path += part;
        }
        std::size_t dot = path.rfind('.');
        if (dot == std::string::npos) {
            name = path;
            package.clear();
        } else {
            package = path.substr(0, dot);
            name = path.substr(dot + 1);
        }
        return true;
    }

    bool parse_identifier(std::string &out) {
        if (pos_ >= text_.size())
            return fail("expected a type name");
        unsigned char first = (unsigned char)text_[pos_];
        if (!(std::isalpha(first) || first == '_'))
            return fail("expected a type name");
        std::size_t start = pos_;
        ++pos_;
        while (pos_ < text_.size()) {
            unsigned char c = (unsigned char)text_[pos_];
            if (!(std::isalnum(c) || c == '_'))
                break;
            ++pos_;
        }
        out.assign(text_, start, pos_ - start);
        if (out == "func" || out == "function" || out == "callable") {
            /* Handled by parse_type's function-type rejection only when the
             * descriptor also has a signature.  A bare function name is
             * still not a serializable target. */
        }
        return true;
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    bool consume(char c) {
        if (peek() != c)
            return false;
        ++pos_;
        return true;
    }

    bool fail(const char *message) {
        if (error_.empty()) {
            error_ = message;
            error_ += " at offset ";
            error_ += std::to_string(pos_);
        }
        return false;
    }

    const std::string &text_;
    std::string &error_;
    std::size_t pos_ = 0;
};

void append_descriptor(const type_descriptor &desc, std::string &out) {
    if (!desc.package.empty()) {
        out += desc.package;
        out += '.';
    }
    out += desc.name;
    if (!desc.args.empty()) {
        out += '[';
        for (std::size_t i = 0; i < desc.args.size(); ++i) {
            if (i != 0)
                out += ", ";
            append_descriptor(*desc.args[i], out);
        }
        out += ']';
    }
    if (desc.nullable)
        out += '?';
}

} // namespace

bool parse_type_descriptor(const std::string &text, type_descriptor &out,
                           std::string &error) {
    error.clear();
    descriptor_parser parser(text, error);
    return parser.parse(out);
}

std::string type_descriptor_display(const type_descriptor &desc) {
    std::string out;
    append_descriptor(desc, out);
    return out;
}
