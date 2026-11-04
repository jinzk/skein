#include "ffi_bindgen.h"

#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace skein {
namespace {

namespace fs = std::filesystem;

/* -------------------------------------------------------------------------
 * Options
 * ---------------------------------------------------------------------- */

struct options {
    std::vector<std::string> headers;
    std::vector<std::string> includes;
    std::vector<std::string> defines;
    std::vector<std::string> symbols;
    std::string module;
    std::string cc;
    std::string output;
    bool emit_constants = true;
    bool check = false;
    bool tolerate_errors = false;
};

struct constant_out {
    enum kind_t { INT, FLOAT, STRING } kind = INT;
    long long i = 0;
    double f = 0.0;
    std::string s;
};

/* Widths reported by the selected C compiler.  These are used instead of the
 * host's sizeof() so that --cc/-m32/sysroot produce target-correct manifests. */
struct abi_info {
    int short_bytes = (int)sizeof(short);
    int int_bytes = (int)sizeof(int);
    int long_bytes = (int)sizeof(long);
    int long_long_bytes = (int)sizeof(long long);
    int pointer_bytes = (int)sizeof(void *);
    int wchar_bytes = 4;
    bool char_unsigned = false;
    bool wchar_unsigned = false;
};

/* -------------------------------------------------------------------------
 * Tokens
 * ---------------------------------------------------------------------- */

enum token_kind { TOK_IDENT = 0, TOK_NUMBER, TOK_STRING, TOK_PUNCT };

struct token {
    std::string text;
    int kind = TOK_PUNCT;
    int pack = 0;
};

bool is_ident_char(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$';
}

void tokenize_line(const std::string &line, int pack,
                   std::vector<token> &out) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        unsigned char c = (unsigned char)line[i];
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        token t;
        t.pack = pack;
        if (std::isalpha(c) || c == '_' || c == '$') {
            size_t start = i;
            while (i < n && is_ident_char((unsigned char)line[i]))
                ++i;
            t.kind = TOK_IDENT;
            t.text = line.substr(start, i - start);
        } else if (std::isdigit(c) ||
                   (c == '.' && i + 1 < n &&
                    std::isdigit((unsigned char)line[i + 1]))) {
            size_t start = i;
            while (i < n) {
                unsigned char d = (unsigned char)line[i];
                if (std::isalnum(d) || d == '.' || d == '\'') {
                    ++i;
                    continue;
                }
                if ((d == '+' || d == '-') && i > start &&
                    (line[i - 1] == 'e' || line[i - 1] == 'E' ||
                     line[i - 1] == 'p' || line[i - 1] == 'P')) {
                    ++i;
                    continue;
                }
                break;
            }
            t.kind = TOK_NUMBER;
            t.text = line.substr(start, i - start);
        } else if (c == '"' || c == '\'') {
            char quote = (char)c;
            size_t start = i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            t.kind = TOK_STRING;
            t.text = line.substr(start, i - start);
        } else if (line.compare(i, 3, "...") == 0) {
            t.text = "...";
            i += 3;
        } else if (line.compare(i, 2, "<<") == 0 ||
                   line.compare(i, 2, ">>") == 0 ||
                   line.compare(i, 2, "==") == 0 ||
                   line.compare(i, 2, "!=") == 0 ||
                   line.compare(i, 2, "<=") == 0 ||
                   line.compare(i, 2, ">=") == 0 ||
                   line.compare(i, 2, "&&") == 0 ||
                   line.compare(i, 2, "||") == 0) {
            t.text = line.substr(i, 2);
            i += 2;
        } else {
            t.text.assign(1, (char)c);
            ++i;
        }
        out.push_back(std::move(t));
    }
}

/* -------------------------------------------------------------------------
 * Preprocessing
 * ---------------------------------------------------------------------- */

