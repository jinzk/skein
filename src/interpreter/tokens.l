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
int last_token_line = 0;
int prev_token_line = 0;
static bool declaration_header = false;
static bool extern_header = false;

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

/*
 * Recognize the unparenthesized `for NAME in exp { ... }` header so the
 * opening brace can be classified as a loop body instead of a dict/struct
 * literal.  Only the token sequence is tracked here; the parser still owns
 * all validation.
 */
enum for_header_state {
    FOR_HEADER_NONE,
    FOR_HEADER_SEEN,
    FOR_HEADER_ONE,
    FOR_HEADER_COMMA,
    FOR_HEADER_TWO
};

static enum for_header_state for_header = FOR_HEADER_NONE;
static bool for_body_pending = false;
static bool for_operand_started = false;
static int for_body_indent = 0;

static void reset_for_header_state() {
    for_header = FOR_HEADER_NONE;
    for_body_pending = false;
    for_operand_started = false;
}

static void note_for_header_token(int tok) {
    if (for_body_pending && tok != SKEIN_TOK_IN && tok != SKEIN_TOK_EOL)
        for_operand_started = true;
}

static void report_same_line_error(const char *message) {
    diag_set_location(g_token_line, g_token_column);
    yyerror("syntax error %s", message);
}

/*
 * No-parenthesis headers such as `if x { ... }` need the first opening
 * brace at the header's indentation to be recognized as the body.  A stack
 * is used so a match expression inside an if condition can still consume
 * its own brace before the outer if body.
 */
struct header_context {
    int kind;
    int indent;
};

static std::vector<header_context> header_stack;
static int pending_header_kind = 0;
static int pending_header_indent = 0;
static bool pending_header_skip = false;
static bool c_for_pending = false;
static int c_for_indent = 0;
static bool pending_for_skip = false;
static bool pending_for_paren = false;
static bool pending_do_while_skip = false;
static bool pending_do_while_paren = false;
static bool pending_match_skip = false;
static bool pending_match_paren = false;
static int pending_match_indent = 0;
static int pending_decl_kind = 0;
static bool pending_decl_skip = false;
static std::vector<int> do_stack;

static void start_header_context(int kind) {
    if (pending_header_kind != 0)
        header_stack.push_back(
            header_context{pending_header_kind, pending_header_indent});
    pending_header_kind = kind;
    pending_header_indent = indent_count;
    pending_header_skip = true;
}

static void note_header_context_token(int tok) {
    if (pending_header_skip) {
        pending_header_skip = false;
    } else if (pending_header_kind != 0) {
        if (tok != '(' && tok != SKEIN_TOK_EOL)
            header_stack.push_back(
                header_context{pending_header_kind, pending_header_indent});
        pending_header_kind = 0;
    }

    if (pending_for_skip) {
        pending_for_skip = false;
    } else if (pending_for_paren) {
        if (tok == '(')
            c_for_pending = false;
        pending_for_paren = false;
    }

    if (pending_do_while_skip) {
        pending_do_while_skip = false;
    } else {
        pending_do_while_paren = false;
    }

    if (pending_match_skip) {
        pending_match_skip = false;
    } else if (pending_match_paren) {
        if (tok != ':' && tok != ',' && tok != '}')
            header_stack.push_back(
                header_context{SKEIN_TOK_MATCH, pending_match_indent});
        pending_match_paren = false;
    }

    if (pending_decl_skip) {
        pending_decl_skip = false;
    } else if (pending_decl_kind != 0) {
        if (tok != ':' && tok != ',' && tok != '}' && tok != ';' &&
            tok != SKEIN_TOK_EOL) {
            if (pending_decl_kind == SKEIN_TOK_EXTERN)
                extern_header = true;
            else
                declaration_header = true;
        }
        pending_decl_kind = 0;
    }

    if (tok == SKEIN_TOK_EOL) {
        header_stack.clear();
        c_for_pending = false;
    } else if (tok == ';') {
        while (!header_stack.empty() &&
               header_stack.back().indent >= indent_count)
            header_stack.pop_back();
    } else if (tok == '}') {
        while (!header_stack.empty() &&
               header_stack.back().indent > indent_count)
            header_stack.pop_back();
        while (!do_stack.empty() && do_stack.back() > indent_count)
            do_stack.pop_back();
    }
}

