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
};

struct constant_out {
    enum kind_t { INT, FLOAT, STRING } kind = INT;
    long long i = 0;
    double f = 0.0;
    std::string s;
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

bool parse_constant_text(const std::string &text, constant_out &out) {
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
    /* Integer literal, optionally with U/L suffixes. */
    std::string int_text = value;
    while (!int_text.empty() &&
           (int_text.back() == 'u' || int_text.back() == 'U' ||
            int_text.back() == 'l' || int_text.back() == 'L'))
        int_text.pop_back();
    if (!int_text.empty()) {
        char *end = NULL;
        long long parsed = std::strtoll(int_text.c_str(), &end, 0);
        if (end && *end == '\0') {
            out.kind = constant_out::INT;
            out.i = parsed;
            return true;
        }
    }
    if (value.find('.') != std::string::npos ||
        value.find('e') != std::string::npos ||
        value.find('E') != std::string::npos) {
        char *end = NULL;
        double parsed = std::strtod(value.c_str(), &end);
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

bool preprocess(const options &o, std::vector<token> &tokens,
                std::map<std::string, constant_out> &constants,
                std::string &preprocessed, std::string &error) {
    std::string cc = o.cc.empty() ? "cc" : o.cc;
    std::ostringstream command;
    command << shell_quote(cc) << " -E -x c -std=gnu11 -dD";
    for (const std::string &include : o.includes)
        command << " -I" << shell_quote(include);
    for (const std::string &define : o.defines)
        command << " -D" << shell_quote(define);
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
                    constant_out constant;
                    if (parse_constant_text(trimmed.substr(i), constant))
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
};

bool is_type_keyword(const std::string &word) {
    static const std::set<std::string> keywords = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "_Bool", "bool", "_Complex", "__signed__",
        "__unsigned__", "__int8", "__int16", "__int32", "__int64"};
    return keywords.count(word) != 0;
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

struct declarator {
    std::string name;
    int pointers = 0;
    std::vector<std::size_t> arrays;
    bool is_callback = false;
    std::vector<ctype_ptr> params;
    bool variadic = false;
    bool function = false;
};

struct spec {
    ctype_ptr base;
    bool is_typedef = false;
    bool packed = false;
    bool unsupported_convention = false;
};

class parser {
public:
    parser(std::vector<token> tokens,
           std::map<std::string, constant_out> constants)
        : tokens_(std::move(tokens)), constants_(std::move(constants)) {
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
                warnings_.push_back(e.message);
                position_ = start;
                skip_declaration();
                if (position_ == start)
                    ++position_;
            }
        }
        error.clear();
        return true;
    }

    std::string render(const options &o, const std::string &source_hash);

    void keep_functions(const std::vector<std::string> &patterns);

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
        typedefs_["intptr_t"] = make_scalar("isize");
        typedefs_["uintptr_t"] = make_scalar("usize");
        typedefs_["ptrdiff_t"] = make_scalar("isize");
        typedefs_["size_t"] = make_scalar("usize");
        typedefs_["ssize_t"] = make_scalar("isize");
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

    [[noreturn]] void fail(const std::string &message) {
        throw parse_error{message};
    }

    void advance() {
        if (position_ < tokens_.size())
            ++position_;
    }

    bool ident_at(size_t offset = 0) const {
        return position_ + offset < tokens_.size() &&
               tokens_[position_ + offset].kind == TOK_IDENT;
    }

    void skip_attributes(bool &packed) {
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
                        if (cur().text == "packed")
                            packed = true;
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
            return make_scalar(is_unsigned ? "u16" : "i16");
        if (longs >= 2)
            return make_scalar(is_unsigned ? "u64" : "i64");
        if (longs == 1)
            return make_scalar(is_unsigned
                                   ? (sizeof(long) == 8 ? "u64" : "u32")
                                   : (sizeof(long) == 8 ? "i64" : "i32"));
        (void)is_int;
        return make_scalar(is_unsigned ? "u32" : "i32");
    }

    ctype_ptr parse_tag(bool &outer_packed) {
        std::string keyword = cur().text;
        advance();
        bool packed = false;
        skip_attributes(packed);
        std::string tag;
        if (ident_at() && !is("{"))
            tag = cur().text, advance();

        ctype_ptr result;
        if (accept("{")) {
            if (keyword == "enum") {
                parse_enum_body();
                result = make_scalar("i32");
            } else if (keyword == "union") {
                skip_declaration();
                fail("unions are not supported");
            } else {
                result = parse_struct_body(tag, packed || outer_packed);
            }
        } else {
            result = record_ref(tag);
        }
        skip_attributes(packed);
        return result;
    }

    ctype_ptr parse_struct_body(const std::string &tag, bool packed) {
        ctype_ptr record = std::make_shared<ctype>();
        record->kind = ctype::RECORD;
        record->name = tag;
        if (packed)
            fail("packed structs are not supported");

        record_out out;
        out.name = tag;

        while (!eof() && !accept("}")) {
            if (cur().pack != 0)
                fail("packed structs are not supported");
            spec s = parse_specifiers();
            if (accept(";")) {
                if (s.base && s.base->kind == ctype::RECORD &&
                    s.base->name.rfind("__anon_", 0) == 0)
                    fail("anonymous struct members are not supported");
                continue;
            }
            for (;;) {
                declarator d = parse_declarator();
                if (accept(":"))
                    fail("bitfields are not supported");
                if (d.name.empty())
                    fail("anonymous struct members are not supported");
                if (d.is_callback)
                    fail("function pointer fields are not supported");
                for (std::size_t dim : d.arrays)
                    if (dim == 0)
                        fail("flexible array members are not supported");
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
            fail("anonymous struct without a declarator is not supported");
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
                bool negative = accept("-");
                if (is("+"))
                    advance();
                if (!ident_at() && cur().kind != TOK_NUMBER)
                    fail("unsupported enumerator value");
                std::string text = cur().text;
                advance();
                char *end = NULL;
                long long parsed = std::strtoll(text.c_str(), &end, 0);
                if (!end || *end != '\0')
                    fail("unsupported enumerator value");
                value = negative ? -parsed : parsed;
            }
            constant_out constant;
            constant.kind = constant_out::INT;
            constant.i = value;
            constants_[name] = constant;
            next = value + 1;
            if (!accept(","))
                break;
        }
    }

    spec parse_specifiers() {
        spec s;
        std::vector<std::string> words;
        bool saw_base = false;
        bool packed = false;

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
            if (text == "static" || text == "extern" || text == "register" ||
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
                skip_attributes(packed);
                continue;
            }
            if (is_calling_convention(text)) {
                s.unsupported_convention = true;
                advance();
                continue;
            }
            if (text == "struct" || text == "union" || text == "enum") {
                s.base = parse_tag(packed);
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
                fail("unknown type '" + text + "'");
            }
            break;
        }

        if (!saw_base) {
            if (words.empty())
                fail("missing type specifier");
            s.base = scalar_from_words(words);
        }
        s.packed = packed;
        return s;
    }