std::string trim(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace((unsigned char)text[begin]))
        ++begin;
    size_t end = text.size();
    while (end > begin && std::isspace((unsigned char)text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::string canonical_path(const std::string &path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(fs::path(path), ec);
    if (ec)
        return path;
    return result.string();
}

std::string shell_quote(const std::string &value) {
#if defined(_WIN32)
    if (value.find_first_of(" \t\"") == std::string::npos)
        return value;
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"')
            quoted += "\\\"";
        else
            quoted += c;
    }
    quoted += '"';
    return quoted;
#else
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += '\'';
    return quoted;
#endif
}

bool parse_pack_directive(const std::string &line, int &pack,
                          std::vector<int> &stack) {
    size_t open = line.find('(');
    if (open == std::string::npos) {
        pack = 0;
        return true;
    }
    size_t close = line.rfind(')');
    std::string args =
        close == std::string::npos || close <= open
            ? std::string()
            : line.substr(open + 1, close - open - 1);
    args = trim(args);
    if (args.empty()) {
        pack = 0;
        return true;
    }
    std::string first = args;
    std::string second;
    size_t comma = args.find(',');
    if (comma != std::string::npos) {
        first = trim(args.substr(0, comma));
        second = trim(args.substr(comma + 1));
    }
    auto number = [](const std::string &text) {
        if (text.empty())
            return 0;
        return (int)std::strtol(text.c_str(), NULL, 10);
    };
    if (first == "push") {
        stack.push_back(pack);
        if (!second.empty())
            pack = number(second);
        return true;
    }
    if (first == "pop") {
        if (!stack.empty()) {
            pack = stack.back();
            stack.pop_back();
        } else {
            pack = 0;
        }
        return true;
    }
    pack = number(first);
    return true;
}


/* Small integer constant-expression evaluator for #define/enum values. */
bool is_type_keyword(const std::string &word);

class integer_expression {
public:
    integer_expression(const std::string &text,
                       const std::map<std::string, long long> &names,
                       const abi_info &abi)
        : text_(text), names_(names), abi_(abi) {}

    bool evaluate(long long &out) {
        out = parse_conditional();
        skip();
        return ok_ && position_ == text_.size();
    }

private:
    void skip() {
        while (position_ < text_.size() &&
               std::isspace((unsigned char)text_[position_]))
            ++position_;
    }

    bool eat(char c) {
        skip();
        if (position_ < text_.size() && text_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    bool eat(const char *text) {
        skip();
        size_t length = std::strlen(text);
        if (text_.compare(position_, length, text) == 0) {
            position_ += length;
            return true;
        }
        return false;
    }

    long long apply_cast(int bytes, bool is_unsigned_type, long long value) {
        if (bytes >= 8)
            return value;
        const int bits = bytes * 8;
        unsigned long long mask = (1ULL << bits) - 1;
        unsigned long long narrowed = (unsigned long long)value & mask;
        if (!is_unsigned_type && (narrowed & (1ULL << (bits - 1))))
            narrowed |= ~mask;
        return (long long)narrowed;
    }

    /* Parse the type name of a simple scalar cast.  The caller consumes the
     * closing parenthesis. */
    bool parse_cast_type(int &bytes, bool &is_unsigned_type) {
        skip();
        const size_t save = position_;
        bool has_unsigned = false;
        bool has_signed = false;
        bool has_short = false;
        bool has_char = false;
        bool has_bool = false;
        bool has_float = false;
        bool has_double = false;
        bool has_void = false;
        bool has_complex = false;
        int longs = 0;
        bool any = false;
        for (;;) {
            skip();
            if (position_ >= text_.size() ||
                (!std::isalpha((unsigned char)text_[position_]) &&
                 text_[position_] != '_'))
                break;
            const size_t start = position_;
            while (position_ < text_.size() &&
                   is_ident_char((unsigned char)text_[position_]))
                ++position_;
            std::string word = text_.substr(start, position_ - start);
            if (!is_type_keyword(word)) {
                position_ = save;
                return false;
            }
            any = true;
            if (word == "unsigned" || word == "__unsigned__")
                has_unsigned = true;
            else if (word == "signed" || word == "__signed__")
                has_signed = true;
            else if (word == "short")
                has_short = true;
            else if (word == "char")
                has_char = true;
            else if (word == "long")
                ++longs;
            else if (word == "_Bool" || word == "bool")
                has_bool = true;
            else if (word == "float")
                has_float = true;
            else if (word == "double")
                has_double = true;
            else if (word == "void")
                has_void = true;
            else if (word == "_Complex")
                has_complex = true;
        }
        skip();
        if (!any || position_ >= text_.size() || text_[position_] != ')' ||
            has_float || has_double || has_void || has_complex) {
            position_ = save;
            return false;
        }
        if (has_char || has_bool)
            bytes = 1;
        else if (has_short)
            bytes = abi_.short_bytes;
        else if (longs >= 2)
            bytes = abi_.long_long_bytes;
        else if (longs == 1)
            bytes = abi_.long_bytes;
        else
            bytes = abi_.int_bytes;
        is_unsigned_type = has_unsigned && !has_signed;
        return true;
    }

    long long parse_conditional() {
        long long condition = parse_logical_or();
        if (ok_ && eat('?')) {
            long long yes = parse_conditional();
            if (!eat(':')) {
                ok_ = false;
                return 0;
            }
            long long no = parse_conditional();
            return condition ? yes : no;
        }
        return condition;
    }

    long long parse_logical_or() {
        long long value = parse_logical_and();
        while (ok_ && eat("||")) {
            long long right = parse_logical_and();
            value = (value != 0 || right != 0) ? 1 : 0;
        }
        return value;
    }

    long long parse_logical_and() {
        long long value = parse_bitor();
        while (ok_ && eat("&&")) {
            long long right = parse_bitor();
            value = (value != 0 && right != 0) ? 1 : 0;
        }
        return value;
    }

    bool eat_bitor() {
        skip();
        if (text_.compare(position_, 2, "||") == 0)
            return false;
        return eat('|');
    }

    bool eat_bitand() {
        skip();
        if (text_.compare(position_, 2, "&&") == 0)
            return false;
        return eat('&');
    }

    long long parse_bitor() {
        long long value = parse_xor();
        while (ok_ && eat_bitor())
            value |= parse_xor();
        return value;
    }

    long long parse_xor() {
        long long value = parse_bitand();
        while (ok_ && eat('^'))
            value ^= parse_bitand();
        return value;
    }

    long long parse_bitand() {
        long long value = parse_equality();
        while (ok_ && eat_bitand())
            value &= parse_equality();
        return value;
    }

    long long parse_equality() {
        long long value = parse_relational();
        for (;;) {
            if (ok_ && eat("=="))
                value = value == parse_relational() ? 1 : 0;
            else if (ok_ && eat("!="))
                value = value != parse_relational() ? 1 : 0;
            else
                break;
        }
        return value;
    }

    long long parse_relational() {
        long long value = parse_shift();
        for (;;) {
            if (ok_ && eat("<="))
                value = value <= parse_shift() ? 1 : 0;
            else if (ok_ && eat(">="))
                value = value >= parse_shift() ? 1 : 0;
            else if (ok_ && eat('<'))
                value = value < parse_shift() ? 1 : 0;
            else if (ok_ && eat('>'))
                value = value > parse_shift() ? 1 : 0;
            else
                break;
        }
        return value;
    }

    long long parse_shift() {
        long long value = parse_add();
        for (;;) {
            skip();
            if (text_.compare(position_, 2, "<<") == 0) {
                position_ += 2;
                value = (long long)((unsigned long long)value
                                    << (parse_add() & 63));
            } else if (text_.compare(position_, 2, ">>") == 0) {
                position_ += 2;
                value = value >> (parse_add() & 63);
            } else {
                break;
            }
        }
        return value;
    }

    long long parse_add() {
        long long value = parse_mul();
        for (;;) {
            if (eat('+'))
                value += parse_mul();
            else if (eat('-'))
                value -= parse_mul();
            else
                break;
        }
        return value;
    }

    long long parse_mul() {
        long long value = parse_unary();
        for (;;) {
            if (eat('*')) {
                value *= parse_unary();
            } else if (eat('/')) {
                long long divisor = parse_unary();
                if (divisor == 0) {
                    ok_ = false;
                    return 0;
                }
                value /= divisor;
            } else if (eat('%')) {
                long long divisor = parse_unary();
                if (divisor == 0) {
                    ok_ = false;
                    return 0;
                }
                value %= divisor;
            } else {
                break;
            }
        }
        return value;
    }

    long long parse_unary() {
        if (eat('+'))
            return parse_unary();
        if (eat('-'))
            return -parse_unary();
        if (eat('~'))
            return ~parse_unary();
        if (eat('!'))
            return parse_unary() == 0 ? 1 : 0;
        return parse_primary();
    }

    long long parse_primary() {
        skip();
        if (position_ >= text_.size()) {
            ok_ = false;
            return 0;
        }
        if (eat('(')) {
            const size_t save = position_;
            int cast_bytes = 0;
            bool cast_unsigned = false;
            if (parse_cast_type(cast_bytes, cast_unsigned)) {
                if (!eat(')')) {
                    ok_ = false;
                    return 0;
                }
                return apply_cast(cast_bytes, cast_unsigned, parse_unary());
            }
            position_ = save;
            long long value = parse_conditional();
            if (!eat(')'))
                ok_ = false;
            return value;
        }
        if (text_[position_] == '\'') {
            ++position_;
            long long value = 0;
            while (position_ < text_.size() && text_[position_] != '\'') {
                char c = text_[position_++];
                if (c == '\\' && position_ < text_.size()) {
                    char next = text_[position_++];
                    switch (next) {
                        case 'n': value = '\n'; break;
                        case 'r': value = '\r'; break;
                        case 't': value = '\t'; break;
                        case '0': value = 0; break;
                        case '\\': value = '\\'; break;
                        case '\'': value = '\''; break;
                        default: value = (unsigned char)next; break;
                    }
                } else {
                    value = (unsigned char)c;
                }
            }
            if (!eat('\''))
                ok_ = false;
            return value;
        }
        if (text_.compare(position_, 2, "0b") == 0 ||
            text_.compare(position_, 2, "0B") == 0) {
            position_ += 2;
            long long value = 0;
            while (position_ < text_.size() &&
                   (text_[position_] == '0' || text_[position_] == '1')) {
                value = (value << 1) | (text_[position_] - '0');
                ++position_;
            }
            while (position_ < text_.size() &&
                   (text_[position_] == 'u' || text_[position_] == 'U' ||
                    text_[position_] == 'l' || text_[position_] == 'L'))
                ++position_;
            return value;
        }
        if (std::isdigit((unsigned char)text_[position_])) {
            size_t start = position_;
            if (text_.compare(position_, 2, "0x") == 0 ||
                text_.compare(position_, 2, "0X") == 0) {
                position_ += 2;
                while (position_ < text_.size() &&
                       std::isxdigit((unsigned char)text_[position_]))
                    ++position_;
            } else {
                while (position_ < text_.size() &&
                       std::isdigit((unsigned char)text_[position_]))
                    ++position_;
            }
            std::string digits = text_.substr(start, position_ - start);
            while (position_ < text_.size() &&
                   (text_[position_] == 'u' || text_[position_] == 'U' ||
                    text_[position_] == 'l' || text_[position_] == 'L'))
                ++position_;
            char *end = NULL;
            long long value = std::strtoll(digits.c_str(), &end, 0);
            if (!end || *end != '\0') {
                ok_ = false;
                return 0;
            }
            return value;
        }
        if (std::isalpha((unsigned char)text_[position_]) ||
            text_[position_] == '_') {
            size_t start = position_;
            while (position_ < text_.size() &&
                   is_ident_char((unsigned char)text_[position_]))
                ++position_;
            std::string name = text_.substr(start, position_ - start);
            auto it = names_.find(name);
            if (it == names_.end()) {
                ok_ = false;
                return 0;
            }
            return it->second;
        }
        ok_ = false;
        return 0;
    }

    const std::string &text_;
    const std::map<std::string, long long> &names_;
    const abi_info &abi_;
    size_t position_ = 0;
    bool ok_ = true;
};

bool parse_constant_text(const std::string &text,
                             const std::map<std::string, long long> &names,
                             const abi_info &abi, constant_out &out) {
    std::string value = trim(text);
    if (value.empty())
        return false;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        out.kind = constant_out::STRING;
        out.s.clear();
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            char c = value[i];
            if (c == '\\' && i + 2 < value.size()) {
                char next = value[++i];
                switch (next) {
                    case 'n': out.s += '\n'; break;
                    case 'r': out.s += '\r'; break;
                    case 't': out.s += '\t'; break;
                    case '\\': out.s += '\\'; break;
                    case '"': out.s += '"'; break;
                    default: out.s += next; break;
                }
            } else {
                out.s += c;
            }
        }
        return true;
    }
    long long integer = 0;
    integer_expression expression(value, names, abi);
    if (expression.evaluate(integer)) {
        out.kind = constant_out::INT;
        out.i = integer;
        return true;
    }
    std::string float_text = value;
    bool has_float_marker = float_text.find('.') != std::string::npos ||
                            float_text.find('e') != std::string::npos ||
                            float_text.find('E') != std::string::npos ||
                            float_text.find('p') != std::string::npos ||
                            float_text.find('P') != std::string::npos;
    while (!float_text.empty() &&
           (float_text.back() == 'f' || float_text.back() == 'F' ||
            float_text.back() == 'l' || float_text.back() == 'L'))
        float_text.pop_back();
    if (has_float_marker) {
        char *end = NULL;
        double parsed = std::strtod(float_text.c_str(), &end);
        if (end && *end == '\0') {
            out.kind = constant_out::FLOAT;
            out.f = parsed;
            return true;
        }
    }
    return false;
}

std::uint64_t fnv1a(const std::string &text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string compiler_flags(const options &o) {
    std::ostringstream command;
    for (const std::string &include : o.includes)
        command << " -I" << shell_quote(include);
    for (const std::string &define : o.defines)
        command << " -D" << shell_quote(define);
    return command.str();
}

int macro_int(const std::map<std::string, std::string> &macros,
              const char *name, int fallback) {
    auto it = macros.find(name);
    if (it == macros.end())
        return fallback;
    char *end = NULL;
    long value = std::strtol(it->second.c_str(), &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 64)
        return fallback;
    return (int)value;
}

/* Ask the selected compiler for its target data model instead of assuming the
 * host's.  This keeps `--cc`, `-m32`, and sysroot flags honest. */
void probe_abi(const options &o, abi_info &abi, std::string &warning) {
    std::string cc = o.cc.empty() ? "cc" : o.cc;
    std::ostringstream command;
    command << shell_quote(cc) << " -dM -E -x c -std=gnu11"
            << compiler_flags(o) << " - ";
#ifdef _WIN32
    command << "< NUL 2>&1";
#else
    command << "</dev/null 2>&1";
#endif
    std::string output;
    int status = -1;
    if (!platform_run_command(command.str(), {}, output, status) ||
        status != 0) {
        warning = "cannot probe the target ABI; using host widths";
        return;
    }

    std::map<std::string, std::string> macros;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, 8, "#define ") != 0)
            continue;
        size_t start = 8;
        while (start < line.size() &&
               std::isspace((unsigned char)line[start]))
            ++start;
        size_t end = start;
        while (end < line.size() &&
               is_ident_char((unsigned char)line[end]))
            ++end;
        if (end == start)
            continue;
        size_t value = end;
        while (value < line.size() &&
               std::isspace((unsigned char)line[value]))
            ++value;
        macros[line.substr(start, end - start)] = line.substr(value);
    }

    abi.short_bytes = macro_int(macros, "__SIZEOF_SHORT__", abi.short_bytes);
    abi.int_bytes = macro_int(macros, "__SIZEOF_INT__", abi.int_bytes);
    abi.long_bytes = macro_int(macros, "__SIZEOF_LONG__", abi.long_bytes);
    abi.long_long_bytes =
        macro_int(macros, "__SIZEOF_LONG_LONG__", abi.long_long_bytes);
    abi.pointer_bytes =
        macro_int(macros, "__SIZEOF_POINTER__", abi.pointer_bytes);
    abi.wchar_bytes = macro_int(macros, "__SIZEOF_WCHAR_T__", abi.wchar_bytes);
    abi.char_unsigned = macros.count("__CHAR_UNSIGNED__") != 0;
    abi.wchar_unsigned = macros.count("__WCHAR_UNSIGNED__") != 0;
}