static void note_for_name_token() {
    if (for_header == FOR_HEADER_SEEN)
        for_header = FOR_HEADER_ONE;
    else if (for_header == FOR_HEADER_COMMA)
        for_header = FOR_HEADER_TWO;
    else if (for_header != FOR_HEADER_NONE)
        for_header = FOR_HEADER_NONE;
}

#define YYRET(tok) do { int yyret_tok = (tok); note_for_header_token(yyret_tok); note_header_context_token(yyret_tok); note_token(yyret_tok); if (yyret_tok != SKEIN_TOK_EOL) { prev_token_line = last_token_line; last_token_line = g_token_line; } prev_token = last_token; last_token = yyret_tok; return yyret_tok; } while (0)

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
        return SKEIN_TOK_FLOAT;
    }
    yylval.i = value;
    return SKEIN_TOK_INT;
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
        return SKEIN_TOK_INT;
    }
    yylval.i = (long long)value;
    return SKEIN_TOK_INT;
}

static int lex_float(const char *text, int len) {
    std::string clean = strip_numeric_underscores(text, len);
    yylval.d = std::strtod(clean.c_str(), NULL);
    return SKEIN_TOK_FLOAT;
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
    return SKEIN_TOK_NAME;
}
/*
 * Decide whether an opening brace introduces a block body.  Only syntactic
 * positions that always require a body are treated as blocks; everything else
 * is treated as a dict when the brace is empty.  For non-empty braces this
 * choice is irrelevant because the closing brace stays a normal '}'.
 */
