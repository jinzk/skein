%{

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>

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
%token EOL
%token IF ELSE WHILE FOR DEF LET
%token RETURN BREAK CONTINUE

%nonassoc <fn> CMP
%precedence '='
%left '+' '-'
%left '*' '/'
%precedence UMINUS
%precedence '['
%precedence NOELSE
%precedence ELSE

%type <a> exp value stmt list scope explist calclist docalc line valuelist pairlist
%type <a> forinit forcond forend
%type <sl> symlist

%destructor { treefree($$); } <a>
%destructor { free($$); } <str>
%destructor { symlistfree($$); } <sl>

%start calc
%%

stmt: IF '(' exp ')' scope %prec NOELSE   { $$ = newflow('I', $3, $5, NULL); }
    | IF '(' exp ')' scope ELSE scope     { $$ = newflow('I', $3, $5, $7); }
    | WHILE '(' exp ')' scope             { $$ = newflow('W', $3, $5, NULL); }
    | FOR '(' forinit ';' forcond ';' forend ')' scope
                                           { $$ = newfor($3, $5, $7, $9); }
    | DEF NAME '(' symlist ')' '{' list '}' { $$ = newfunc($2, $4, $7); }
    | LET NAME '=' exp ';'                { $$ = newvar($2, $4); }
    ;

line: RETURN exp                          { $$ = newret($2); }
    | RETURN                              { $$ = newret(NULL); }
    | BREAK                               { $$ = newbrk(); }
    | CONTINUE                            { $$ = newctn(); }
    | exp                                 { $$ = $1; }
    ;

scope: stmt                               { $$ = $1; }
     | line ';'                           { $$ = $1; }
     | '{' list '}'                       { $$ = newscope($2); }
     ;

list: %empty                              { $$ = NULL; }
    | scope list {
        if ($2 == NULL)
            $$ = $1;
        else
            $$ = newast('L', $1, $2);
      }
    ;

exp: exp '+' exp                          { $$ = newast('+', $1, $3); }
    | exp '-' exp                         { $$ = newast('-', $1, $3); }
    | exp '*' exp                         { $$ = newast('*', $1, $3); }
    | exp '/' exp                         { $$ = newast('/', $1, $3); }
    | '(' exp ')'                         { $$ = $2; }
    | '-' exp %prec UMINUS                { $$ = newast('M', $2, NULL); }
    | exp CMP exp                         { $$ = newcmp($2, $1, $3); }
    | NAME                                { $$ = newref($1); }
    | NAME '=' exp                        { $$ = newasgn($1, $3); }
    | FUNC '(' explist ')'                { $$ = newfunc($1, $3); }
    | NAME '(' explist ')'                { $$ = newcall($1, $3); }
    | exp '[' exp ']'                     { $$ = newast('G', $1, $3); }
    | '[' valuelist ']'                   { $$ = $2; }
    | '{' pairlist '}'                    { $$ = $2; }
    | value
    ;

value: INT                                { $$ = newint($1); }
    | FLOAT                               { $$ = newfloat($1); }
    | XSTRING                             { $$ = newstr($1); free($1); }
    ;

valuelist: %empty                         { $$ = newlist(NULL, NULL); }
    | exp                                 { $$ = newlist($1, NULL); }
    | exp ',' valuelist                   { $$ = newlist($1, $3); }
    ;

pairlist: exp ':' exp                     { $$ = newdict($1, $3, NULL); }
    | exp ':' exp ',' pairlist            { $$ = newdict($1, $3, $5); }
    ;

forinit: %empty                           { $$ = NULL; }
    | exp                                 { $$ = $1; }
    | LET NAME '=' exp                    { $$ = newvar($2, $4); }
    ;

forcond: %empty                           { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

forend: %empty                            { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

explist: %empty                            { $$ = NULL; }
    | exp
    | exp ',' explist                     { $$ = newast('L', $1, $3); }
    ;

symlist: %empty                           { $$ = NULL; }
    | NAME {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, NULL);
      }
    | NAME ',' symlist {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, $3);
      }
    ;

calclist: exp { $$ = $1; }
    | list { $$ = $1; }
    ;

docalc: calclist EOL {
        struct value_t *rslt = eval($1);
        if (rslt) {
            switch (rslt->type) {
                case T_return:
                    yyerror("return outside function");
                    break;
                case T_break:
                    yyerror("break outside loop");
                    break;
                case T_continue:
                    yyerror("continue outside loop");
                    break;
                default:
                    printf("= ");
                    printvalue(rslt);
                    printf("\n");
                    break;
            }
            freeval(rslt);
        }
        printf("> ");
        fflush(stdout);
        if ($1) treefree($1);
        $$ = NULL;
    }
    | error EOL {
        yyerrok;
        printf("> ");
        fflush(stdout);
        $$ = NULL;
    }
    ;

calc: %empty
    | calc docalc { (void)$2; }
    ;
%%

void yyerror(const char *s, ...) {
    va_list ap;
    va_start(ap, s);

    fflush(stdout);
    fprintf(stderr, "%d: error: ", yylineno);
    vfprintf(stderr, s, ap);
    fprintf(stderr, "\n");
    fflush(stderr);

    va_end(ap);
}
