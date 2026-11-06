%option noyywrap nodefault yylineno noinput nounput nounistd
%x STRING
%x CHARLIT

%{
#ifndef YY_NO_UNISTD_H
#define YY_NO_UNISTD_H
#endif
#ifdef _WIN32
#include <io.h>
#ifndef isatty
#define isatty _isatty
#endif
#ifndef fileno
#define fileno _fileno
#endif
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"
#include "parser.hpp"
#include "repl.h"
#include "unicode.h"
#include "unicode_ident.h"

int indent_count = 0;

static std::string string_buffer;

/* Location of the opening quote of the string currently being scanned. */
static int string_start_line = 1;
static int string_start_column = 1;

static std::string char_buffer;
static int char_start_line = 1;
static int char_start_column = 1;

static int last_token = 0;
static int prev_token = 0;

struct brace_frame {
    bool is_dict;
    bool empty;
};

/*
 * Tracks the lexical kind of every currently open brace.  The kind is only
 * consulted when the brace turns out to be empty; non-empty braces use the
 * normal '{' ... '}' path and let the parser decide from their contents.
 */
static std::vector<brace_frame> brace_stack;

#define YYRET(tok) do { int yyret_tok = (tok); note_token(yyret_tok); prev_token = last_token; last_token = yyret_tok; return yyret_tok; } while (0)

#define YY_USER_ACTION diag_scan_token(yytext, yyleng);

// 当 stdin 不是终端（管道、IDE 运行窗口等）时，内核不会处理退格，
// 需要自己按行读出并删除 \b / 0x7f 前的一个字符。
static std::string input_line;
static size_t input_pos = 0;

enum input_status {
    INPUT_LINE,       // 读到完整的一行（含 \n）
    INPUT_PARTIAL,    // 读到 EOF，但还有一行没有换行符
    INPUT_EOF,        // 真正的输入结束
    INPUT_INTERRUPTED // 被 Ctrl-C 打断
};

static void strip_utf8_bom_once() {
    static bool checked = false;
    if (checked)
        return;
    checked = true;
    if (input_line.size() >= 3 &&
        (unsigned char)input_line[0] == 0xEF &&
        (unsigned char)input_line[1] == 0xBB &&
        (unsigned char)input_line[2] == 0xBF) {
        input_line.erase(0, 3);
    }
}

static input_status read_input_line() {
    input_line.clear();
    input_pos = 0;
    diag_start_input_line();

    if (g_show_prompt && (yyin == NULL || yyin == stdin)) {
        repl_line_status st = repl_read_line(input_line, indent_count > 0);
        if (st == REPL_EOF) {
            input_line.clear();
            input_pos = 0;
            g_stdin_eof = 1;
            return INPUT_EOF;
        }
        if (st == REPL_INTERRUPTED) {
            input_line.clear();
            input_pos = 0;
            std::clearerr(yyin);
            return INPUT_INTERRUPTED;
        }
        input_line.push_back('\n');
        strip_utf8_bom_once();
        diag_record_line(diag_current_line(), input_line);
        return INPUT_LINE;
    }

    for (;;) {
        if (g_interrupted) {
            std::clearerr(yyin);
            return INPUT_INTERRUPTED;
        }

        errno = 0;
        int c = std::fgetc(yyin);
        if (c == EOF) {
            if (g_interrupted) {
                std::clearerr(yyin);
                return INPUT_INTERRUPTED;
            }
#ifdef EINTR
            if (std::ferror(yyin) && errno == EINTR) {
                std::clearerr(yyin);
                g_interrupted = 1;
                return INPUT_INTERRUPTED;
            }
#endif
            if (input_line.empty())
                return INPUT_EOF;
            strip_utf8_bom_once();
            diag_record_line(diag_current_line(), input_line);
            return INPUT_PARTIAL;
        }
        if (c == '\n') {
            input_line.push_back('\n');
            strip_utf8_bom_once();
            diag_record_line(diag_current_line(), input_line);
            return INPUT_LINE;
        }
        if (c == '\b' || c == 0x7f) {
            if (g_interactive && !input_line.empty())
                input_line.pop_back();
            if (g_interactive)
                continue;
        }
        input_line.push_back(static_cast<char>(c));
    }
}

#define YY_INPUT(buf, result, max_size)                     \
    do {                                                    \
        if (g_interrupted) {                                \
            result = YY_NULL;                               \
        } else {                                            \
            if (input_pos >= input_line.size()) {           \
                input_status st = read_input_line();        \
                if (st == INPUT_EOF)                        \
                    g_stdin_eof = 1;                        \
            }                                               \
            size_t n = 0;                                   \
            while (n < static_cast<size_t>(max_size) &&     \
                   input_pos < input_line.size())           \
                buf[n++] = input_line[input_pos++];         \
            result = static_cast<int>(n);                   \
        }                                                   \
    } while (0)

static void append_unescaped(const char *s, int len) {
    for (int i = 0; i < len; ++i) {
        if (s[i] == '\\' && i + 1 < len) {
            ++i;
            switch (s[i]) {
                case 'n': string_buffer += '\n'; break;
                case 't': string_buffer += '\t'; break;
                case 'r': string_buffer += '\r'; break;
                case '\\': string_buffer += '\\'; break;
                case '"': string_buffer += '"'; break;
                case '\'': string_buffer += '\''; break;
                default: string_buffer += s[i]; break;
            }
        } else {
            string_buffer += s[i];
        }
    }
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_char_literal(const std::string &text, unsigned int &codepoint,
                               std::string &error) {
    size_t pos = 0;
    bool have_value = false;
    unsigned int value = 0;

    while (pos < text.size()) {
        if (have_value) {
            error = "character literal must contain exactly one character";
            return false;
        }

        unsigned char c = (unsigned char)text[pos];
        if (c == '\\') {
            ++pos;
            if (pos >= text.size()) {
                error = "invalid escape in character literal";
                return false;
            }
            char esc = text[pos++];
            switch (esc) {
                case 'n': value = '\n'; break;
                case 't': value = '\t'; break;
                case 'r': value = '\r'; break;
                case '\\': value = '\\'; break;
                case '\'': value = '\''; break;
                case '"': value = '"'; break;
                case '0': value = 0; break;
                case 'u': {
                    if (pos < text.size() && text[pos] == '{') {
                        ++pos;
                        size_t start = pos;
                        while (pos < text.size() && text[pos] != '}')
                            ++pos;
                        if (pos >= text.size() || pos == start) {
                            error = "invalid \\u{...} escape";
                            return false;
                        }
                        std::string hex = text.substr(start, pos - start);
                        ++pos; /* skip '}' */
                        if (hex.size() > 6) {
                            error = "invalid \\u{...} escape";
                            return false;
                        }
                        unsigned int parsed = 0;
                        for (char h : hex) {
                            int digit = hex_digit_value(h);
                            if (digit < 0) {
                                error = "invalid \\u{...} escape";
                                return false;
                            }
                            parsed = (parsed << 4) | (unsigned int)digit;
                        }
                        value = parsed;
                    } else {
                        if (pos + 4 > text.size()) {
                            error = "invalid \\uXXXX escape";
                            return false;
                        }
                        unsigned int parsed = 0;
                        for (int i = 0; i < 4; ++i) {
                            int digit = hex_digit_value(text[pos++]);
                            if (digit < 0) {
                                error = "invalid \\uXXXX escape";
                                return false;
                            }
                            parsed = (parsed << 4) | (unsigned int)digit;
                        }
                        value = parsed;
                    }
                    break;
                }
                default:
                    error = "invalid escape in character literal";
                    return false;
            }
        } else {
            size_t consumed = 0;
            unsigned int decoded = 0;
            if (!utf8_decode_one(text.data() + pos, text.size() - pos,
                                 consumed, decoded)) {
                error = "invalid UTF-8 in character literal";
                return false;
            }
            pos += consumed;
            value = decoded;
        }
        have_value = true;
    }

    if (!have_value) {
        error = "empty character literal";
        return false;
    }
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        error = "character literal is not a Unicode scalar value";
        return false;
    }

    codepoint = value;
    return true;
}

static bool ascii_name(const char *text, int len) {
    for (int i = 0; i < len; ++i) {
        if ((unsigned char)text[i] >= 0x80)
            return false;
    }
    return true;
}

/* Numeric literals may contain underscores between digits.  Parsing always
 * happens on a copy with the separators removed. */
static std::string strip_numeric_underscores(const char *text, int len) {
    std::string out;
    out.reserve((size_t)len);
    for (int i = 0; i < len; ++i) {
        if (text[i] != '_')
            out.push_back(text[i]);
    }
    return out;
}

static int lex_decimal_integer(const char *text, int len) {
    std::string clean = strip_numeric_underscores(text, len);
    errno = 0;
    long long value = std::strtoll(clean.c_str(), NULL, 10);
    if (errno == ERANGE) {
        yylval.d = std::strtod(clean.c_str(), NULL);
        return FLOAT;
    }
    yylval.i = value;
    return INT;
}

static int lex_prefixed_integer(const char *text, int len, int base,
                                const char *kind) {
    std::string clean = strip_numeric_underscores(text, len);
    errno = 0;
    unsigned long long value = std::strtoull(clean.c_str() + 2, NULL, base);
    if (errno == ERANGE || value > (unsigned long long)LLONG_MAX) {
        diag_report_lexical(g_token_line, g_token_column,
                            "%s integer literal out of range", kind);
        yylval.i = 0;
        return INT;
    }
    yylval.i = (long long)value;
    return INT;
}

static int lex_float(const char *text, int len) {
    std::string clean = strip_numeric_underscores(text, len);
    yylval.d = std::strtod(clean.c_str(), NULL);
    return FLOAT;
}

/* Copy a token of explicit byte length into yylval.str, NFC-normalizing
 * non-ASCII identifiers. */
static int make_name_token(const char *text, int len) {
    std::string name;
    if (ascii_name(text, len)) {
        name.assign(text, (size_t)len);
    } else {
        std::string error;
        if (!normalize_nfc(std::string(text, (size_t)len), name, error)) {
            diag_report_lexical(g_token_line, g_token_column,
                                "identifier normalization failed: %s",
                                error.c_str());
            return 0;
        }
        unicode_ident::scan_result scanned =
            unicode_ident::scan_identifier(name.data(), name.size());
        if (!scanned.valid) {
            diag_report_lexical(g_token_line, g_token_column,
                                "identifier is not valid after NFC normalization");
            return 0;
        }
    }

    char *raw = (char *)std::malloc(name.size() + 1);
    if (!raw) {
        yyerror("out of space");
        return 0;
    }
    std::memcpy(raw, name.data(), name.size());
    raw[name.size()] = '\0';
    yylval.str = raw;
    return NAME;
}
/*
 * Decide whether an opening brace introduces a block body.  Only syntactic
 * positions that always require a body are treated as blocks; everything else
 * is treated as a dict when the brace is empty.  For non-empty braces this
 * choice is irrelevant because the closing brace stays a normal '}'.
 */
static bool brace_starts_block() {
    if (last_token == NAME && prev_token == PACKAGE)
        return true;
    if (last_token == ')' || last_token == ELSE || last_token == DO ||
        last_token == ARROW)
        return true;
    return false;
}

/*
 * Any real token between braces makes the innermost brace non-empty.
 * Comments, whitespace and newlines never reach this function.
 */
static void note_token(int tok) {
    if (brace_stack.empty())
        return;
    if (tok == '{' || tok == '}' ||
        tok == EMPTY_DICT_CLOSE || tok == EMPTY_BLOCK_CLOSE ||
        tok == EOL)
        return;
    brace_stack.back().empty = false;
}

%}

