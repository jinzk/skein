%option noyywrap nodefault yylineno noinput nounput nounistd
%x STRING

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

extern int indent_count;

static std::string string_buffer;

/* Location of the opening quote of the string currently being scanned. */
static int string_start_line = 1;
static int string_start_column = 1;

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
/*
 * Decide whether an opening brace introduces a block body.  Only syntactic
 * positions that always require a body are treated as blocks; everything else
 * is treated as a dict when the brace is empty.  For non-empty braces this
 * choice is irrelevant because the closing brace stays a normal '}'.
 */
static bool brace_starts_block() {
    if (last_token == NAME && prev_token == PACKAGE)
        return true;
    if (last_token == ')' || last_token == ELSE)
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

EXP ([Ee][-+]?[0-9]+)

%%

^"#!"[^\n]*\n? { }

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
"@" { YYRET(yytext[0]); }

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
"for" { YYRET(FOR); }
"let" { YYRET(LET); }
"const" { YYRET(CONST); }
"func" { YYRET(FUNC); }
"package" { YYRET(PACKAGE); }
"import" { YYRET(IMPORT); }
"export" { YYRET(EXPORT); }
"return" { YYRET(RETURN); }
"break" { YYRET(BREAK); }
"continue" { YYRET(CONTINUE); }
"in" { YYRET(IN); }
"as" { YYRET(AS); }
"null" { YYRET(NULLVAL); }

"sqrt" { yylval.fn = B_sqrt; YYRET(BUILTIN); }
"exp" { yylval.fn = B_exp; YYRET(BUILTIN); }
"log" { yylval.fn = B_log; YYRET(BUILTIN); }
"abs" { yylval.fn = B_abs; YYRET(BUILTIN); }
"len" { yylval.fn = B_len; YYRET(BUILTIN); }
"range" { yylval.fn = B_range; YYRET(BUILTIN); }
"print" { yylval.fn = B_print; YYRET(BUILTIN); }
"list" { yylval.fn = B_list; YYRET(BUILTIN); }
"dict" { yylval.fn = B_dict; YYRET(BUILTIN); }
"int" { yylval.fn = B_int; YYRET(BUILTIN); }
"float" { yylval.fn = B_float; YYRET(BUILTIN); }
"str" { yylval.fn = B_str; YYRET(BUILTIN); }
"format" { yylval.fn = B_format; YYRET(BUILTIN); }
"command" { yylval.fn = B_command; YYRET(BUILTIN); }
"command_status" { yylval.fn = B_command_status; YYRET(BUILTIN); }
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

[a-zA-Z_][a-zA-Z0-9_]* {
    int len = (int)std::strlen(yytext) + 1;
    yylval.str = (char *)std::malloc(len);
    if (!yylval.str) {
        yyerror("out of space");
        return 0;
    }
    std::memcpy(yylval.str, yytext, len);
    YYRET(NAME);
}

[0-9]+ {
    errno = 0;
    long long v = std::strtoll(yytext, NULL, 10);
    if (errno == ERANGE) {
        yylval.d = std::strtod(yytext, NULL);
        YYRET(FLOAT);
    }
    yylval.i = v;
    YYRET(INT);
}

([0-9]+"."[0-9]*|"."[0-9]+)({EXP})? { yylval.d = std::strtod(yytext, NULL); YYRET(FLOAT); }
[0-9]+{EXP} { yylval.d = std::strtod(yytext, NULL); YYRET(FLOAT); }

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

\n {
    if (indent_count > 0) {
        if (g_interactive) {
            std::printf("> ");
            for (int i = 0; i < indent_count; ++i)
                std::printf("..");
            std::fflush(stdout);
        }
    } else {
        YYRET(EOL);
    }
}

. {
    unsigned char c = (unsigned char)yytext[0];
    if (std::isprint(c))
        diag_report_lexical(g_token_line, g_token_column,
                            "unexpected character '%c' (0x%02x)", c, c);
    else
        diag_report_lexical(g_token_line, g_token_column,
                            "unexpected byte 0x%02x", c);
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