static bool brace_starts_block() {
    if (last_token == SKEIN_TOK_NAME && prev_token == SKEIN_TOK_PACKAGE)
        return true;
    if (declaration_header || extern_header)
        return true;
    if (last_token == ')' || last_token == SKEIN_TOK_ELSE || last_token == SKEIN_TOK_DO ||
        last_token == SKEIN_TOK_ARROW)
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
    if (tok == '{' || tok == '}' || tok == SKEIN_TOK_BODY_LBRACE ||
        tok == SKEIN_TOK_EMPTY_DICT_CLOSE || tok == SKEIN_TOK_EMPTY_BLOCK_CLOSE ||
        tok == SKEIN_TOK_EOL)
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

"++" { YYRET(SKEIN_TOK_INC); }
"--" { YYRET(SKEIN_TOK_DEC); }

"+=" { YYRET(SKEIN_TOK_PLUS_ASSIGN); }
"-=" { YYRET(SKEIN_TOK_MINUS_ASSIGN); }
"*=" { YYRET(SKEIN_TOK_MUL_ASSIGN); }
"/=" { YYRET(SKEIN_TOK_DIV_ASSIGN); }
"%=" { YYRET(SKEIN_TOK_MOD_ASSIGN); }
"&=" { YYRET(SKEIN_TOK_AND_ASSIGN); }
"|=" { YYRET(SKEIN_TOK_OR_ASSIGN); }
"|>>" { YYRET(SKEIN_TOK_PIPE_APPLY); }
"|>" { YYRET(SKEIN_TOK_PIPE); }
"^=" { YYRET(SKEIN_TOK_XOR_ASSIGN); }
"<<=" { YYRET(SKEIN_TOK_SHL_ASSIGN); }
">>=" { YYRET(SKEIN_TOK_SHR_ASSIGN); }

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
"?" {
    if (yytext[0] == ';') {
        declaration_header = false;
        extern_header = false;
        if (!for_body_pending || indent_count <= for_body_indent)
            reset_for_header_state();
    } else if (yytext[0] == ',') {
        if (for_header == FOR_HEADER_ONE)
            for_header = FOR_HEADER_COMMA;
        else if (for_header != FOR_HEADER_NONE)
            for_header = FOR_HEADER_NONE;
    }
    YYRET(yytext[0]);
}

"$" { YYRET(SKEIN_TOK_PLACEHOLDER); }

"..." { YYRET(SKEIN_TOK_ELLIPSIS); }

".." { YYRET(SKEIN_TOK_DOTDOT); }

"=>" { YYRET(SKEIN_TOK_ARROW); }
"->" { YYRET(SKEIN_TOK_RARROW); }

"::" { YYRET(SKEIN_TOK_COLONCOLON); }

"&&" { YYRET(SKEIN_TOK_LOGAND); }
"||" { YYRET(SKEIN_TOK_LOGOR); }
"<<" { YYRET(SKEIN_TOK_SHL); }
">>" { YYRET(SKEIN_TOK_SHR); }

"{" {
    int prev_line = last_token_line;
    if (!brace_stack.empty())
        brace_stack.back().empty = false;
    bool header_body = false;
    if (!header_stack.empty() &&
        header_stack.back().indent == indent_count) {
        header_body = true;
        header_stack.pop_back();
    }
    bool c_for_body = !header_body && c_for_pending &&
                      indent_count == c_for_indent;
    if (c_for_body)
        c_for_pending = false;
    bool for_body = !header_body && !c_for_body &&
                    for_body_pending && for_operand_started &&
                    indent_count == for_body_indent;
    bool body_token = header_body || c_for_body || for_body;
    bool body_brace = brace_starts_block() || body_token;
    if (body_brace && prev_line != 0 && g_token_line != prev_line)
        report_same_line_error(
            "opening brace must be on the same line as its header");
    for_header = FOR_HEADER_NONE;
    declaration_header = false;
    extern_header = false;
    if (body_token) {
        if (for_body)
            for_body_pending = false;
        brace_stack.push_back(brace_frame{false, true});
        indent_count++;
        YYRET(SKEIN_TOK_BODY_LBRACE);
    }
    if (for_body_pending && indent_count < for_body_indent)
        for_body_pending = false;
    brace_stack.push_back(brace_frame{!body_brace, true});
    indent_count++;
    YYRET('{');
}

"}" {
    if (indent_count > 0) indent_count--;
    for_header = FOR_HEADER_NONE;
    declaration_header = false;
    extern_header = false;
    if (for_body_pending && indent_count < for_body_indent)
        for_body_pending = false;
    int tok = '}';
    if (!brace_stack.empty()) {
        brace_frame frame = brace_stack.back();
        brace_stack.pop_back();
        if (frame.empty)
            tok = frame.is_dict ? SKEIN_TOK_EMPTY_DICT_CLOSE : SKEIN_TOK_EMPTY_BLOCK_CLOSE;
    }
    YYRET(tok);
}
"[" { indent_count++; YYRET('['); }
"]" { if (indent_count > 0) indent_count--; YYRET(']'); }
"(" {
    if (pending_match_paren) {
        header_stack.push_back(
            header_context{SKEIN_TOK_MATCH, pending_match_indent});
        pending_match_paren = false;
        pending_match_skip = false;
        indent_count++;
        YYRET(SKEIN_TOK_HEADER_LPAREN);
    }
    if (pending_do_while_paren) {
        pending_do_while_paren = false;
        pending_do_while_skip = false;
        indent_count++;
        YYRET(SKEIN_TOK_DO_WHILE_LPAREN);
    }
    if (pending_header_kind != 0) {
        pending_header_kind = 0;
        pending_header_skip = false;
        indent_count++;
        YYRET(SKEIN_TOK_HEADER_LPAREN);
    }
    if (pending_for_paren) {
        pending_for_paren = false;
        pending_for_skip = false;
        c_for_pending = false;
        for_header = FOR_HEADER_NONE;
        indent_count++;
        YYRET(SKEIN_TOK_HEADER_LPAREN);
    }
    if (for_header != FOR_HEADER_NONE)
        for_header = FOR_HEADER_NONE;
    indent_count++;
    YYRET('(');
}
")" { if (indent_count > 0) indent_count--; YYRET(')'); }

">" { yylval.fn = 1; YYRET(SKEIN_TOK_CMP); }
"<" { yylval.fn = 2; YYRET(SKEIN_TOK_CMP); }
"<>" |
"!=" { yylval.fn = 3; YYRET(SKEIN_TOK_CMP); }
"==" { yylval.fn = 4; YYRET(SKEIN_TOK_CMP); }
">=" { yylval.fn = 5; YYRET(SKEIN_TOK_CMP); }
"<=" { yylval.fn = 6; YYRET(SKEIN_TOK_CMP); }

"if" {
    start_header_context(SKEIN_TOK_IF);
    YYRET(SKEIN_TOK_IF);
}
"else" {
    if (last_token_line != 0 && g_token_line != last_token_line)
        report_same_line_error(
            "else must be on the same line as the preceding token");
    YYRET(SKEIN_TOK_ELSE);
}
"while" {
    if (!do_stack.empty() && do_stack.back() == indent_count) {
        do_stack.pop_back();
        pending_do_while_paren = true;
        pending_do_while_skip = true;
    } else {
        start_header_context(SKEIN_TOK_WHILE);
    }
    YYRET(SKEIN_TOK_WHILE);
}
"do" {
    do_stack.push_back(indent_count);
    YYRET(SKEIN_TOK_DO);
}
"for" {
    if (!declaration_header) {
        for_header = FOR_HEADER_SEEN;
        c_for_pending = true;
        c_for_indent = indent_count;
        pending_for_paren = true;
        pending_for_skip = true;
    }
    YYRET(SKEIN_TOK_FOR);
}
"struct" {
    if (last_token != '.') {
        pending_decl_kind = SKEIN_TOK_STRUCT;
        pending_decl_skip = true;
    }
    YYRET(SKEIN_TOK_STRUCT);
}
"extern" {
    if (last_token != '.') {
        pending_decl_kind = SKEIN_TOK_EXTERN;
        pending_decl_skip = true;
    }
    YYRET(SKEIN_TOK_EXTERN);
}
"enum" {
    if (last_token != '.') {
        pending_decl_kind = SKEIN_TOK_ENUM;
        pending_decl_skip = true;
    }
    YYRET(SKEIN_TOK_ENUM);
}
"impl" {
    if (last_token != '.') {
        pending_decl_kind = SKEIN_TOK_IMPL;
        pending_decl_skip = true;
    }
    YYRET(SKEIN_TOK_IMPL);
}
"trait" {
    if (last_token != '.') {
        pending_decl_kind = SKEIN_TOK_TRAIT;
        pending_decl_skip = true;
    }
    YYRET(SKEIN_TOK_TRAIT);
}
"switch" {
    start_header_context(SKEIN_TOK_SWITCH);
    YYRET(SKEIN_TOK_SWITCH);
}
"match" {
    if (last_token != '.') {
        if (pending_header_kind != 0) {
            header_stack.push_back(
                header_context{pending_header_kind, pending_header_indent});
            pending_header_kind = 0;
        }
        pending_match_indent = indent_count;
        pending_match_paren = true;
        pending_match_skip = true;
    }
    YYRET(SKEIN_TOK_MATCH);
}
"case" { YYRET(SKEIN_TOK_CASE); }
"default" { YYRET(SKEIN_TOK_DEFAULT); }
"let" { YYRET(SKEIN_TOK_LET); }
"const" { YYRET(SKEIN_TOK_CONST); }
"func" { YYRET(SKEIN_TOK_FUNC); }
"thread"[ \t]+/"func" { YYRET(SKEIN_TOK_THREAD); }
"process"[ \t]+/"func" { YYRET(SKEIN_TOK_PROCESS); }
"yield" { YYRET(SKEIN_TOK_YIELD); }
"launch" { YYRET(SKEIN_TOK_LAUNCH); }
"defer" { YYRET(SKEIN_TOK_DEFER); }
"package" { YYRET(SKEIN_TOK_PACKAGE); }
"import" { YYRET(SKEIN_TOK_IMPORT); }
"export" { YYRET(SKEIN_TOK_EXPORT); }
"return" { YYRET(SKEIN_TOK_RETURN); }
"break" { YYRET(SKEIN_TOK_BREAK); }
"continue" { YYRET(SKEIN_TOK_CONTINUE); }
"fail" { YYRET(SKEIN_TOK_FAIL); }
"is" { YYRET(SKEIN_TOK_IS); }
"label" { YYRET(SKEIN_TOK_LABEL); }
"on" { YYRET(SKEIN_TOK_ON); }
"error" { YYRET(SKEIN_TOK_ERROR); }
"recover" { YYRET(SKEIN_TOK_RECOVER); }
"pass" { YYRET(SKEIN_TOK_PASS); }
"in" {
    if (for_header == FOR_HEADER_ONE || for_header == FOR_HEADER_TWO) {
        for_body_pending = true;
        for_operand_started = false;
        for_body_indent = indent_count;
        c_for_pending = false;
    }
    for_header = FOR_HEADER_NONE;
    YYRET(SKEIN_TOK_IN);
}
"as" { YYRET(SKEIN_TOK_AS); }
"null" { YYRET(SKEIN_TOK_NULLVAL); }
"true" { YYRET(SKEIN_TOK_TRUE); }
"false" { YYRET(SKEIN_TOK_FALSE); }

"sqrt" { yylval.fn = B_sqrt; YYRET(SKEIN_TOK_BUILTIN); }
"exp" { yylval.fn = B_exp; YYRET(SKEIN_TOK_BUILTIN); }
"log" { yylval.fn = B_log; YYRET(SKEIN_TOK_BUILTIN); }
"abs" { yylval.fn = B_abs; YYRET(SKEIN_TOK_BUILTIN); }
"sin" { yylval.fn = B_sin; YYRET(SKEIN_TOK_BUILTIN); }
"cos" { yylval.fn = B_cos; YYRET(SKEIN_TOK_BUILTIN); }
"tan" { yylval.fn = B_tan; YYRET(SKEIN_TOK_BUILTIN); }
"asin" { yylval.fn = B_asin; YYRET(SKEIN_TOK_BUILTIN); }
"acos" { yylval.fn = B_acos; YYRET(SKEIN_TOK_BUILTIN); }
"atan" { yylval.fn = B_atan; YYRET(SKEIN_TOK_BUILTIN); }
"atan2" { yylval.fn = B_atan2; YYRET(SKEIN_TOK_BUILTIN); }
"pow" { yylval.fn = B_pow; YYRET(SKEIN_TOK_BUILTIN); }
"floor" { yylval.fn = B_floor; YYRET(SKEIN_TOK_BUILTIN); }
"ceil" { yylval.fn = B_ceil; YYRET(SKEIN_TOK_BUILTIN); }
"round" { yylval.fn = B_round; YYRET(SKEIN_TOK_BUILTIN); }
"trunc" { yylval.fn = B_trunc; YYRET(SKEIN_TOK_BUILTIN); }
"log2" { yylval.fn = B_log2; YYRET(SKEIN_TOK_BUILTIN); }
"log10" { yylval.fn = B_log10; YYRET(SKEIN_TOK_BUILTIN); }
"hypot" { yylval.fn = B_hypot; YYRET(SKEIN_TOK_BUILTIN); }
"min" { yylval.fn = B_min; YYRET(SKEIN_TOK_BUILTIN); }
"max" { yylval.fn = B_max; YYRET(SKEIN_TOK_BUILTIN); }
"len" { yylval.fn = B_len; YYRET(SKEIN_TOK_BUILTIN); }
"range" { yylval.fn = B_range; YYRET(SKEIN_TOK_BUILTIN); }
"print" { yylval.fn = B_print; YYRET(SKEIN_TOK_BUILTIN); }
"println" { yylval.fn = B_println; YYRET(SKEIN_TOK_BUILTIN); }
"list"/"." {
    int token = make_name_token("list", 4);
    if (token)
        YYRET(token);
    yylval.fn = B_list;
    YYRET(SKEIN_TOK_BUILTIN);
}
"list" { yylval.fn = B_list; YYRET(SKEIN_TOK_BUILTIN); }
"dict"/"." {
    int token = make_name_token("dict", 4);
    if (token)
        YYRET(token);
    yylval.fn = B_dict;
    YYRET(SKEIN_TOK_BUILTIN);
}
"dict" { yylval.fn = B_dict; YYRET(SKEIN_TOK_BUILTIN); }
"int" { yylval.fn = B_int; YYRET(SKEIN_TOK_BUILTIN); }
"float" { yylval.fn = B_float; YYRET(SKEIN_TOK_BUILTIN); }
"bool" { yylval.fn = B_bool; YYRET(SKEIN_TOK_BUILTIN); }
"str" { yylval.fn = B_str; YYRET(SKEIN_TOK_BUILTIN); }
"char" { yylval.fn = B_char; YYRET(SKEIN_TOK_BUILTIN); }
"byte" { yylval.fn = B_byte; YYRET(SKEIN_TOK_BUILTIN); }
"kind_of" { yylval.fn = B_kind_of; YYRET(SKEIN_TOK_BUILTIN); }
"type_name" { yylval.fn = B_type_name; YYRET(SKEIN_TOK_BUILTIN); }
"type_module" { yylval.fn = B_type_module; YYRET(SKEIN_TOK_BUILTIN); }
"type_source" { yylval.fn = B_type_source; YYRET(SKEIN_TOK_BUILTIN); }
"format" { yylval.fn = B_format; YYRET(SKEIN_TOK_BUILTIN); }
"read_file" { yylval.fn = B_read_file; YYRET(SKEIN_TOK_BUILTIN); }
"write_file" { yylval.fn = B_write_file; YYRET(SKEIN_TOK_BUILTIN); }
"append_file" { yylval.fn = B_append_file; YYRET(SKEIN_TOK_BUILTIN); }
"file_exists" { yylval.fn = B_file_exists; YYRET(SKEIN_TOK_BUILTIN); }
"file_open" { yylval.fn = B_file_open; YYRET(SKEIN_TOK_BUILTIN); }
"file_read" { yylval.fn = B_file_read; YYRET(SKEIN_TOK_BUILTIN); }
"file_write" { yylval.fn = B_file_write; YYRET(SKEIN_TOK_BUILTIN); }
"file_seek" { yylval.fn = B_file_seek; YYRET(SKEIN_TOK_BUILTIN); }
"file_tell" { yylval.fn = B_file_tell; YYRET(SKEIN_TOK_BUILTIN); }
"file_eof" { yylval.fn = B_file_eof; YYRET(SKEIN_TOK_BUILTIN); }
"file_close" { yylval.fn = B_file_close; YYRET(SKEIN_TOK_BUILTIN); }
"file_flush" { yylval.fn = B_file_flush; YYRET(SKEIN_TOK_BUILTIN); }
"input" { yylval.fn = B_input; YYRET(SKEIN_TOK_BUILTIN); }
"split" { yylval.fn = B_split; YYRET(SKEIN_TOK_BUILTIN); }
"split_ws" { yylval.fn = B_split_ws; YYRET(SKEIN_TOK_BUILTIN); }
"trim" { yylval.fn = B_trim; YYRET(SKEIN_TOK_BUILTIN); }
"find" { yylval.fn = B_find; YYRET(SKEIN_TOK_BUILTIN); }
"substr" { yylval.fn = B_substr; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_from_string" { yylval.fn = B_bytes_from_string; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_to_string" { yylval.fn = B_bytes_to_string; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_from_char" { yylval.fn = B_bytes_from_char; YYRET(SKEIN_TOK_BUILTIN); }
"char_from_bytes" { yylval.fn = B_char_from_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_len" { yylval.fn = B_bytes_len; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_get" { yylval.fn = B_bytes_get; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_slice" { yylval.fn = B_bytes_slice; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_concat" { yylval.fn = B_bytes_concat; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_hex" { yylval.fn = B_bytes_hex; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_from_hex" { yylval.fn = B_bytes_from_hex; YYRET(SKEIN_TOK_BUILTIN); }
"bytes_to_int" { yylval.fn = B_bytes_to_int; YYRET(SKEIN_TOK_BUILTIN); }
"int_to_bytes" { yylval.fn = B_int_to_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"read_bytes" { yylval.fn = B_read_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"write_bytes" { yylval.fn = B_write_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"append_bytes" { yylval.fn = B_append_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"file_read_bytes" { yylval.fn = B_file_read_bytes; YYRET(SKEIN_TOK_BUILTIN); }
"file_write_bytes" { yylval.fn = B_file_write_bytes; YYRET(SKEIN_TOK_BUILTIN); }

{ASCII_NAME} {
    int tok = make_name_token(yytext, yyleng);
    if (tok == 0)
        return 0;
    note_for_name_token();
    YYRET(tok);
}

{UNICODE_NAME} {
    unicode_ident::scan_result scanned =
        unicode_ident::scan_identifier(yytext, (size_t)yyleng);

    if (scanned.valid) {
        int tok = make_name_token(yytext, yyleng);
        if (tok == 0)
            return 0;
        note_for_name_token();
        YYRET(tok);
    }

    if (scanned.valid_bytes > 0) {
        int keep = (int)scanned.valid_bytes;
        diag_scan_rewind(yytext + keep, yyleng - keep);
        yyless(keep);
        int tok = make_name_token(yytext, keep);
        if (tok == 0)
            return 0;
        note_for_name_token();
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
    YYRET(SKEIN_TOK_INT);
}
0[bB][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid binary integer literal");
    yylval.i = 0;
    YYRET(SKEIN_TOK_INT);
}
0[oO][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid octal integer literal");
    yylval.i = 0;
    YYRET(SKEIN_TOK_INT);
}

{DIGITS}".." { yyless((int)(yyleng - 2)); YYRET(lex_decimal_integer(yytext, yyleng)); }
{DIGITS} { YYRET(lex_decimal_integer(yytext, yyleng)); }

({DIGITS}"."({DIGITS})?|"."{DIGITS})({EXP})? { YYRET(lex_float(yytext, yyleng)); }
{DIGITS}{EXP} { YYRET(lex_float(yytext, yyleng)); }

[0-9][0-9A-Za-z_]* {
    diag_report_lexical(g_token_line, g_token_column,
                        "invalid numeric literal");
    yylval.i = 0;
    YYRET(SKEIN_TOK_INT);
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
        YYRET(SKEIN_TOK_EOL);
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
        YYRET(SKEIN_TOK_XSTRING);
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
        YYRET(SKEIN_TOK_EOL);
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
        YYRET(SKEIN_TOK_CHAR);
    }
    . {
        char_buffer += yytext;
    }
}

\n {
    if (indent_count == 0) {
        reset_for_header_state();
        YYRET(SKEIN_TOK_EOL);
    }
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
    last_token_line = 0;
    prev_token_line = 0;
    declaration_header = false;
    extern_header = false;
    header_stack.clear();
    pending_header_kind = 0;
    pending_header_indent = 0;
    pending_header_skip = false;
    c_for_pending = false;
    c_for_indent = 0;
    pending_for_skip = false;
    pending_for_paren = false;
    pending_do_while_skip = false;
    pending_do_while_paren = false;
    pending_match_skip = false;
    pending_match_paren = false;
    pending_match_indent = 0;
    pending_decl_kind = 0;
    pending_decl_skip = false;
    do_stack.clear();
    brace_stack.clear();
    reset_for_header_state();
    for_body_indent = 0;
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
    int last_token_line;
    int prev_token_line;
    bool declaration_header;
    bool extern_header;
    int for_header;
    bool for_body_pending;
    bool for_operand_started;
    int for_body_indent;
    std::vector<header_context> headers;
    int pending_header_kind;
    int pending_header_indent;
    bool pending_header_skip;
    bool c_for_pending;
    int c_for_indent;
    bool pending_for_skip;
    bool pending_for_paren;
    bool pending_do_while_skip;
    bool pending_do_while_paren;
    bool pending_match_skip;
    bool pending_match_paren;
    int pending_match_indent;
    int pending_decl_kind;
    bool pending_decl_skip;
    std::vector<int> do_indents;
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
    checkpoint->last_token_line = last_token_line;
    checkpoint->prev_token_line = prev_token_line;
    checkpoint->declaration_header = declaration_header;
    checkpoint->extern_header = extern_header;
    checkpoint->for_header = for_header;
    checkpoint->for_body_pending = for_body_pending;
    checkpoint->for_operand_started = for_operand_started;
    checkpoint->for_body_indent = for_body_indent;
    checkpoint->headers = header_stack;
    checkpoint->pending_header_kind = pending_header_kind;
    checkpoint->pending_header_indent = pending_header_indent;
    checkpoint->pending_header_skip = pending_header_skip;
    checkpoint->c_for_pending = c_for_pending;
    checkpoint->c_for_indent = c_for_indent;
    checkpoint->pending_for_skip = pending_for_skip;
    checkpoint->pending_for_paren = pending_for_paren;
    checkpoint->pending_do_while_skip = pending_do_while_skip;
    checkpoint->pending_do_while_paren = pending_do_while_paren;
    checkpoint->pending_match_skip = pending_match_skip;
    checkpoint->pending_match_paren = pending_match_paren;
    checkpoint->pending_match_indent = pending_match_indent;
    checkpoint->pending_decl_kind = pending_decl_kind;
    checkpoint->pending_decl_skip = pending_decl_skip;
    checkpoint->do_indents = do_stack;
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
    last_token_line = checkpoint->last_token_line;
    prev_token_line = checkpoint->prev_token_line;
    declaration_header = checkpoint->declaration_header;
    extern_header = checkpoint->extern_header;
    for_header = (enum for_header_state)checkpoint->for_header;
    for_body_pending = checkpoint->for_body_pending;
    for_operand_started = checkpoint->for_operand_started;
    for_body_indent = checkpoint->for_body_indent;
    header_stack = checkpoint->headers;
    pending_header_kind = checkpoint->pending_header_kind;
    pending_header_indent = checkpoint->pending_header_indent;
    pending_header_skip = checkpoint->pending_header_skip;
    c_for_pending = checkpoint->c_for_pending;
    c_for_indent = checkpoint->c_for_indent;
    pending_for_skip = checkpoint->pending_for_skip;
    pending_for_paren = checkpoint->pending_for_paren;
    pending_do_while_skip = checkpoint->pending_do_while_skip;
    pending_do_while_paren = checkpoint->pending_do_while_paren;
    pending_match_skip = checkpoint->pending_match_skip;
    pending_match_paren = checkpoint->pending_match_paren;
    pending_match_indent = checkpoint->pending_match_indent;
    pending_decl_kind = checkpoint->pending_decl_kind;
    pending_decl_skip = checkpoint->pending_decl_skip;
    do_stack = checkpoint->do_indents;
    brace_stack = checkpoint->braces;
    BEGIN(checkpoint->condition);
    delete checkpoint;
}
