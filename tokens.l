%option noyywrap nodefault yylineno noinput nounput
%x STRING

%{
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ast.h"
#include "parser.hpp"
#include "repl.h"

extern int indent_count;

static std::string string_buffer;

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

static input_status read_input_line() {
    input_line.clear();
    input_pos = 0;

    for (;;) {
        if (g_interrupted) {
            std::clearerr(stdin);
            return INPUT_INTERRUPTED;
        }

        errno = 0;
        int c = std::getchar();
        if (c == EOF) {
            if (g_interrupted) {
                std::clearerr(stdin);
                return INPUT_INTERRUPTED;
            }
            if (std::ferror(stdin) && errno == EINTR) {
                std::clearerr(stdin);
                g_interrupted = 1;
                return INPUT_INTERRUPTED;
            }
            return input_line.empty() ? INPUT_EOF : INPUT_PARTIAL;
        }
        if (c == '\n') {
            input_line.push_back('\n');
            return INPUT_LINE;
        }
        if (c == '\b' || c == 0x7f) {
            if (!input_line.empty())
                input_line.pop_back();
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
"def" { return DEF; }
"return" { return RETURN; }
"break" { return BREAK; }
"continue" { return CONTINUE; }
"null" { return NULLVAL; }

"sqrt" { yylval.fn = B_sqrt; return FUNC; }
"exp" { yylval.fn = B_exp; return FUNC; }
"log" { yylval.fn = B_log; return FUNC; }
"abs" { yylval.fn = B_abs; return FUNC; }
"print" { yylval.fn = B_print; return FUNC; }
"list" { yylval.fn = B_list; return FUNC; }
"dict" { yylval.fn = B_dict; return FUNC; }
"int" { yylval.fn = B_int; return FUNC; }
"float" { yylval.fn = B_float; return FUNC; }
"str" { yylval.fn = B_str; return FUNC; }
"format" { yylval.fn = B_format; return FUNC; }

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
    string_buffer.clear();
    BEGIN(STRING);
}

<STRING>{
    \n {
        yyerror("unterminated string literal");
        string_buffer.clear();
        BEGIN(INITIAL);
        return EOL;
    }
    <<EOF>> {
        yyerror("unterminated string literal at end of input");
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
        std::printf("> ");
        for (int i = 0; i < indent_count; ++i)
            std::printf("..");
        std::fflush(stdout);
    } else {
        return EOL;
    }
}

. {
    yyerror("mystery character '%c'", *yytext);
}

%%

void reset_scanner_state(std::FILE *in) {
    BEGIN(INITIAL);
    string_buffer.clear();
    input_line.clear();
    input_pos = 0;
    std::clearerr(in);
    yyrestart(in);
}
