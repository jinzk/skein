#include "formatter.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace skein {
namespace formatter {

namespace {

enum class TokenKind {
    Shebang,
    Comment,
    Newline,
    Whitespace,
    Word,
    Number,
    String,
    Char,
    Symbol,
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::Unknown;
    std::string raw;
    bool space_before = false;
};

struct Line {
    bool shebang = false;
    std::vector<Token> tokens;
};

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

bool is_ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_ident_start(unsigned char c) {
    return is_ascii_alpha(c) || c == '_' || c >= 0x80;
}

bool is_ident_continue(unsigned char c) {
    return is_ident_start(c) || is_digit(c);
}

bool starts_with(const std::string &text, std::size_t pos, const char *word) {
    std::size_t i = 0;
    while (word[i] && pos + i < text.size()) {
        if (text[pos + i] != word[i])
            return false;
        ++i;
    }
    return word[i] == '\0';
}

const char *const OPERATORS[] = {
    "<<=", ">>=", "...", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=",
    "|=",  "^=",  "|>",  "->", "=>", "::", "&&", "||", "<<", ">>", "<>",
    "!=",  "==",  ">=",  "<=", "..", "+",  "-",  "*",  "/",  "%",  "=",
    ",",   ":",   ";",   "&",  "|",  "^",  "~",  "!",  "@",  "?",  "$",
    "{",   "}",   "[",   "]",  "(",  ")",  "<",  ">",  ".",
};

bool consume_quoted(const std::string &source, std::size_t &pos, char quote,
                    std::string &error) {
    ++pos;
    while (pos < source.size()) {
        unsigned char c = (unsigned char)source[pos];
        if (c == '\\') {
            if (pos + 1 >= source.size()) {
                error = "unterminated escape sequence";
                return false;
            }
            pos += 2;
            continue;
        }
        if ((char)c == quote) {
            ++pos;
            return true;
        }
        if (c == '\n') {
            error = quote == '"' ? "unterminated string literal"
                                 : "unterminated character literal";
            return false;
        }
        ++pos;
    }
    error = quote == '"' ? "unterminated string literal at end of input"
                         : "unterminated character literal at end of input";
    return false;
}

std::size_t consume_number(const std::string &source, std::size_t pos) {
    const std::size_t start = pos;
    unsigned char c = (unsigned char)source[pos];
    if (c == '0' && pos + 1 < source.size()) {
        unsigned char prefix = (unsigned char)source[pos + 1];
        if (prefix == 'x' || prefix == 'X' || prefix == 'b' ||
            prefix == 'B' || prefix == 'o' || prefix == 'O') {
            pos += 2;
            while (pos < source.size()) {
                unsigned char d = (unsigned char)source[pos];
                if (is_digit(d) || d == '_' ||
                    (d >= 'A' && d <= 'F') || (d >= 'a' && d <= 'f')) {
                    ++pos;
                } else {
                    break;
                }
            }
            return pos;
        }
    }

    while (pos < source.size()) {
        unsigned char d = (unsigned char)source[pos];
        if (is_digit(d) || d == '_')
            ++pos;
        else
            break;
    }
    if (pos + 1 < source.size() && source[pos] == '.' &&
        is_digit((unsigned char)source[pos + 1])) {
        ++pos;
        while (pos < source.size()) {
            unsigned char d = (unsigned char)source[pos];
            if (is_digit(d) || d == '_')
                ++pos;
            else
                break;
        }
    }
    if (pos < source.size() &&
        (source[pos] == 'e' || source[pos] == 'E')) {
        std::size_t j = pos + 1;
        if (j < source.size() && (source[j] == '+' || source[j] == '-'))
            ++j;
        if (j < source.size() && is_digit((unsigned char)source[j])) {
            pos = j;
            while (pos < source.size()) {
                unsigned char d = (unsigned char)source[pos];
                if (is_digit(d) || d == '_')
                    ++pos;
                else
                    break;
            }
        }
    }
    return pos > start ? pos : start + 1;
}

bool tokenize(const std::string &source, std::vector<Token> &tokens,
              std::string &error) {
    tokens.clear();
    bool space_before = false;
    std::size_t pos = 0;
    while (pos < source.size()) {
        std::size_t start = pos;
        unsigned char c = (unsigned char)source[pos];

        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            ++pos;
            space_before = true;
            continue;
        }
        if (c == '\r') {
            if (pos + 1 < source.size() && source[pos + 1] == '\n') {
                tokens.push_back(
                    Token{TokenKind::Newline, source.substr(pos, 2), false});
                pos += 2;
            } else {
                tokens.push_back(
                    Token{TokenKind::Whitespace, source.substr(pos, 1), false});
                ++pos;
            }
            space_before = true;
            continue;
        }
        if (c == '\n') {
            tokens.push_back(
                Token{TokenKind::Newline, source.substr(pos, 1), false});
            ++pos;
            space_before = true;
            continue;
        }
        if (start == 0 && c == '#' && pos + 1 < source.size() &&
            source[pos + 1] == '!') {
            while (pos < source.size() && source[pos] != '\n' &&
                   source[pos] != '\r')
                ++pos;
            tokens.push_back(
                Token{TokenKind::Shebang, source.substr(start, pos - start),
                      false});
            space_before = true;
            continue;
        }
        if (c == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
            while (pos < source.size() && source[pos] != '\n' &&
                   source[pos] != '\r')
                ++pos;
            tokens.push_back(
                Token{TokenKind::Comment, source.substr(start, pos - start),
                      space_before});
            space_before = true;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (!consume_quoted(source, pos, (char)c, error))
                return false;
            tokens.push_back(Token{c == '"' ? TokenKind::String
                                             : TokenKind::Char,
                                   source.substr(start, pos - start),
                                   space_before});
            space_before = false;
            continue;
        }
        if (is_digit(c)) {
            pos = consume_number(source, pos);
            tokens.push_back(Token{TokenKind::Number,
                                   source.substr(start, pos - start),
                                   space_before});
            space_before = false;
            continue;
        }
        if (is_ident_start(c)) {
            ++pos;
            while (pos < source.size() &&
                   is_ident_continue((unsigned char)source[pos]))
                ++pos;
            tokens.push_back(Token{TokenKind::Word,
                                   source.substr(start, pos - start),
                                   space_before});
            space_before = false;
            continue;
        }

        const char *matched = NULL;
        for (const char *op : OPERATORS) {
            if (starts_with(source, pos, op)) {
                matched = op;
                break;
            }
        }
        if (matched) {
            pos += std::strlen(matched);
            tokens.push_back(Token{TokenKind::Symbol, matched, space_before});
        } else {
            ++pos;
            tokens.push_back(Token{TokenKind::Unknown,
                                   source.substr(start, 1), space_before});
        }
        space_before = false;
    }
    return true;
}

bool is_trivia(TokenKind kind) {
    return kind == TokenKind::Whitespace || kind == TokenKind::Newline ||
           kind == TokenKind::Comment || kind == TokenKind::Shebang;
}

bool is_open(const std::string &text) {
    return text == "(" || text == "[" || text == "{";
}

bool is_close(const std::string &text) {
    return text == ")" || text == "]" || text == "}";
}

bool is_atom(const Token &token) {
    return token.kind == TokenKind::Word ||
           token.kind == TokenKind::Number ||
           token.kind == TokenKind::String ||
           token.kind == TokenKind::Char || is_close(token.raw);
}

bool is_assignment(const std::string &text) {
    return text == "=" || text == "+=" || text == "-=" || text == "*=" ||
           text == "/=" || text == "%=" || text == "&=" || text == "|=" ||
           text == "^=" || text == "<<=" || text == ">>=";
}

bool is_pair_operator(const std::string &text) {
    return is_assignment(text) || text == "+" || text == "-" ||
           text == "*" || text == "/" || text == "%" || text == "&" ||
           text == "|" || text == "^" || text == "<<" || text == ">>" ||
           text == "==" || text == "!=" || text == "<>" || text == "<" ||
           text == ">" || text == "<=" || text == ">=" || text == "&&" ||
           text == "||" || text == "|>" || text == "=>" || text == "->";
}

bool is_prefix_candidate(const std::string &text) {
    return text == "+" || text == "-" || text == "!" || text == "~" ||
           text == "++" || text == "--" || text == "@" || text == "$";
}

bool is_join_word(const std::string &text) {
    return text == "else" || text == "while" || text == "in" ||
           text == "is" || text == "as" || text == "from";
}

bool is_control_keyword(const std::string &text) {
    return text == "if" || text == "else" || text == "while" ||
           text == "for" || text == "switch" || text == "match" ||
           text == "do" || text == "case";
}

bool should_space(const Token &prev, const Token &cur, bool expect_operand,
                  bool prev_unary, bool ternary_pending) {
    const std::string &p = prev.raw;
    const std::string &c = cur.raw;

    if (prev_unary)
        return false;
    if (c == "," || c == ";" || c == ")" || c == "]" || c == "." ||
        c == "::" || c == "++" || c == "--")
        return false;
    if (p == "(" || p == "[" || p == "." || p == "::" || p == "@" ||
        p == "$" || p == "..." || p == "..")
        return false;
    if (c == "(" && prev.kind == TokenKind::Word)
        return is_control_keyword(p);
    if (c == "[" &&
        (prev.kind == TokenKind::Word || prev.kind == TokenKind::Number ||
         p == ")" || p == "]"))
        return false;
    if (c == "?" || p == "?")
        return cur.space_before;
    if (c == ":")
        return ternary_pending;
    if (p == ":")
        return true;
    if (c == "{")
        return true;
    if (c == "}")
        return p == ";";
    if (p == "{")
        return c != "}";
    if (p == "," || p == ";")
        return true;
    if (is_close(p))
        return is_join_word(c) || is_pair_operator(c);
    if (is_pair_operator(c))
        return !expect_operand ? true : cur.space_before;
    if (is_pair_operator(p))
        return true;
    if (is_atom(prev) && is_atom(cur))
        return true;
    return cur.space_before;
}

std::string render_tokens(const std::vector<Token> &code) {
    std::string out;
    bool expect_operand = true;
    bool prev_unary = false;
    std::vector<bool> operand_stack;
    const Token *prev = NULL;
    bool ternary_pending = false;
    bool import_brace_pending = false;
    bool in_import_selection = false;

    for (std::size_t i = 0; i < code.size(); ++i) {
        const Token &cur = code[i];
        const bool compact_braces =
            in_import_selection && prev &&
            (prev->raw == "{" || cur.raw == "}");
        if (prev && !compact_braces &&
            should_space(*prev, cur, expect_operand, prev_unary,
                         ternary_pending))
            out.push_back(' ');
        out += cur.raw;

        if (cur.raw == "import" && i + 1 < code.size() &&
            code[i + 1].raw == "{") {
            import_brace_pending = true;
        } else if (import_brace_pending && cur.raw == "{") {
            in_import_selection = true;
            import_brace_pending = false;
        } else if (in_import_selection && cur.raw == "}") {
            in_import_selection = false;
        }

        if (cur.raw == "?") {
            ternary_pending = true;
        } else if (cur.raw == ":" && ternary_pending) {
            ternary_pending = false;
        } else if (cur.raw == ";" || cur.raw == "{" || cur.raw == "}") {
            ternary_pending = false;
        }

        if (is_open(cur.raw)) {
            operand_stack.push_back(expect_operand);
            expect_operand = true;
            prev_unary = false;
        } else if (is_close(cur.raw)) {
            expect_operand = false;
            if (!operand_stack.empty()) {
                expect_operand = operand_stack.back();
                operand_stack.pop_back();
            }
            prev_unary = false;
        } else if (cur.kind == TokenKind::Symbol &&
                   is_pair_operator(cur.raw)) {
            bool postfix = !expect_operand &&
                           (cur.raw == "++" || cur.raw == "--");
            prev_unary = expect_operand && is_prefix_candidate(cur.raw);
            expect_operand = !postfix;
        } else if (cur.kind == TokenKind::Symbol &&
                   is_prefix_candidate(cur.raw)) {
            prev_unary = expect_operand;
            expect_operand = true;
        } else {
            expect_operand = false;
            prev_unary = false;
        }
        prev = &cur;
    }
    return out;
}

std::vector<Line> split_lines(const std::vector<Token> &tokens) {
    std::vector<Line> lines;
    Line current;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token &token = tokens[i];
        switch (token.kind) {
            case TokenKind::Whitespace:
                break;
            case TokenKind::Newline:
                lines.push_back(current);
                current = Line{};
                break;
            case TokenKind::Shebang: {
                Line shebang_line;
                shebang_line.shebang = true;
                shebang_line.tokens.push_back(token);
                lines.push_back(shebang_line);
                break;
            }
            default:
                current.tokens.push_back(token);
                break;
        }
    }
    if (!current.tokens.empty())
        lines.push_back(current);
    return lines;
}