DIGITS [0-9](_?[0-9])*
HEX_DIGITS [0-9A-Fa-f](_?[0-9A-Fa-f])*
BIN_DIGITS [01](_?[01])*
OCT_DIGITS [0-7](_?[0-7])*

EXP ([Ee][-+]?{DIGITS})

ASCII_NAME [A-Za-z_][A-Za-z0-9_]*

UTF8_2 [\xC2-\xDF][\x80-\xBF]
UTF8_3 [\xE0-\xEF][\x80-\xBF]{2}
UTF8_4 [\xF0-\xF4][\x80-\xBF]{3}
UTF8_SEQ ({UTF8_2}|{UTF8_3}|{UTF8_4})

UNICODE_NAME ([A-Za-z_]|{UTF8_SEQ})([A-Za-z0-9_]|{UTF8_SEQ})*

%%

^"#!"[^\n]*\n? { }

"++" { YYRET(INC); }
"--" { YYRET(DEC); }

"+=" { YYRET(PLUS_ASSIGN); }
"-=" { YYRET(MINUS_ASSIGN); }
"*=" { YYRET(MUL_ASSIGN); }
"/=" { YYRET(DIV_ASSIGN); }
"%=" { YYRET(MOD_ASSIGN); }
"&=" { YYRET(AND_ASSIGN); }
"|=" { YYRET(OR_ASSIGN); }
"|>>" { YYRET(PIPE_APPLY); }
"|>" { YYRET(PIPE); }
"^=" { YYRET(XOR_ASSIGN); }
"<<=" { YYRET(SHL_ASSIGN); }
">>=" { YYRET(SHR_ASSIGN); }