bool preprocess(const options &o, const abi_info &abi,
                std::vector<token> &tokens,
                std::map<std::string, constant_out> &constants,
                std::string &preprocessed, std::string &error) {
    std::string cc = o.cc.empty() ? "cc" : o.cc;
    std::ostringstream command;
    command << shell_quote(cc) << " -E -x c -std=gnu11 -dD"
            << compiler_flags(o);
    for (size_t i = 1; i < o.headers.size(); ++i)
        command << " -include " << shell_quote(o.headers[i]);
    command << " " << shell_quote(o.headers[0]) << " 2>&1";

    std::string output;
    int status = -1;
    if (!platform_run_command(command.str(), {}, output, status)) {
        error = "cannot run the C preprocessor";
        return false;
    }
    if (status != 0) {
        error = "C preprocessor failed:\n" + output;
        return false;
    }
    preprocessed = output;

    std::set<std::string> targets;
    for (const std::string &header : o.headers)
        targets.insert(canonical_path(header));

    std::string current_file;
    int pack = 0;
    std::vector<int> pack_stack;
    size_t pos = 0;
    while (pos <= preprocessed.size()) {
        size_t end = preprocessed.find('\n', pos);
        if (end == std::string::npos)
            end = preprocessed.size();
        std::string line = preprocessed.substr(pos, end - pos);
        pos = end + 1;

        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] == '#') {
            const std::string marker = "# ";
            if (trimmed.compare(0, 2, marker) == 0 &&
                trimmed.size() > 3 &&
                std::isdigit((unsigned char)trimmed[2])) {
                size_t quote = trimmed.find('"');
                if (quote != std::string::npos) {
                    size_t close = trimmed.find('"', quote + 1);
                    if (close != std::string::npos)
                        current_file =
                            canonical_path(trimmed.substr(quote + 1,
                                                          close - quote - 1));
                }
            } else if (trimmed.compare(0, 13, "#pragma pack") == 0) {
                parse_pack_directive(trimmed, pack, pack_stack);
            } else if (trimmed.compare(0, 7, "#define") == 0 &&
                       o.emit_constants &&
                       targets.count(current_file)) {
                size_t i = 7;
                while (i < trimmed.size() &&
                       std::isspace((unsigned char)trimmed[i]))
                    ++i;
                size_t start = i;
                while (i < trimmed.size() &&
                       is_ident_char((unsigned char)trimmed[i]))
                    ++i;
                std::string name = trimmed.substr(start, i - start);
                bool function_like =
                    i < trimmed.size() && trimmed[i] == '(';
                if (!name.empty() && !function_like) {
                    std::map<std::string, long long> names;
                    for (const auto &entry : constants) {
                        if (entry.second.kind == constant_out::INT)
                            names[entry.first] = entry.second.i;
                    }
                    constant_out constant;
                    if (parse_constant_text(trimmed.substr(i), names, abi,
                                            constant))
                        constants[name] = std::move(constant);
                }
            }
        } else if (targets.count(current_file)) {
            tokenize_line(line, pack, tokens);
        }

        if (end == preprocessed.size())
            break;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * C type model
 * ---------------------------------------------------------------------- */

struct ctype;
using ctype_ptr = std::shared_ptr<ctype>;

struct ctype {
    enum kind_t { SCALAR, RECORD, POINTER, ARRAY, CALLBACK } kind = SCALAR;
    std::string name;
    ctype_ptr element;
    std::size_t count = 0;
    ctype_ptr result;
    std::vector<ctype_ptr> params;
};