bool semantic_equal(const std::vector<Token> &a, const std::vector<Token> &b,
                    std::string &error) {
    std::vector<Token> filtered_a;
    std::vector<Token> filtered_b;
    std::vector<std::string> comments_a;
    std::vector<std::string> comments_b;
    std::vector<std::string> shebangs_a;
    std::vector<std::string> shebangs_b;

    for (const Token &token : a) {
        if (token.kind == TokenKind::Comment)
            comments_a.push_back(token.raw);
        else if (token.kind == TokenKind::Shebang)
            shebangs_a.push_back(token.raw);
        else if (!is_trivia(token.kind))
            filtered_a.push_back(token);
    }
    for (const Token &token : b) {
        if (token.kind == TokenKind::Comment)
            comments_b.push_back(token.raw);
        else if (token.kind == TokenKind::Shebang)
            shebangs_b.push_back(token.raw);
        else if (!is_trivia(token.kind))
            filtered_b.push_back(token);
    }

    if (filtered_a.size() != filtered_b.size()) {
        error = "formatter would change the token stream";
        return false;
    }
    for (std::size_t i = 0; i < filtered_a.size(); ++i) {
        if (filtered_a[i].kind != filtered_b[i].kind ||
            filtered_a[i].raw != filtered_b[i].raw) {
            error = "formatter would change token '" + filtered_a[i].raw +
                    "' to '" + filtered_b[i].raw + "'";
            return false;
        }
    }
    if (comments_a != comments_b || shebangs_a != shebangs_b) {
        error = "formatter would change comments or shebang";
        return false;
    }
    return true;
}

} // namespace