"+" |
"-" |
"*" |
"/" |
"%" |
"=" |
"," |
":" |
";" |
"&" |
"|" |
"^" |
"~" |
"!" |
"@" |
"?" { YYRET(yytext[0]); }

"$" { YYRET(PLACEHOLDER); }

"..." { YYRET(ELLIPSIS); }

".." { YYRET(DOTDOT); }

"=>" { YYRET(ARROW); }
"->" { YYRET(RARROW); }

"::" { YYRET(COLONCOLON); }

"&&" { YYRET(LOGAND); }
"||" { YYRET(LOGOR); }
"<<" { YYRET(SHL); }
">>" { YYRET(SHR); }

"{" {
    if (!brace_stack.empty())
        brace_stack.back().empty = false;
    brace_stack.push_back(brace_frame{!brace_starts_block(), true});
    indent_count++;
    YYRET('{');
}

"}" {
    if (indent_count > 0) indent_count--;
    int tok = '}';
    if (!brace_stack.empty()) {
        brace_frame frame = brace_stack.back();
        brace_stack.pop_back();
        if (frame.empty)
            tok = frame.is_dict ? EMPTY_DICT_CLOSE : EMPTY_BLOCK_CLOSE;
    }
    YYRET(tok);
}
"[" { indent_count++; YYRET('['); }
"]" { if (indent_count > 0) indent_count--; YYRET(']'); }
"(" { indent_count++; YYRET('('); }
")" { if (indent_count > 0) indent_count--; YYRET(')'); }