ctype_ptr make_scalar(const std::string &name) {
    ctype_ptr type = std::make_shared<ctype>();
    type->kind = ctype::SCALAR;
    type->name = name;
    return type;
}

ctype_ptr make_pointer(ctype_ptr element) {
    ctype_ptr type = std::make_shared<ctype>();
    type->kind = ctype::POINTER;
    type->element = std::move(element);
    return type;
}

ctype_ptr make_array(ctype_ptr element, std::size_t count) {
    ctype_ptr type = std::make_shared<ctype>();
    type->kind = ctype::ARRAY;
    type->element = std::move(element);
    type->count = count;
    return type;
}

ctype_ptr make_callback(ctype_ptr result, std::vector<ctype_ptr> params) {
    ctype_ptr type = std::make_shared<ctype>();
    type->kind = ctype::CALLBACK;
    type->result = std::move(result);
    type->params = std::move(params);
    return type;
}

/* -------------------------------------------------------------------------
 * Parser
 * ---------------------------------------------------------------------- */

struct parse_error {
    std::string message;
    bool fatal = false;
};

bool is_type_keyword(const std::string &word) {
    static const std::set<std::string> keywords = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "_Bool", "bool", "_Complex", "__signed__",
        "__unsigned__", "__int8", "__int16", "__int32", "__int64"};
    return keywords.count(word) != 0;
}

bool is_layout_attribute(const std::string &word) {
    static const std::set<std::string> attributes = {
        "aligned", "__aligned__", "align", "vector_size",
        "__vector_size__", "mode", "__mode__", "transparent_union",
        "ms_struct", "gcc_struct", "scalar_storage_order"};
    return attributes.count(word) != 0;
}

bool is_calling_convention(const std::string &word) {
    static const std::set<std::string> conventions = {
        "__cdecl", "__stdcall", "__fastcall", "__vectorcall",
        "WINAPI", "CALLBACK", "APIENTRY", "__attribute_cdecl"};
    return conventions.count(word) != 0;
}

struct record_out {
    std::string name;
    std::vector<std::pair<std::string, ctype_ptr>> fields;
};

struct callback_out {
    std::string name;
    ctype_ptr result;
    std::vector<ctype_ptr> params;
};

struct function_out {
    std::string name;
    ctype_ptr result;
    std::vector<ctype_ptr> args;
};

struct variable_out {
    std::string name;
    ctype_ptr type;
};

struct declarator {
    std::string name;
    int pointers = 0;
    std::vector<std::size_t> arrays;
    bool is_callback = false;
    std::vector<ctype_ptr> params;
    bool variadic = false;
    bool function = false;
    bool layout_unsafe = false;
    std::string layout_name;
};

struct attribute_flags {
    bool packed = false;
    bool layout_unsafe = false;
    std::string layout_name;
};

struct spec {
    ctype_ptr base;
    bool is_typedef = false;
    bool is_static = false;
    bool packed = false;
    bool layout_unsafe = false;
    std::string layout_name;
    bool unsupported_convention = false;
};

class parser {
public:
    parser(std::vector<token> tokens,
           std::map<std::string, constant_out> constants,
           const abi_info &abi, bool tolerate_errors)
        : tokens_(std::move(tokens)), constants_(std::move(constants)),
          abi_(abi), tolerate_errors_(tolerate_errors) {
        preload_typedefs();
    }

    bool parse(std::string &error) {
        while (!eof()) {
            size_t start = position_;
            try {
                if (accept(";"))
                    continue;
                if (is("_Static_assert")) {
                    skip_declaration();
                    continue;
                }
                if (is("typedef")) {
                    parse_typedef();
                    continue;
                }
                spec s = parse_specifiers();
                if (accept(";"))
                    continue;
                parse_declaration_rest(s);
            } catch (const parse_error &e) {
                record_error(e);
                position_ = start;
                skip_declaration();
                if (position_ == start)
                    ++position_;
            }
        }
        try {
            validate_value_types();
        } catch (const parse_error &e) {
            record_error(e);
        }
        error.clear();
        return true;
    }

    std::string render(const options &o, const std::string &source_hash);

    void keep_symbols(const std::vector<std::string> &patterns);

private:
    void preload_typedefs() {
        typedefs_["int8_t"] = make_scalar("i8");
        typedefs_["uint8_t"] = make_scalar("u8");
        typedefs_["int16_t"] = make_scalar("i16");
        typedefs_["uint16_t"] = make_scalar("u16");
        typedefs_["int32_t"] = make_scalar("i32");
        typedefs_["uint32_t"] = make_scalar("u32");
        typedefs_["int64_t"] = make_scalar("i64");
        typedefs_["uint64_t"] = make_scalar("u64");
        const char *signed_pointer =
            abi_.pointer_bytes == 4 ? "i32" : "i64";
        const char *unsigned_pointer =
            abi_.pointer_bytes == 4 ? "u32" : "u64";
        typedefs_["intptr_t"] = make_scalar(signed_pointer);
        typedefs_["uintptr_t"] = make_scalar(unsigned_pointer);
        typedefs_["ptrdiff_t"] = make_scalar(signed_pointer);
        typedefs_["size_t"] = make_scalar(unsigned_pointer);
        typedefs_["ssize_t"] = make_scalar(signed_pointer);
        typedefs_["intmax_t"] =
            make_scalar(abi_.long_long_bytes == 8 ? "i64" : "i32");
        typedefs_["uintmax_t"] =
            make_scalar(abi_.long_long_bytes == 8 ? "u64" : "u32");
        if (abi_.wchar_bytes == 1)
            typedefs_["wchar_t"] =
                make_scalar(abi_.wchar_unsigned ? "u8" : "i8");
        else if (abi_.wchar_bytes == 2)
            typedefs_["wchar_t"] =
                make_scalar(abi_.wchar_unsigned ? "u16" : "i16");
        else
            typedefs_["wchar_t"] =
                make_scalar(abi_.wchar_unsigned ? "u32" : "i32");
        typedefs_["char16_t"] = make_scalar("u16");
        typedefs_["char32_t"] = make_scalar("u32");
        typedefs_["bool"] = make_scalar("u8");
        typedefs_["_Bool"] = make_scalar("u8");
    }

    const token &cur() const {
        static const token empty;
        return position_ < tokens_.size() ? tokens_[position_] : empty;
    }

    bool eof() const { return position_ >= tokens_.size(); }

    bool is(const std::string &text, size_t offset = 0) const {
        return position_ + offset < tokens_.size() &&
               tokens_[position_ + offset].text == text;
    }

    bool accept(const std::string &text) {
        if (!is(text))
            return false;
        ++position_;
        return true;
    }

    void expect(const std::string &text) {
        if (!accept(text))
            fail("expected '" + text + "'");
    }

    [[noreturn]] void fail(const std::string &message, bool fatal = true) {
        throw parse_error{message, fatal};
    }

    void record_error(const parse_error &error) {
        warnings_.push_back(error.message);
        if (error.fatal && !tolerate_errors_)
            fatal_ = true;
    }

    void mark_type(const ctype_ptr &type) {
        if (!type)
            return;
        switch (type->kind) {
            case ctype::SCALAR:
                return;
            case ctype::RECORD:
                if (!kept_records_.insert(type->name).second)
                    return;
                {
                    auto it = records_.find(type->name);
                    if (it != records_.end()) {
                        for (const auto &field : it->second.fields)
                            mark_type(field.second);
                    }
                }
                return;
            case ctype::POINTER:
            case ctype::ARRAY:
                mark_type(type->element);
                return;
            case ctype::CALLBACK:
                if (!type->name.empty())
                    kept_callbacks_.insert(type->name);
                mark_type(type->result);
                for (const ctype_ptr &param : type->params)
                    mark_type(param);
                return;
        }
    }