bool format_source(const std::string &source, std::string &formatted,
                   std::string &error) {
    std::vector<Token> tokens;
    if (!tokenize(source, tokens, error))
        return false;

    std::size_t crlf_count = 0;
    std::size_t lf_count = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '\n')
            continue;
        if (i > 0 && source[i - 1] == '\r')
            ++crlf_count;
        else
            ++lf_count;
    }
    const char *newline = crlf_count > 0 && crlf_count >= lf_count
                              ? "\r\n"
                              : "\n";

    std::vector<Line> lines = split_lines(tokens);
    std::vector<Line> normalized;
    for (Line &line : lines) {
        bool empty = line.tokens.empty();
        if (empty && (normalized.empty() || normalized.back().tokens.empty()))
            continue;
        normalized.push_back(line);
    }

    std::string output;
    int depth = 0;
    for (const Line &line : normalized) {
        if (line.shebang) {
            output += line.tokens.front().raw;
            output += newline;
            continue;
        }
        if (line.tokens.empty()) {
            output += newline;
            continue;
        }

        std::vector<Token> code;
        std::string comment;
        bool has_comment = false;
        for (const Token &token : line.tokens) {
            if (token.kind == TokenKind::Comment) {
                comment = token.raw;
                has_comment = true;
            } else {
                code.push_back(token);
            }
        }

        int leading_closers = 0;
        for (const Token &token : code) {
            if (!is_close(token.raw))
                break;
            ++leading_closers;
        }
        int line_indent = std::max(0, depth - leading_closers);
        int net = 0;
        for (const Token &token : code) {
            if (is_open(token.raw))
                ++net;
            else if (is_close(token.raw))
                --net;
        }
        depth = std::max(0, depth + net);

        output.append((std::size_t)line_indent * 2, ' ');
        output += render_tokens(code);
        if (has_comment) {
            if (!code.empty())
                output += "  ";
            output += comment;
        }
        output += newline;
    }

    std::vector<Token> output_tokens;
    std::string output_error;
    if (!tokenize(output, output_tokens, output_error)) {
        error = "formatted output cannot be tokenized: " + output_error;
        return false;
    }
    if (!semantic_equal(tokens, output_tokens, error))
        return false;

    formatted = output;
    return true;
}

} // namespace formatter
} // namespace skein
