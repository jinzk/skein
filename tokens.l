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

extern int indent_count;

static std::string string_buffer;

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
"=" |
"," |
":" |
";" { return yytext[0]; }

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

"sqrt" { yylval.fn = B_sqrt; return FUNC; }
"exp" { yylval.fn = B_exp; return FUNC; }
"log" { yylval.fn = B_log; return FUNC; }
"abs" { yylval.fn = B_abs; return FUNC; }
"print" { yylval.fn = B_print; return FUNC; }

[a-zA-Z][a-zA-Z0-9]* {
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
    long v = std::strtol(yytext, NULL, 10);
    if (errno == ERANGE || v > INT_MAX || v < INT_MIN) {
        yylval.d = std::strtod(yytext, NULL);
        return FLOAT;
    }
    yylval.i = (int)v;
    return INT;
}

([0-9]+"."[0-9]*|"."[0-9]+)({EXP})? { yylval.d = std::strtod(yytext, NULL); return FLOAT; }
[0-9]+{EXP} { yylval.d = std::strtod(yytext, NULL); return FLOAT; }

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