">" { yylval.fn = 1; YYRET(CMP); }
"<" { yylval.fn = 2; YYRET(CMP); }
"<>" |
"!=" { yylval.fn = 3; YYRET(CMP); }
"==" { yylval.fn = 4; YYRET(CMP); }
">=" { yylval.fn = 5; YYRET(CMP); }
"<=" { yylval.fn = 6; YYRET(CMP); }

"if" { YYRET(IF); }
"else" { YYRET(ELSE); }
"while" { YYRET(WHILE); }
"do" { YYRET(DO); }
"for" { YYRET(FOR); }
"struct" { YYRET(STRUCT); }
"extern" { YYRET(EXTERN); }
"enum" { YYRET(ENUM); }
"impl" { YYRET(IMPL); }
"trait" { YYRET(TRAIT); }
"switch" { YYRET(SWITCH); }
"match" { YYRET(MATCH); }
"case" { YYRET(CASE); }
"default" { YYRET(DEFAULT); }
"let" { YYRET(LET); }
"const" { YYRET(CONST); }
"func" { YYRET(FUNC); }
"async" { YYRET(ASYNC); }
"await" { YYRET(AWAIT); }
"yield" { YYRET(YIELD); }
"defer" { YYRET(DEFER); }
"package" { YYRET(PACKAGE); }
"import" { YYRET(IMPORT); }
"export" { YYRET(EXPORT); }
"return" { YYRET(RETURN); }
"break" { YYRET(BREAK); }
"continue" { YYRET(CONTINUE); }
"fail" { YYRET(FAIL); }
"is" { YYRET(IS); }
"label" { YYRET(LABEL); }
"on" { YYRET(ON); }
"error" { YYRET(ERROR); }
"recover" { YYRET(RECOVER); }
"pass" { YYRET(PASS); }
"in" { YYRET(IN); }
"as" { YYRET(AS); }
"null" { YYRET(NULLVAL); }
"true" { YYRET(TRUE); }
"false" { YYRET(FALSE); }

