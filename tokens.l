%option noyywrap nodefault yylineno 
%x STRING

%{
#include <string.h>
#include "ast.h"
#include "parser.hpp"
extern int indent_count;

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
";" |
"(" |
")" |
"[" |
"]" {return yytext[0];}

"{" {indent_count++;return yytext[0];}
"}" {indent_count--;return yytext[0];}

">" {yylval.fn = 1; return CMP;}
"<" {yylval.fn = 2; return CMP;}
"<>" |
"!=" {yylval.fn = 3; return CMP;}
"==" {yylval.fn = 4; return CMP;}
">=" {yylval.fn = 5; return CMP;}
"<=" {yylval.fn = 6; return CMP;}

"if" {return IF;}
"then" {return THEN;}
"else" {return ELSE;}
"while" {return WHILE;}
"do" {return DO;}
"for" {return FOR;}
"let" {return LET;}
"def" {return DEF;}
"end" {return END;}
"return" {return RETURN;}
"break" {return BREAK;}
"continue" {return CONTINUE;}

"sqrt" {yylval.fn = B_sqrt; return FUNC;}
"exp" {yylval.fn = B_exp; return FUNC;}
"log" {yylval.fn = B_log; return FUNC;}
"abs" {yylval.fn = B_abs; return FUNC;}
"print" {yylval.fn = B_print; return FUNC;}

[a-zA-Z][a-zA-Z0-9]* {
    int len = strlen(yytext) + 1;
    yylval.str = (char *)malloc(len);
    strcpy(yylval.str, yytext); 
    return NAME;
}

[0-9]+ {yylval.i = atoi(yytext); return INT;}

[0-9]+{EXP} |
[0-9]+"."[0-9]*{EXP}? |
"."?[0-9]*{EXP}? {yylval.d = atof(yytext); return FLOAT;}

"//".*
[ \t] {}

\" BEGIN(STRING);
<STRING>{
    \n yyerror("the string misses \" to termiate before newline");
    <<EOF>> yyerror("the string misses \" to terminate before EOF");
    ([^\\\"]|\\.)* {
        int len = strlen(yytext) + 1;
        yylval.str = (char *)malloc(len);
        strcpy(yylval.str, yytext); 
        return XSTRING;
    }
    \" BEGIN(INITIAL);
    . {}
}

\n {
    if (indent_count > 0) {
        printf("> ");
        for (int i = 0; i < indent_count; i++) {
			printf("..");
		}
    } else {
        return EOL;
    }
}

. {yyerror("Mystery character %c\n", *yytext);}

%%