    void collect_value_type(const ctype_ptr &type, const std::string &where,
                            std::vector<std::string> &errors) {
        if (!type)
            return;
        if (type->kind == ctype::RECORD) {
            if (!records_.count(type->name))
                errors.push_back(
                    "struct " + type->name +
                    " is only forward-declared and cannot be used by value (" +
                    where + ")");
            return;
        }
        if (type->kind == ctype::ARRAY)
            collect_value_type(type->element, where, errors);
    }

    void validate_value_types() {
        std::vector<std::string> errors;
        for (const auto &entry : records_) {
            for (const auto &field : entry.second.fields)
                collect_value_type(field.second,
                                   "field " + entry.first + "." + field.first,
                                   errors);
        }
        for (const auto &function : functions_) {
            collect_value_type(function.result,
                               "return type of " + function.name, errors);
            for (std::size_t i = 0; i < function.args.size(); ++i)
                collect_value_type(function.args[i],
                                   "argument " + std::to_string(i + 1) +
                                       " of " + function.name,
                                   errors);
        }
        for (const auto &variable : variables_)
            collect_value_type(variable.type,
                               "variable " + variable.name, errors);
        for (const std::string &message : errors) {
            warnings_.push_back(message);
            if (!tolerate_errors_)
                fatal_ = true;
        }
    }

    void advance() {
        if (position_ < tokens_.size())
            ++position_;
    }

    bool ident_at(size_t offset = 0) const {
        return position_ + offset < tokens_.size() &&
               tokens_[position_ + offset].kind == TOK_IDENT;
    }

    void skip_attributes(attribute_flags &flags) {
        while (!eof()) {
            const std::string &text = cur().text;
            if (text == "__attribute__" || text == "__attribute" ||
                text == "__declspec" || text == "__asm__" ||
                text == "__asm" || text == "asm") {
                advance();
                if (!accept("("))
                    continue;
                int depth = 1;
                while (depth > 0 && !eof()) {
                    if (is("(")) {
                        ++depth;
                        advance();
                    } else if (is(")")) {
                        --depth;
                        advance();
                    } else {
                        if (cur().text == "packed" ||
                            cur().text == "__packed__") {
                            flags.packed = true;
                        } else if (is_layout_attribute(cur().text)) {
                            flags.layout_unsafe = true;
                            if (flags.layout_name.empty())
                                flags.layout_name = cur().text;
                        }
                        advance();
                    }
                }
                continue;
            }
            if (text == "__extension__" || text == "__inline" ||
                text == "__inline__" || text == "__restrict" ||
                text == "__restrict__") {
                advance();
                continue;
            }
            break;
        }
    }

    ctype_ptr integer_type(int bytes, bool is_unsigned_type) {
        switch (bytes) {
            case 1: return make_scalar(is_unsigned_type ? "u8" : "i8");
            case 2: return make_scalar(is_unsigned_type ? "u16" : "i16");
            case 4: return make_scalar(is_unsigned_type ? "u32" : "i32");
            case 8: return make_scalar(is_unsigned_type ? "u64" : "i64");
            default:
                fail("unsupported integer width");
        }
    }

    ctype_ptr scalar_from_words(const std::vector<std::string> &words) {
        bool is_unsigned = false;
        bool is_signed = false;
        int longs = 0;
        bool is_short = false;
        bool is_char = false;
        bool is_int = false;
        bool is_float = false;
        bool is_double = false;
        bool is_void = false;
        bool is_bool = false;
        for (const std::string &word : words) {
            if (word == "unsigned" || word == "__unsigned__")
                is_unsigned = true;
            else if (word == "signed" || word == "__signed__")
                is_signed = true;
            else if (word == "long")
                ++longs;
            else if (word == "short")
                is_short = true;
            else if (word == "char")
                is_char = true;
            else if (word == "int")
                is_int = true;
            else if (word == "float")
                is_float = true;
            else if (word == "double")
                is_double = true;
            else if (word == "void")
                is_void = true;
            else if (word == "_Bool" || word == "bool")
                is_bool = true;
            else if (word == "_Complex")
                fail("complex types are not supported");
            else if (word == "__int8")
                return make_scalar(is_unsigned ? "u8" : "i8");
            else if (word == "__int16")
                return make_scalar(is_unsigned ? "u16" : "i16");
            else if (word == "__int32")
                return make_scalar(is_unsigned ? "u32" : "i32");
            else if (word == "__int64")
                return make_scalar(is_unsigned ? "u64" : "i64");
        }
        (void)is_signed;
        if (is_void)
            return make_scalar("void");
        if (is_bool)
            return make_scalar("u8");
        if (is_float)
            return make_scalar("f32");
        if (is_double) {
            if (longs > 0)
                fail("long double is not supported");
            return make_scalar("f64");
        }
        if (is_char)
            return make_scalar(is_unsigned ? "u8" : "i8");
        if (is_short)
            return integer_type(abi_.short_bytes, is_unsigned);
        if (longs >= 2)
            return integer_type(abi_.long_long_bytes, is_unsigned);
        if (longs == 1)
            return integer_type(abi_.long_bytes, is_unsigned);
        if (is_int)
            return integer_type(abi_.int_bytes, is_unsigned);
        return integer_type(abi_.int_bytes, is_unsigned);
    }

    ctype_ptr parse_tag(const attribute_flags &outer) {
        std::string keyword = cur().text;
        advance();
        attribute_flags flags = outer;
        skip_attributes(flags);
        std::string tag;
        if (ident_at() && !is("{"))
            tag = cur().text, advance();

        ctype_ptr result;
        if (accept("{")) {
            if (keyword == "enum") {
                parse_enum_body();
                result = integer_type(abi_.int_bytes, false);
            } else if (keyword == "union") {
                skip_declaration();
                fail("unions are not supported", true);
            } else {
                result = parse_struct_body(tag, flags.packed);
            }
        } else {
            result = record_ref(tag);
        }
        skip_attributes(flags);
        if (flags.packed)
            fail("packed structs are not supported", true);
        if (flags.layout_unsafe)
            fail("layout-changing attribute '" + flags.layout_name +
                     "' is not supported", true);
        return result;
    }

    ctype_ptr parse_struct_body(const std::string &tag, bool packed) {
        ctype_ptr record = std::make_shared<ctype>();
        record->kind = ctype::RECORD;
        record->name = tag;
        if (packed)
            fail("packed structs are not supported", true);

        record_out out;
        out.name = tag;

        while (!eof() && !accept("}")) {
            if (cur().pack != 0)
                fail("packed structs are not supported", true);
            spec s = parse_specifiers();
            if (s.layout_unsafe)
                fail("layout-changing attribute '" + s.layout_name +
                     "' is not supported",
                 true);
            if (accept(";")) {
                if (s.base && s.base->kind == ctype::RECORD &&
                    s.base->name.rfind("__anon_", 0) == 0)
                    fail("anonymous struct members are not supported", true);
                continue;
            }
            for (;;) {
                declarator d = parse_declarator();
                if (d.layout_unsafe)
                    fail("layout-changing attribute '" + d.layout_name +
                         "' is not supported",
                     true);
                if (accept(":"))
                    fail("bitfields are not supported", true);
                if (d.name.empty())
                    fail("anonymous struct members are not supported", true);
                if (d.is_callback)
                    fail("function pointer fields are not supported", true);
                for (std::size_t dim : d.arrays)
                    if (dim == 0)
                        fail("flexible array members are not supported", true);
                ctype_ptr field_type = apply_type(s.base, d);
                out.fields.emplace_back(d.name, field_type);
                if (!accept(","))
                    break;
            }
            expect(";");
        }

        if (tag.empty()) {
            record->name = "__anon_" + std::to_string(++anon_counter_);
            out.name = record->name;
        }
        if (records_.count(out.name))
            fail("duplicate struct " + out.name);
        records_[out.name] = std::move(out);
        record->name = tag.empty() ? record->name : tag;
        return record;
    }