"sqrt" { yylval.fn = B_sqrt; YYRET(BUILTIN); }
"exp" { yylval.fn = B_exp; YYRET(BUILTIN); }
"log" { yylval.fn = B_log; YYRET(BUILTIN); }
"abs" { yylval.fn = B_abs; YYRET(BUILTIN); }
"sin" { yylval.fn = B_sin; YYRET(BUILTIN); }
"cos" { yylval.fn = B_cos; YYRET(BUILTIN); }
"tan" { yylval.fn = B_tan; YYRET(BUILTIN); }
"asin" { yylval.fn = B_asin; YYRET(BUILTIN); }
"acos" { yylval.fn = B_acos; YYRET(BUILTIN); }
"atan" { yylval.fn = B_atan; YYRET(BUILTIN); }
"atan2" { yylval.fn = B_atan2; YYRET(BUILTIN); }
"pow" { yylval.fn = B_pow; YYRET(BUILTIN); }
"floor" { yylval.fn = B_floor; YYRET(BUILTIN); }
"ceil" { yylval.fn = B_ceil; YYRET(BUILTIN); }
"round" { yylval.fn = B_round; YYRET(BUILTIN); }
"trunc" { yylval.fn = B_trunc; YYRET(BUILTIN); }
"log2" { yylval.fn = B_log2; YYRET(BUILTIN); }
"log10" { yylval.fn = B_log10; YYRET(BUILTIN); }
"hypot" { yylval.fn = B_hypot; YYRET(BUILTIN); }
"min" { yylval.fn = B_min; YYRET(BUILTIN); }
"max" { yylval.fn = B_max; YYRET(BUILTIN); }
"len" { yylval.fn = B_len; YYRET(BUILTIN); }
"range" { yylval.fn = B_range; YYRET(BUILTIN); }
"print" { yylval.fn = B_print; YYRET(BUILTIN); }
"println" { yylval.fn = B_println; YYRET(BUILTIN); }
"list"/"." {
    int token = make_name_token("list", 4);
    if (token)
        YYRET(token);
    yylval.fn = B_list;
    YYRET(BUILTIN);
}
"list" { yylval.fn = B_list; YYRET(BUILTIN); }
"dict"/"." {
    int token = make_name_token("dict", 4);
    if (token)
        YYRET(token);
    yylval.fn = B_dict;
    YYRET(BUILTIN);
}
"dict" { yylval.fn = B_dict; YYRET(BUILTIN); }
"int" { yylval.fn = B_int; YYRET(BUILTIN); }
"float" { yylval.fn = B_float; YYRET(BUILTIN); }
"bool" { yylval.fn = B_bool; YYRET(BUILTIN); }
"str" { yylval.fn = B_str; YYRET(BUILTIN); }
"char" { yylval.fn = B_char; YYRET(BUILTIN); }
"byte" { yylval.fn = B_byte; YYRET(BUILTIN); }
"kind_of" { yylval.fn = B_kind_of; YYRET(BUILTIN); }
"type_name" { yylval.fn = B_type_name; YYRET(BUILTIN); }
"type_module" { yylval.fn = B_type_module; YYRET(BUILTIN); }
"type_source" { yylval.fn = B_type_source; YYRET(BUILTIN); }
"format" { yylval.fn = B_format; YYRET(BUILTIN); }
"read_file" { yylval.fn = B_read_file; YYRET(BUILTIN); }
"write_file" { yylval.fn = B_write_file; YYRET(BUILTIN); }
"append_file" { yylval.fn = B_append_file; YYRET(BUILTIN); }
"file_exists" { yylval.fn = B_file_exists; YYRET(BUILTIN); }
"file_open" { yylval.fn = B_file_open; YYRET(BUILTIN); }
"file_read" { yylval.fn = B_file_read; YYRET(BUILTIN); }
"file_write" { yylval.fn = B_file_write; YYRET(BUILTIN); }
"file_seek" { yylval.fn = B_file_seek; YYRET(BUILTIN); }
"file_tell" { yylval.fn = B_file_tell; YYRET(BUILTIN); }
"file_eof" { yylval.fn = B_file_eof; YYRET(BUILTIN); }
"file_close" { yylval.fn = B_file_close; YYRET(BUILTIN); }
"file_flush" { yylval.fn = B_file_flush; YYRET(BUILTIN); }
"input" { yylval.fn = B_input; YYRET(BUILTIN); }
"split" { yylval.fn = B_split; YYRET(BUILTIN); }
"split_ws" { yylval.fn = B_split_ws; YYRET(BUILTIN); }
"trim" { yylval.fn = B_trim; YYRET(BUILTIN); }
"find" { yylval.fn = B_find; YYRET(BUILTIN); }
"substr" { yylval.fn = B_substr; YYRET(BUILTIN); }
"bytes_from_string" { yylval.fn = B_bytes_from_string; YYRET(BUILTIN); }
"bytes_to_string" { yylval.fn = B_bytes_to_string; YYRET(BUILTIN); }
"bytes_from_char" { yylval.fn = B_bytes_from_char; YYRET(BUILTIN); }
"char_from_bytes" { yylval.fn = B_char_from_bytes; YYRET(BUILTIN); }
"bytes_len" { yylval.fn = B_bytes_len; YYRET(BUILTIN); }
"bytes_get" { yylval.fn = B_bytes_get; YYRET(BUILTIN); }
"bytes_slice" { yylval.fn = B_bytes_slice; YYRET(BUILTIN); }
"bytes_concat" { yylval.fn = B_bytes_concat; YYRET(BUILTIN); }
"bytes_hex" { yylval.fn = B_bytes_hex; YYRET(BUILTIN); }
"bytes_from_hex" { yylval.fn = B_bytes_from_hex; YYRET(BUILTIN); }
"bytes_to_int" { yylval.fn = B_bytes_to_int; YYRET(BUILTIN); }
"int_to_bytes" { yylval.fn = B_int_to_bytes; YYRET(BUILTIN); }
"read_bytes" { yylval.fn = B_read_bytes; YYRET(BUILTIN); }
"write_bytes" { yylval.fn = B_write_bytes; YYRET(BUILTIN); }
"append_bytes" { yylval.fn = B_append_bytes; YYRET(BUILTIN); }
"file_read_bytes" { yylval.fn = B_file_read_bytes; YYRET(BUILTIN); }
"file_write_bytes" { yylval.fn = B_file_write_bytes; YYRET(BUILTIN); }

