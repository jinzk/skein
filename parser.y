%{

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <map>
#include <vector>
#include <iostream>
#include "ast.h"
#include "scopestack.h"
#include "eval.h"

extern int yylex();
extern int yyparse();
%}

%union {
	struct ast *a;
	struct symlist *sl;
	double d;
	int i;
	int fn;
	char *str;
}

%token <d> FLOAT
%token <i> INT
%token <str> NAME
%token <fn> FUNC
%token <str> XSTRING
%token EOL INDENT
%token IF THEN ELSE WHILE DO FOR DEF END LET
%token RETURN BREAK CONTINUE

%right RETURN
%nonassoc <fn> CMP
%right '='
%left '+' '-'
%left '*' '/'
%nonassoc UMINUS
%nonassoc NOELSE
%nonassoc ELSE

%type <a> exp value stmt list scope explist calclist docalc line valuelist pairlist;
%type <sl> symlist

%start calc
%%

stmt: IF '(' exp ')' scope {$$=newflow('I', $3, $5, NULL);} %prec NOELSE
	| IF '(' exp ')' scope ELSE scope {$$=newflow('I', $3, $5, $7);}
	| WHILE '(' exp ')' scope {$$=newflow('W', $3, $5, NULL);}
	| DEF NAME '(' symlist ')' '{' list '}' {$$=newfunc($2, $4, $7);}
	| LET NAME '=' exp ';' {$$=newvar($2, $4);}
	;

line: RETURN exp {$$=newret($2);}
	| RETURN {$$=newret(NULL);}
	| BREAK {$$=newbrk();}
	| CONTINUE {$$=newctn();}
	| exp {$$ = $1;}
	;

scope: stmt {$$ = $1;}
	| line ';' {$$ = $1;}
	| '{' list '}' {$$ = newscope($2);}
	;

list: {$$=NULL;}
	| scope list {
		if ($2 == NULL)
			$$ = $1;
		else
			$$ = newast('L', $1, $2);
	}
	;

exp: exp '+' exp {$$ = newast('+', $1, $3);}
	| exp '-' exp {$$ = newast('-', $1, $3);}
	| exp '*' exp {$$ = newast('*', $1, $3);}
	| exp '/' exp {$$ = newast('/', $1, $3);}
	| '(' exp ')' {$$ = $2;}
	| '-' exp %prec UMINUS {$$ = newast('M', $2, NULL);}
	| exp CMP exp {$$ = newcmp($2, $1, $3);}
	| NAME { $$ = newref($1);}
	| NAME '=' exp { $$ = newasgn($1, $3);}
	| FUNC '(' explist ')' {$$ = newfunc($1, $3);}
	| NAME '(' explist ')' {$$ = newcall($1, $3);}
	| '[' valuelist ']' {$$ = $2;}
	| '{' pairlist '}' {$$ = $2;}
	| value
	;

value: INT {$$ = newint($1);}
	| FLOAT {$$ = newfloat($1);}
	| XSTRING {$$ = newstr($1);}
	;

valuelist: value {$$=newlist($1, NULL);}
	| value ',' valuelist  {$$ = $$=newlist($1, $3);}
	;

pairlist: value ':' value {$$=newdict($1, $3, NULL);}
	| value ':' value ',' pairlist  {$$=newdict($1, $3, $5);}
	;

explist: exp
	| exp ',' explist {$$ = newast('L', $1, $3);}
	;

symlist: NAME {$$ = newsymlist(newsym($1), NULL);}
	| NAME ',' symlist {$$ = newsymlist(newsym($1), $3);}
	;

calclist: exp {$$ = $1;}
	| list {$$ = $1;}
	;

docalc: calclist EOL {
		struct value_t *rslt = eval($1);
		if (rslt) {
			switch (rslt->type) {
				case T_int: printf("= %4d\n> ", ((struct int_value_t *)rslt)->value); break;
				case T_float: printf("= %4.4g\n> ", ((struct float_value_t *)rslt)->value); break;
				case T_string: printf("= %s\n> ", ((struct string_value_t *)rslt)->value); break;
				case T_list: {printf("list\n> ");}; break;
				case T_dict: {printf("dict\n> ");}; break;
				default:  printf("not define value type %c\n> ", rslt->type); break;
			}
		} else {
			printf("> ");
		}
		if ($1) treefree($1);
	}
	| error {yyclearin; yyerrok; printf("> ");}
	;

calc: 
	| calc docalc
	;
%%

void yyerror(const char *s, ...) {
	va_list ap;
	va_start(ap, s);

	fprintf(stderr, "%d: error: ", yylineno);
	vfprintf(stderr, s, ap);
	fprintf(stderr, "\n");
}