    ctype_ptr record_ref(const std::string &tag) {
        if (tag.empty())
            fail("anonymous struct without a declarator is not supported", true);
        ctype_ptr type = std::make_shared<ctype>();
        type->kind = ctype::RECORD;
        type->name = tag;
        referenced_records_.insert(tag);
        return type;
    }

    void parse_enum_body() {
        long long next = 0;
        while (!eof() && !accept("}")) {
            if (!ident_at())
                fail("invalid enumerator");
            std::string name = cur().text;
            advance();
            long long value = next;
            if (accept("=")) {
                std::string expression_text;
                int depth = 0;
                while (!eof()) {
                    if (depth == 0 && (is(",") || is("}")))
                        break;
                    if (is("("))
                        ++depth;
                    else if (is(")"))
                        --depth;
                    expression_text += cur().text;
                    expression_text += ' ';
                    advance();
                }
                std::map<std::string, long long> names;
                for (const auto &entry : constants_) {
                    if (entry.second.kind == constant_out::INT)
                        names[entry.first] = entry.second.i;
                }
                long long parsed = 0;
                integer_expression expression(expression_text, names, abi_);
                if (!expression.evaluate(parsed))
                    fail("unsupported enumerator value", true);
                value = parsed;
            }
            constant_out constant;
            constant.kind = constant_out::INT;
            constant.i = value;
            constants_[name] = constant;
            next = value + 1;
            if (!accept(",")) {
                accept("}");
                break;
            }
        }
    }

    spec parse_specifiers() {
        spec s;
        std::vector<std::string> words;
        bool saw_base = false;
        attribute_flags flags;

        while (!eof()) {
            const token &t = cur();
            const std::string &text = t.text;
            if (t.kind != TOK_IDENT)
                break;
            if (text == "typedef") {
                s.is_typedef = true;
                advance();
                continue;
            }
            if (text == "static") {
                s.is_static = true;
                advance();
                continue;
            }
            if (text == "extern" || text == "register" ||
                text == "auto" || text == "inline" || text == "__inline" ||
                text == "__inline__" || text == "const" ||
                text == "volatile" || text == "restrict" ||
                text == "__restrict" || text == "__restrict__" ||
                text == "_Nullable" || text == "__nullable" ||
                text == "_Nonnull" || text == "__nonnull" ||
                text == "__null_unspecified" || text == "_Atomic") {
                advance();
                continue;
            }
            if (text == "__attribute__" || text == "__attribute" ||
                text == "__declspec" || text == "__asm__" ||
                text == "__asm" || text == "asm" ||
                text == "__extension__") {
                skip_attributes(flags);
                continue;
            }
            if (is_calling_convention(text)) {
                s.unsupported_convention = true;
                advance();
                continue;
            }
            if (text == "struct" || text == "union" || text == "enum") {
                s.base = parse_tag(flags);
                saw_base = true;
                continue;
            }
            if (is_type_keyword(text)) {
                words.push_back(text);
                advance();
                continue;
            }
            if (!saw_base && words.empty()) {
                auto it = typedefs_.find(text);
                if (it != typedefs_.end()) {
                    s.base = it->second;
                    saw_base = true;
                    advance();
                    continue;
                }
                fail("unknown type '" + text +
                         "'; pass the header that defines it with "
                         "--header",
                     true);
            }
            break;
        }

        if (!saw_base) {
            if (words.empty())
                fail("missing type specifier");
            s.base = scalar_from_words(words);
        }
        s.packed = s.packed || flags.packed;
        s.layout_unsafe = s.layout_unsafe || flags.layout_unsafe;
        if (s.layout_name.empty())
            s.layout_name = flags.layout_name;
        return s;
    }

    declarator parse_declarator() {
        declarator d;
        while (accept("*")) {
            ++d.pointers;
            attribute_flags pointer_flags;
            skip_attributes(pointer_flags);
            if (pointer_flags.layout_unsafe) {
                d.layout_unsafe = true;
                if (d.layout_name.empty())
                    d.layout_name = pointer_flags.layout_name;
            }
            while (is("const") || is("volatile") || is("restrict") ||
                   is("__restrict") || is("__restrict__"))
                advance();
        }

        if (is("(") && is("*", 1)) {
            expect("(");
            expect("*");
            if (ident_at())
                d.name = cur().text, advance();
            expect(")");
            expect("(");
            parse_params(d.params, d.variadic);
            expect(")");
            d.is_callback = true;
            return d;
        }

        if (ident_at() && !is_type_keyword(cur().text))
            d.name = cur().text, advance();

        for (;;) {
            if (accept("[")) {
                std::size_t count = 0;
                if (!is("]")) {
                    if (cur().kind != TOK_NUMBER)
                        fail("array length must be an integer");
                    char *end = NULL;
                    count = (std::size_t)std::strtoull(cur().text.c_str(),
                                                       &end, 0);
                    if (!end || *end != '\0')
                        fail("array length must be an integer");
                    advance();
                }
                expect("]");
                d.arrays.push_back(count);
                continue;
            }
            if (accept("(")) {
                d.function = true;
                parse_params(d.params, d.variadic);
                expect(")");
                break;
            }
            break;
        }
        attribute_flags trailing;
        skip_attributes(trailing);
        if (trailing.layout_unsafe) {
            d.layout_unsafe = true;
            if (d.layout_name.empty())
                d.layout_name = trailing.layout_name;
        }
        return d;
    }

    void parse_params(std::vector<ctype_ptr> &params, bool &variadic) {
        if (is("void") && is(")", 1)) {
            advance();
            return;
        }
        if (is(")"))
            return;
        for (;;) {
            if (accept("...")) {
                variadic = true;
                return;
            }
            spec s = parse_specifiers();
            if (s.unsupported_convention)
                fail("unsupported calling convention");
            declarator d = parse_declarator();
            if (d.variadic)
                variadic = true;
            if (d.is_callback) {
                if (d.variadic)
                    fail("variadic callbacks are not supported");
                ctype_ptr result = apply_pointers(s.base, d.pointers);
                params.push_back(make_callback(result, d.params));
            } else {
                params.push_back(decay_parameter(apply_type(s.base, d)));
            }
            if (!accept(","))
                break;
        }
    }

    ctype_ptr apply_pointers(ctype_ptr base, int pointers) {
        ctype_ptr type = std::move(base);
        for (int i = 0; i < pointers; ++i) {
            if (type && type->kind == ctype::SCALAR &&
                type->name == "i8") {
                type = make_scalar("cstring");
            } else if (type && type->kind == ctype::SCALAR &&
                       type->name == "u8") {
                type = make_scalar("bytes");
            } else if (type && type->kind == ctype::SCALAR &&
                       type->name == "void") {
                type = make_scalar("ptr");
            } else {
                type = make_pointer(type);
            }
        }
        return type;
    }

    ctype_ptr apply_type(ctype_ptr base, const declarator &d) {
        ctype_ptr type = apply_pointers(std::move(base), d.pointers);
        if (d.is_callback)
            type = make_callback(type, d.params);
        for (auto it = d.arrays.rbegin(); it != d.arrays.rend(); ++it)
            type = make_array(type, *it);
        return type;
    }

    /* C function parameters of array type are adjusted to pointers one level
     * at a time: `int a[4]` becomes `int *`, `int a[][4]` becomes
     * `int (*)[4]`. */
    ctype_ptr decay_parameter(ctype_ptr type) {
        while (type && type->kind == ctype::ARRAY)
            type = make_pointer(type->element);
        return type;
    }