{ASCII_NAME} {
    int tok = make_name_token(yytext, yyleng);
    if (tok == 0)
        return 0;
    YYRET(tok);
}

{UNICODE_NAME} {
    unicode_ident::scan_result scanned =
        unicode_ident::scan_identifier(yytext, (size_t)yyleng);

    if (scanned.valid) {
        int tok = make_name_token(yytext, yyleng);
        if (tok == 0)
            return 0;
        YYRET(tok);
    }

    if (scanned.valid_bytes > 0) {
        int keep = (int)scanned.valid_bytes;
        diag_scan_rewind(yytext + keep, yyleng - keep);
        yyless(keep);
        int tok = make_name_token(yytext, keep);
        if (tok == 0)
            return 0;
        YYRET(tok);
    }

    if (scanned.bad_codepoint != 0)
        diag_report_lexical(g_token_line, g_token_column,
                            "identifier cannot start with U+%04X",
                            (unsigned int)scanned.bad_codepoint);
    else
        diag_report_lexical(g_token_line, g_token_column,
                            "invalid UTF-8 byte 0x%02x in identifier; "
                            "source must be UTF-8 or use --encoding <name>",
                            (unsigned char)yytext[scanned.bad_offset]);

    int bad = (int)scanned.bad_length;
    diag_scan_rewind(yytext + bad, yyleng - bad);
    yyless(bad);
}

