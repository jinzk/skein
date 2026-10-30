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

#include "ast.h"
#include "diagnostics.h"
#include "parser.hpp"
#include "repl.h"

extern int indent_count;

static std::string string_buffer;

/* Location of the opening quote of the string currently being scanned. */
static int string_start_line = 1;
static int string_start_column = 1;

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
"!" { return yytext[0]; }

"&&" { return LOGAND; }
"||" { return LOGOR; }
"<<" { return SHL; }
">>" { return SHR; }

"{" { indent_count++; return '{'; }
"}" { if (indent_count > 0) indent_count--; return '}'; }
"[" { indent_count++; return '['; }
"]" { if (indent_count > 0) indent_count--; return ']'; }
"(" { indent_count++; return '('; }
")" { if (indent_count > 0) indent_count--; return ')'; }

">" { yylval.fn = 1; return CMP; }
"<" { yylval.fn = 2; return CMP; }
"<>" |
"!=" { yylval.fn = 3; return CMP; }
"==" { yylval.fn = 4; return CMP; }
">=" { yylval.fn = 5; return CMP; }
"<=" { yylval.fn = 6; return CMP; }

"if" { return IF; }
"else" { return ELSE; }
"while" { return WHILE; }
"for" { return FOR; }
"let" { return LET; }
"const" { return CONST; }
"func" { return FUNC; }
"package" { return PACKAGE; }
"import" { return IMPORT; }
"export" { return EXPORT; }
"return" { return RETURN; }
"break" { return BREAK; }
"continue" { return CONTINUE; }
"null" { return NULLVAL; }

"sqrt" { yylval.fn = B_sqrt; return BUILTIN; }
"exp" { yylval.fn = B_exp; return BUILTIN; }
"log" { yylval.fn = B_log; return BUILTIN; }
"abs" { yylval.fn = B_abs; return BUILTIN; }
"print" { yylval.fn = B_print; return BUILTIN; }
"list" { yylval.fn = B_list; return BUILTIN; }
"dict" { yylval.fn = B_dict; return BUILTIN; }
"int" { yylval.fn = B_int; return BUILTIN; }
"float" { yylval.fn = B_float; return BUILTIN; }
"str" { yylval.fn = B_str; return BUILTIN; }
"format" { yylval.fn = B_format; return BUILTIN; }
"command" { yylval.fn = B_command; return BUILTIN; }
"command_status" { yylval.fn = B_command_status; return BUILTIN; }
"read_file" { yylval.fn = B_read_file; return BUILTIN; }
"write_file" { yylval.fn = B_write_file; return BUILTIN; }
"append_file" { yylval.fn = B_append_file; return BUILTIN; }
"file_exists" { yylval.fn = B_file_exists; return BUILTIN; }
"file_open" { yylval.fn = B_file_open; return BUILTIN; }
"file_read" { yylval.fn = B_file_read; return BUILTIN; }
"file_write" { yylval.fn = B_file_write; return BUILTIN; }
"file_seek" { yylval.fn = B_file_seek; return BUILTIN; }
"file_tell" { yylval.fn = B_file_tell; return BUILTIN; }
"file_eof" { yylval.fn = B_file_eof; return BUILTIN; }
"file_close" { yylval.fn = B_file_close; return BUILTIN; }
"file_flush" { yylval.fn = B_file_flush; return BUILTIN; }
"input" { yylval.fn = B_input; return BUILTIN; }
"split" { yylval.fn = B_split; return BUILTIN; }
"split_ws" { yylval.fn = B_split_ws; return BUILTIN; }
"trim" { yylval.fn = B_trim; return BUILTIN; }
"find" { yylval.fn = B_find; return BUILTIN; }
"substr" { yylval.fn = B_substr; return BUILTIN; }
"bytes_from_string" { yylval.fn = B_bytes_from_string; return BUILTIN; }
"bytes_to_string" { yylval.fn = B_bytes_to_string; return BUILTIN; }
"bytes_len" { yylval.fn = B_bytes_len; return BUILTIN; }
"bytes_get" { yylval.fn = B_bytes_get; return BUILTIN; }
"bytes_slice" { yylval.fn = B_bytes_slice; return BUILTIN; }
"bytes_concat" { yylval.fn = B_bytes_concat; return BUILTIN; }
"bytes_hex" { yylval.fn = B_bytes_hex; return BUILTIN; }
"bytes_from_hex" { yylval.fn = B_bytes_from_hex; return BUILTIN; }
"bytes_to_int" { yylval.fn = B_bytes_to_int; return BUILTIN; }
"int_to_bytes" { yylval.fn = B_int_to_bytes; return BUILTIN; }
"read_bytes" { yylval.fn = B_read_bytes; return BUILTIN; }
"write_bytes" { yylval.fn = B_write_bytes; return BUILTIN; }
"append_bytes" { yylval.fn = B_append_bytes; return BUILTIN; }
"file_read_bytes" { yylval.fn = B_file_read_bytes; return BUILTIN; }
"file_write_bytes" { yylval.fn = B_file_write_bytes; return BUILTIN; }

[a-zA-Z_][a-zA-Z0-9_]* {
    int len = (int)std::strlen(yytext) + 1;
    yylval.str = (char *)std::malloc(len);
    if (!yylval.str) {
        yyerror("out of space");
        return 0;
    }
    std::memcpy(yylval.str, yytext, len);
    return NAME;
}

[0-9]+ {
    errno = 0;
    long long v = std::strtoll(yytext, NULL, 10);
    if (errno == ERANGE) {
        yylval.d = std::strtod(yytext, NULL);
        return FLOAT;
    }
    yylval.i = v;
    return INT;
}

([0-9]+"."[0-9]*|"."[0-9]+)({EXP})? { yylval.d = std::strtod(yytext, NULL); return FLOAT; }
[0-9]+{EXP} { yylval.d = std::strtod(yytext, NULL); return FLOAT; }

"." { return '.'; }

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
        return EOL;
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
        return XSTRING;
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
        return EOL;
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
    BEGIN(checkpoint->condition);
    delete checkpoint;
}