    declarator parse_declarator() {
        declarator d;
        while (accept("*")) {
            ++d.pointers;
            bool ignored = false;
            skip_attributes(ignored);
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
            declarator d = parse_declarator();
            if (d.variadic)
                variadic = true;
            if (d.is_callback) {
                ctype_ptr result = apply_pointers(s.base, d.pointers);
                params.push_back(make_callback(result, d.params));
            } else {
                params.push_back(apply_type(s.base, d));
            }
            if (!accept(","))
                break;
        }
    }

    ctype_ptr apply_pointers(ctype_ptr base, int pointers) {
        ctype_ptr type = std::move(base);
        for (int i = 0; i < pointers; ++i) {
            if (type && type->kind == ctype::SCALAR &&
                (type->name == "i8" || type->name == "u8")) {
                type = make_scalar("cstring");
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

    void parse_typedef() {
        expect("typedef");
        spec s = parse_specifiers();
        for (;;) {
            declarator d = parse_declarator();
            if (d.name.empty())
                fail("typedef requires a name");
            if (d.is_callback) {
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
        for (;;) {
            declarator d = parse_declarator();
            if (d.name.empty())
                fail("declaration requires a name");
            if (d.function) {
                if (d.variadic)
                    fail("variadic functions are not supported");
                if (s.unsupported_convention)
                    fail("unsupported calling convention");
                function_out function;
                function.name = d.name;
                function.result = apply_pointers(s.base, d.pointers);
                function.args = d.params;
                functions_.push_back(std::move(function));
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
    std::map<std::string, ctype_ptr> typedefs_;
    std::map<std::string, record_out> records_;
    std::map<std::string, callback_out> callbacks_;
    std::vector<function_out> functions_;
    std::set<std::string> referenced_records_;
    std::vector<std::string> warnings_;
    size_t position_ = 0;
    size_t anon_counter_ = 0;

public:
    const std::vector<std::string> &warnings() const { return warnings_; }
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

void parser::keep_functions(const std::vector<std::string> &patterns) {
    if (patterns.empty())
        return;
    functions_.erase(
        std::remove_if(functions_.begin(), functions_.end(),
                       [&](const function_out &function) {
                           for (const std::string &pattern : patterns) {
                               if (glob_match(pattern.c_str(),
                                              function.name.c_str()))
                                   return false;
                           }
                           return true;
                       }),
        functions_.end());
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
        std::string text = "    " + json_string(callback.name) + ": " +
                           render_callback(callback.result, callback.params);
        type_entries.push_back(std::move(text));
    }
    for (const std::string &name : referenced_records_) {
        if (records_.count(name))
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

    std::vector<token> tokens;
    std::map<std::string, constant_out> constants;
    std::string preprocessed;
    if (!preprocess(o, tokens, constants, preprocessed, error))
        return 1;

    parser p(std::move(tokens), std::move(constants));
    if (!p.parse(error))
        return 1;
    p.keep_functions(o.symbols);

    char hash_buffer[32];
    std::snprintf(hash_buffer, sizeof hash_buffer, "%016llx",
                  (unsigned long long)fnv1a(preprocessed));
    std::string manifest = p.render(o, hash_buffer);

    for (const std::string &warning : p.warnings())
        std::fprintf(stderr, "skein --ffi-gen: warning: %s\n",
                     warning.c_str());

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