0[xX]{HEX_DIGITS} { YYRET(lex_prefixed_integer(yytext, yyleng, 16, "hexadecimal")); }
0[bB]{BIN_DIGITS} { YYRET(lex_prefixed_integer(yytext, yyleng, 2, "binary")); }
0[oO]{OCT_DIGITS} { YYRET(lex_prefixed_integer(yytext, yyleng, 8, "octal")); }

0[xX][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid hexadecimal integer literal");
    yylval.i = 0;
    YYRET(INT);
}
0[bB][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid binary integer literal");
    yylval.i = 0;
    YYRET(INT);
}
0[oO][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid octal integer literal");
    yylval.i = 0;
    YYRET(INT);
}

{DIGITS}".." { yyless((int)(yyleng - 2)); YYRET(lex_decimal_integer(yytext, yyleng)); }
{DIGITS} { YYRET(lex_decimal_integer(yytext, yyleng)); }

({DIGITS}"."({DIGITS})?|"."{DIGITS})({EXP})? { YYRET(lex_float(yytext, yyleng)); }
{DIGITS}{EXP} { YYRET(lex_float(yytext, yyleng)); }

[0-9][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid numeric literal");
    yylval.i = 0;
    YYRET(INT);
}

"." { YYRET('.'); }

"//".*
[ \t\r] {}

\" {
    string_start_line = g_token_line;
    string_start_column = g_token_column;
    string_buffer.clear();
    BEGIN(STRING);
}

<STRING>{
    \n {
        diag_report_lexical(string_start_line, string_start_column,
                            "unterminated string literal");
        string_buffer.clear();
        BEGIN(INITIAL);
        YYRET(EOL);
    }
    <<EOF>> {
        diag_report_lexical(string_start_line, string_start_column,
                            "unterminated string literal at end of input");
        string_buffer.clear();
        BEGIN(INITIAL);
        yyterminate();
    }
    ([^\\\"\n]|\\.)+ {
        append_unescaped(yytext, (int)yyleng);
    }
    \" {
        BEGIN(INITIAL);
        size_t len = string_buffer.size();
        yylval.str = (char *)std::malloc(len + 1);
        if (!yylval.str) {
            yyerror("out of space");
            return 0;
        }
        std::memcpy(yylval.str, string_buffer.c_str(), len);
        yylval.str[len] = '\0';
        YYRET(XSTRING);
    }
    . {
        append_unescaped(yytext, (int)yyleng);
    }
}