    void parse_typedef() {
        expect("typedef");
        spec s = parse_specifiers();
        if (s.unsupported_convention)
            fail("unsupported calling convention");
        if (s.layout_unsafe)
            fail("layout-changing attribute '" + s.layout_name +
                     "' is not supported", true);
        for (;;) {
            declarator d = parse_declarator();
            if (d.name.empty())
                fail("typedef requires a name");
            if (d.layout_unsafe)
                fail("layout-changing attribute '" + d.layout_name +
                     "' is not supported", true);
            if (d.is_callback) {
                if (d.variadic)
                    fail("variadic callbacks are not supported");
                ctype_ptr result = apply_pointers(s.base, d.pointers);
                callback_out out;
                out.name = d.name;
                out.result = result;
                out.params = d.params;
                callbacks_[d.name] = std::move(out);
                ctype_ptr type = make_callback(result, d.params);
                type->name = d.name;
                typedefs_[d.name] = type;
            } else {
                ctype_ptr type = apply_type(s.base, d);
                if (type->kind == ctype::RECORD &&
                    (type->name.empty() ||
                     type->name.rfind("__anon_", 0) == 0))
                    rename_anonymous(type, d.name);
                typedefs_[d.name] = type;
            }
            if (!accept(","))
                break;
        }
        expect(";");
    }

    void rename_anonymous(const ctype_ptr &type, const std::string &name) {
        auto it = records_.find(type->name);
        if (it == records_.end())
            fail("internal: anonymous record not found");
        record_out record = std::move(it->second);
        records_.erase(it);
        record.name = name;
        records_[name] = std::move(record);
        type->name = name;
    }

    void parse_declaration_rest(spec &s) {
        if (s.layout_unsafe)
            fail("layout-changing attribute '" + s.layout_name +
                     "' is not supported", true);
        for (;;) {
            declarator d = parse_declarator();
            if (d.name.empty())
                fail("declaration requires a name");
            if (d.layout_unsafe)
                fail("layout-changing attribute '" + d.layout_name +
                     "' is not supported", true);
            if (d.function) {
                if (d.variadic)
                    fail("variadic functions are not supported");
                if (s.unsupported_convention)
                    fail("unsupported calling convention");
                if (!s.is_static) {
                    function_out function;
                    function.name = d.name;
                    function.result = apply_pointers(s.base, d.pointers);
                    function.args = d.params;
                    functions_.push_back(std::move(function));
                }
            } else if (!s.is_static) {
                variable_out variable;
                variable.name = d.name;
                variable.type = apply_type(s.base, d);
                variables_.push_back(std::move(variable));
            }
            if (!accept(","))
                break;
        }
        expect(";");
    }

    void skip_declaration() {
        int depth = 0;
        while (!eof()) {
            const std::string &text = cur().text;
            if (text == "{") {
                ++depth;
                advance();
                continue;
            }
            if (text == "}") {
                if (depth == 0) {
                    advance();
                    return;
                }
                --depth;
                advance();
                continue;
            }
            if (text == "(" || text == "[") {
                ++depth;
                advance();
                continue;
            }
            if (text == ")" || text == "]") {
                if (depth > 0)
                    --depth;
                advance();
                continue;
            }
            if (text == ";" && depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    std::string render_type(const ctype_ptr &type) {
        if (!type)
            return json_string("void");
        switch (type->kind) {
            case ctype::SCALAR:
                return json_string(type->name);
            case ctype::RECORD:
                if (type->name.empty())
                    fail("internal: unnamed record");
                referenced_records_.insert(type->name);
                return json_string(type->name);
            case ctype::POINTER:
                return "{\"kind\":\"pointer\",\"to\":" +
                       render_type(type->element) + "}";
            case ctype::ARRAY:
                return "{\"kind\":\"array\",\"element\":" +
                       render_type(type->element) +
                       ",\"count\":" + std::to_string(type->count) + "}";
            case ctype::CALLBACK:
                if (!type->name.empty())
                    return json_string(type->name);
                return render_callback(type->result, type->params);
        }
        return json_string("void");
    }

    std::string render_callback(const ctype_ptr &result,
                                const std::vector<ctype_ptr> &params) {
        std::string out = "{\"kind\":\"callback\",\"result\":";
        out += render_type(result);
        out += ",\"args\":[";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i)
                out += ',';
            out += render_type(params[i]);
        }
        out += "]}";
        return out;
    }

    static std::string json_string(const std::string &value) {
        std::string out = "\"";
        for (unsigned char c : value) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                        out += buffer;
                    } else {
                        out += (char)c;
                    }
                    break;
            }
        }
        out += '"';
        return out;
    }

    std::vector<token> tokens_;
    std::map<std::string, constant_out> constants_;
    abi_info abi_;
    std::map<std::string, ctype_ptr> typedefs_;
    std::map<std::string, record_out> records_;
    std::map<std::string, callback_out> callbacks_;
    std::vector<function_out> functions_;
    std::vector<variable_out> variables_;
    bool filtered_ = false;
    std::set<std::string> kept_records_;
    std::set<std::string> kept_callbacks_;
    std::set<std::string> referenced_records_;
    std::vector<std::string> warnings_;
    size_t position_ = 0;
    size_t anon_counter_ = 0;
    bool fatal_ = false;
    bool tolerate_errors_ = false;

public:
    const std::vector<std::string> &warnings() const { return warnings_; }
    bool fatal() const { return fatal_; }
};

namespace {

bool glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*')
                ++pattern;
            if (!*pattern)
                return true;
            while (*text) {
                if (glob_match(pattern, text))
                    return true;
                ++text;
            }
            return false;
        }
        if (*pattern == '?') {
            if (!*text)
                return false;
            ++pattern;
            ++text;
            continue;
        }
        if (*pattern != *text)
            return false;
        ++pattern;
        ++text;
    }
    return *text == '\0';
}

} // namespace

void parser::keep_symbols(const std::vector<std::string> &patterns) {
    if (patterns.empty())
        return;
    filtered_ = true;
    auto keep = [&](const std::string &name) {
        for (const std::string &pattern : patterns) {
            if (glob_match(pattern.c_str(), name.c_str()))
                return true;
        }
        return false;
    };
    functions_.erase(
        std::remove_if(functions_.begin(), functions_.end(),
                       [&](const function_out &function) {
                           return !keep(function.name);
                       }),
        functions_.end());
    variables_.erase(
        std::remove_if(variables_.begin(), variables_.end(),
                       [&](const variable_out &variable) {
                           return !keep(variable.name);
                       }),
        variables_.end());
    for (auto it = constants_.begin(); it != constants_.end();) {
        if (keep(it->first))
            ++it;
        else
            it = constants_.erase(it);
    }
    for (const function_out &function : functions_) {
        mark_type(function.result);
        for (const ctype_ptr &argument : function.args)
            mark_type(argument);
    }
    for (const variable_out &variable : variables_)
        mark_type(variable.type);
}