"'" {
    char_start_line = g_token_line;
    char_start_column = g_token_column;
    char_buffer.clear();
    BEGIN(CHARLIT);
}

<CHARLIT>{
    \n {
        diag_report_lexical(char_start_line, char_start_column,
                            "unterminated character literal");
        char_buffer.clear();
        BEGIN(INITIAL);
        YYRET(EOL);
    }
    <<EOF>> {
        diag_report_lexical(char_start_line, char_start_column,
                            "unterminated character literal at end of input");
        char_buffer.clear();
        BEGIN(INITIAL);
        yyterminate();
    }
    ([^\\'\n]|\\.)+ {
        char_buffer += yytext;
    }
    ' {
        BEGIN(INITIAL);
        unsigned int codepoint = 0;
        std::string error;
        if (!parse_char_literal(char_buffer, codepoint, error)) {
            diag_report_lexical(char_start_line, char_start_column, "%s",
                                error.c_str());
            yylval.i = 0;
        } else {
            yylval.i = (long long)codepoint;
        }
        char_buffer.clear();
        YYRET(CHAR);
    }
    . {
        char_buffer += yytext;
    }
}

\n {
    if (indent_count == 0)
        YYRET(EOL);
}

. {
    unsigned char c = (unsigned char)yytext[0];
    if (c < 0x80) {
        if (std::isprint(c))
            diag_report_lexical(g_token_line, g_token_column,
                                "unexpected character '%c' (0x%02x)", c, c);
        else
            diag_report_lexical(g_token_line, g_token_column,
                                "unexpected byte 0x%02x", c);
    } else {
        diag_report_lexical(g_token_line, g_token_column,
                            "invalid UTF-8 byte 0x%02x; source must be "
                            "UTF-8 or use --encoding <name>", c);
    }
}

%%

void reset_scanner_state(std::FILE *in) {
    BEGIN(INITIAL);
    string_buffer.clear();
    clear_func_locations();
    input_line.clear();
    input_pos = 0;
    last_token = 0;
    prev_token = 0;
    brace_stack.clear();
    std::clearerr(in);
    yyrestart(in);
}

struct scanner_checkpoint {
    std::FILE *in;
    std::string input;
    size_t position;
    std::string strings;
    int string_line;
    int string_column;
    int condition;
    int indent;
    int line;
    int stdin_eof;
    int last_token;
    int prev_token;
    std::vector<brace_frame> braces;
};

scanner_checkpoint *scanner_checkpoint_save() {
    scanner_checkpoint *checkpoint = new scanner_checkpoint;
    checkpoint->in = yyin;
    checkpoint->input = input_line;
    checkpoint->position = input_pos;
    checkpoint->strings = string_buffer;
    checkpoint->string_line = string_start_line;
    checkpoint->string_column = string_start_column;
    checkpoint->condition = YY_START;
    checkpoint->indent = indent_count;
    checkpoint->line = yylineno;
    checkpoint->stdin_eof = g_stdin_eof;
    checkpoint->last_token = last_token;
    checkpoint->prev_token = prev_token;
    checkpoint->braces = brace_stack;
    return checkpoint;
}

void scanner_checkpoint_restore(scanner_checkpoint *checkpoint) {
    if (!checkpoint)
        return;
    reset_scanner_state(checkpoint->in);
    input_line = checkpoint->input;
    input_pos = checkpoint->position;
    string_buffer = checkpoint->strings;
    string_start_line = checkpoint->string_line;
    string_start_column = checkpoint->string_column;
    indent_count = checkpoint->indent;
    yylineno = checkpoint->line;
    g_stdin_eof = checkpoint->stdin_eof;
    last_token = checkpoint->last_token;
    prev_token = checkpoint->prev_token;
    brace_stack = checkpoint->braces;
    BEGIN(checkpoint->condition);
    delete checkpoint;
}