std::string parser::render(const options &o, const std::string &source_hash) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"ffi\": 1,\n";
    out << "  \"library\": " << json_string(o.module) << ",\n";
    out << "  \"generated\": {\n";
    out << "    \"tool\": \"skein --ffi-gen\",\n";
    out << "    \"version\": 1,\n";
    out << "    \"headers\": [";
    for (size_t i = 0; i < o.headers.size(); ++i) {
        if (i)
            out << ", ";
        out << json_string(o.headers[i]);
    }
    out << "],\n";
    out << "    \"cc\": " << json_string(o.cc.empty() ? "cc" : o.cc) << ",\n";
    out << "    \"flags\": [";
    bool first_flag = true;
    for (const std::string &include : o.includes) {
        if (!first_flag)
            out << ", ";
        first_flag = false;
        out << json_string("-I" + include);
    }
    for (const std::string &define : o.defines) {
        if (!first_flag)
            out << ", ";
        first_flag = false;
        out << json_string("-D" + define);
    }
    out << "],\n";
    out << "    \"source_hash\": " << json_string(source_hash) << "\n";
    out << "  },\n";

    out << "  \"types\": {";
    std::vector<std::string> type_entries;
    for (const auto &entry : records_) {
        const record_out &record = entry.second;
        if (filtered_ && !kept_records_.count(record.name))
            continue;
        std::string text = "    " + json_string(record.name) +
                           ": {\"kind\":\"struct\",\"fields\":[";
        for (size_t i = 0; i < record.fields.size(); ++i) {
            if (i)
                text += ", ";
            text += "{\"name\":" + json_string(record.fields[i].first) +
                    ",\"type\":" + render_type(record.fields[i].second) +
                    "}";
        }
        text += "]}";
        type_entries.push_back(std::move(text));
        referenced_records_.erase(record.name);
    }
    for (const auto &entry : callbacks_) {
        const callback_out &callback = entry.second;
        if (filtered_ && !kept_callbacks_.count(callback.name))
            continue;
        std::string text = "    " + json_string(callback.name) + ": " +
                           render_callback(callback.result, callback.params);
        type_entries.push_back(std::move(text));
    }
    for (const std::string &name : referenced_records_) {
        if (records_.count(name))
            continue;
        if (filtered_ && !kept_records_.count(name))
            continue;
        type_entries.push_back("    " + json_string(name) +
                               ": {\"kind\":\"opaque\"}");
    }
    for (size_t i = 0; i < type_entries.size(); ++i) {
        out << "\n";
        out << type_entries[i];
        if (i + 1 < type_entries.size())
            out << ",";
    }
    if (!type_entries.empty())
        out << "\n  ";
    out << "},\n";

    out << "  \"functions\": {";
    for (size_t i = 0; i < functions_.size(); ++i) {
        const function_out &function = functions_[i];
        out << "\n    " << json_string(function.name)
            << ": {\"result\":" << render_type(function.result)
            << ",\"args\":[";
        for (size_t j = 0; j < function.args.size(); ++j) {
            if (j)
                out << ", ";
            out << render_type(function.args[j]);
        }
        out << "]}";
        if (i + 1 < functions_.size())
            out << ",";
    }
    if (!functions_.empty())
        out << "\n  ";
    out << "}";

    out << ",\n  \"variables\": {";
    for (size_t i = 0; i < variables_.size(); ++i) {
        const variable_out &variable = variables_[i];
        out << "\n    " << json_string(variable.name) << ": "
            << render_type(variable.type);
        if (i + 1 < variables_.size())
            out << ",";
    }
    if (!variables_.empty())
        out << "\n  ";
    out << "}";

    if (o.emit_constants) {
        out << ",\n  \"constants\": {";
        bool first = true;
        for (const auto &entry : constants_) {
            out << (first ? "\n    " : ",\n    ");
            first = false;
            out << json_string(entry.first) << ": ";
            const constant_out &constant = entry.second;
            switch (constant.kind) {
                case constant_out::INT:
                    out << constant.i;
                    break;
                case constant_out::FLOAT:
                    out << constant.f;
                    break;
                case constant_out::STRING:
                    out << json_string(constant.s);
                    break;
            }
        }
        if (!constants_.empty())
            out << "\n  ";
        out << "}";
    }

    out << "\n}\n";
    return out.str();
}

/* -------------------------------------------------------------------------
 * Driver
 * ---------------------------------------------------------------------- */

bool detect_cpp(const std::vector<token> &tokens, std::string &what) {
    static const std::set<std::string> keywords = {
        "class", "template", "namespace", "virtual", "noexcept",
        "decltype", "constexpr", "typename"};
    for (size_t i = 0; i < tokens.size(); ++i) {
        const token &value = tokens[i];
        if (value.kind == TOK_PUNCT && value.text == "::") {
            what = "::";
            return true;
        }
        if (value.kind != TOK_IDENT)
            continue;
        if (keywords.count(value.text)) {
            what = value.text;
            return true;
        }
        if ((value.text == "public" || value.text == "private" ||
             value.text == "protected") &&
            i + 1 < tokens.size() && tokens[i + 1].text == ":") {
            what = value.text + ":";
            return true;
        }
    }
    return false;
}

std::string default_module(const std::string &header) {
    fs::path path(header);
    std::string stem = path.stem().string();
    if (stem.empty())
        return "ffi";
    return stem;
}

void print_usage() {
    std::printf(
        "usage: skein --ffi-gen --header <file.h> [options]\n"
        "\n"
        "  --header <path>       header to translate (repeatable; first is the\n"
        "                        preprocessor input, others are pre-included)\n"
        "  --ffi-include <dir>   add an include directory\n"
        "  --ffi-define <def>    add a preprocessor definition\n"
        "  --cc <compiler>       C compiler used for -E (default: cc)\n"
        "  --symbols <pattern>   only emit matching function names (repeatable)\n"
        "  --module <name>       manifest library name (default: header stem)\n"
        "  -o, --output <path>   write the manifest here (default: stdout)\n"
        "  --no-constants        do not emit #define/enum constants\n"
        "  --tolerate-errors     downgrade unsupported declarations to warnings\n"
        "  --check               fail if the output file is out of date\n");
}

} // namespace

int run_ffi_gen(const std::vector<std::string> &args, std::string &error) {
    options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size())
                return std::string();
            return args[++i];
        };
        if (arg == "--header") {
            std::string header = value();
            if (header.empty()) {
                error = "--header requires a path";
                return 2;
            }
            o.headers.push_back(header);
        } else if (arg == "--ffi-include") {
            std::string dir = value();
            if (dir.empty()) {
                error = "--ffi-include requires a directory";
                return 2;
            }
            o.includes.push_back(dir);
        } else if (arg == "--ffi-define") {
            std::string define = value();
            if (define.empty()) {
                error = "--ffi-define requires a value";
                return 2;
            }
            o.defines.push_back(define);
        } else if (arg == "--symbols") {
            std::string pattern = value();
            if (pattern.empty()) {
                error = "--symbols requires a pattern";
                return 2;
            }
            o.symbols.push_back(pattern);
        } else if (arg == "--module") {
            o.module = value();
        } else if (arg == "--cc") {
            o.cc = value();
        } else if (arg == "-o" || arg == "--output") {
            o.output = value();
        } else if (arg == "--no-constants") {
            o.emit_constants = false;
        } else if (arg == "--tolerate-errors") {
            o.tolerate_errors = true;
        } else if (arg == "--check") {
            o.check = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            error = "unknown option " + arg;
            return 2;
        }
    }

    if (o.headers.empty()) {
        error = "--header is required";
        return 2;
    }
    if (o.module.empty())
        o.module = default_module(o.headers.front());

    abi_info abi;
    std::string abi_warning;
    probe_abi(o, abi, abi_warning);
    if (!abi_warning.empty())
        std::fprintf(stderr, "skein --ffi-gen: warning: %s\n",
                     abi_warning.c_str());

    std::vector<token> tokens;
    std::map<std::string, constant_out> constants;
    std::string preprocessed;
    if (!preprocess(o, abi, tokens, constants, preprocessed, error))
        return 1;

    std::string cpp_token;
    if (detect_cpp(tokens, cpp_token)) {
        error = "C++ headers are not supported (found '" + cpp_token +
                "'); provide a C header";
        return 1;
    }

    parser p(std::move(tokens), std::move(constants), abi,
             o.tolerate_errors);
    if (!p.parse(error))
        return 1;
    p.keep_symbols(o.symbols);

    char hash_buffer[32];
    std::snprintf(hash_buffer, sizeof hash_buffer, "%016llx",
                  (unsigned long long)fnv1a(preprocessed));
    std::string manifest = p.render(o, hash_buffer);

    for (const std::string &warning : p.warnings())
        std::fprintf(stderr, "skein --ffi-gen: warning: %s\n",
                     warning.c_str());
    if (p.fatal()) {
        error = "header contains unsupported declarations";
        return 1;
    }

    if (o.check) {
        if (o.output.empty()) {
            error = "--check requires --output";
            return 2;
        }
        std::ifstream input(o.output, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
        if (existing != manifest) {
            error = o.output + " is out of date";
            return 1;
        }
        return 0;
    }

    if (o.output.empty()) {
        std::fwrite(manifest.data(), 1, manifest.size(), stdout);
        return 0;
    }
    std::ofstream output(o.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "cannot write " + o.output;
        return 1;
    }
    output.write(manifest.data(), (std::streamsize)manifest.size());
    if (!output) {
        error = "cannot write " + o.output;
        return 1;
    }
    return 0;
}

} // namespace skein
