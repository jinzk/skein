%{

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>

#include "ast.h"
#include "diagnostics.h"
#include "repl.h"
#include "scopestack.h"
#include "eval.h"

extern int yylex();
extern int yyparse();

static char *join_import_path(const char *left, const char *right) {
    size_t left_len = std::strlen(left);
    size_t right_len = std::strlen(right);
    char *path = (char *)std::malloc(left_len + right_len + 2);
    if (!path) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(path, left, left_len);
    path[left_len] = '.';
    std::memcpy(path + left_len + 1, right, right_len + 1);
    return path;
}
%}

%define parse.error custom
%define parse.lac full

%union {
    struct ast *a;
    struct symlist *sl;
    double d;
    long long i;
    int fn;
    char *str;
}

%token <d> FLOAT "float"
%token <i> INT "integer"
%token <str> NAME "identifier"
%token <fn> BUILTIN "built-in function"
%token <str> XSTRING "string"
%token EOL "end of line"
%token IF "if" ELSE "else" WHILE "while" FOR "for" FUNC "func" LET "let"
%token RETURN "return" BREAK "break" CONTINUE "continue"
%token NULLVAL "null"
%token IMPORT "import" PACKAGE "package" EXPORT "export"
%token LOGAND "&&" LOGOR "||" SHL "<<" SHR ">>"

%right '='
%left LOGOR
%left LOGAND
%left '|'
%left '^'
%left '&'
%nonassoc <fn> CMP
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%precedence UMINUS NOT BITNOT
%precedence '['
%precedence '.'
%precedence '('
%precedence NOELSE
%precedence ELSE

%type <a> exp value stmt list scope explist calclist docalc line valuelist pairlist
%type <a> forinit forcond forend
%type <sl> symlist
%type <str> importpath

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
    | funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($2, $4, $7); }
    | EXPORT LET NAME '=' exp ';'         { $$ = newvar($3, $5, true); }
    | EXPORT NAME '=' exp ';'             { $$ = newvar($2, $4, true); }
    | EXPORT funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($3, $5, $8, true); }
    | PACKAGE NAME '{' list '}'           { $$ = newpackage($2, $4); }
    | IMPORT importpath ';'               { $$ = newimport($2); }
    | LET NAME '=' exp ';'                { $$ = newvar($2, $4); }
    ;

importpath: NAME                          { $$ = $1; }
    | importpath '.' NAME                 {
        $$ = join_import_path($1, $3);
        free($1);
        free($3);
      }
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
    | exp '%' exp                         { $$ = newast('%', $1, $3); }
    | exp LOGAND exp                      { $$ = newast('A', $1, $3); }
    | exp LOGOR exp                       { $$ = newast('Q', $1, $3); }
    | exp '&' exp                         { $$ = newast('&', $1, $3); }
    | exp '|' exp                         { $$ = newast('|', $1, $3); }
    | exp '^' exp                         { $$ = newast('^', $1, $3); }
    | exp SHL exp                         { $$ = newast('s', $1, $3); }
    | exp SHR exp                         { $$ = newast('r', $1, $3); }
    | '(' exp ')'                         { $$ = $2; }
    | '-' exp %prec UMINUS                { $$ = newast('M', $2, NULL); }
    | '!' exp %prec NOT                   { $$ = newast('!', $2, NULL); }
    | '~' exp %prec BITNOT                { $$ = newast('~', $2, NULL); }
    | exp CMP exp                         { $$ = newcmp($2, $1, $3); }
    | NAME                                { $$ = newref($1); }
    | exp '=' exp                         { $$ = newasgn($1, $3); }
    | exp '(' explist ')'      { $$ = newcall($1, $3); }
    | exp '[' exp ']'                     { $$ = newast('G', $1, $3); }
    | exp '.' NAME                        { $$ = newast('G', $1, newstr($3)); free($3); }
    | '[' valuelist ']'                   { $$ = $2; }
    | '{' pairlist '}'                    { $$ = $2; }
    | funcstart '(' symlist ')' '{' list '}'
                                           { $$ = newfunc(NULL, $3, $6); }
    | value
    ;

funcstart: FUNC { push_func_location(g_token_line, g_token_column); }
    ;

value: INT                                { $$ = newint($1); }
    | FLOAT                               { $$ = newfloat($1); }
    | XSTRING                             { $$ = newstr($1); free($1); }
    | BUILTIN                             { $$ = newbuiltin($1); }
    | NULLVAL                             { $$ = newnullast(); }
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
        g_runtime_error_count = 0;
        diag_set_location(0, 0);

        bool invalid = g_statement_invalid;
        g_statement_invalid = false;
        bool skip_eval = invalid || (!g_interactive && g_script_stop);
        struct value_t *rslt = NULL;

        if (!skip_eval) {
            if (!g_package_loading)
                maybe_collect_cycles();

            rslt = eval($1);
            if (rslt && !g_interrupted && rslt->type != T_null) {
                switch (rslt->type) {
                    case T_return:
                        yyerror("return outside function");
                        break;
                    case T_break:
                        diag_set_location(g_flow_line, g_flow_column);
                        yyerror("break outside loop");
                        break;
                    case T_continue:
                        diag_set_location(g_flow_line, g_flow_column);
                        yyerror("continue outside loop");
                        break;
                    default:
                        if (g_interactive) {
                            printf("= ");
                            printvalue(rslt);
                            printf("\n");
                        }
                        break;
                }
            }
            freeval(rslt);
        }

        if (g_interactive && !g_interrupted) {
            printf("> ");
            fflush(stdout);
        }

        if ($1) treefree($1);
        $1 = NULL;
        $$ = NULL;
        if (g_interrupted || (!g_interactive && g_script_stop))
            YYABORT;
    }
    | error EOL {
        yyerrok;
        g_statement_invalid = false;
        if (!g_interactive && g_script_stop)
            YYABORT;
        if (g_interactive && !g_interrupted) {
            printf("> ");
            fflush(stdout);
        }
        $$ = NULL;
    }
    ;

calc: %empty
    | calc docalc { (void)$2; }
    ;
%%

void parser_begin_nested_parse() {
    yychar = YYEMPTY;
    yynerrs = 0;
}

int yyreport_syntax_error(const yypcontext_t *ctx) {
    if (diag_lexical_error_on_line(g_token_line)) {
        clear_func_locations();
        return 0;
    }

    enum { MAX_EXPECTED = 64, MAX_SHOWN = 6 };
    yysymbol_kind_t expected[MAX_EXPECTED];

    yysymbol_kind_t unexpected = yypcontext_token(ctx);
    std::string message;
    if (unexpected == YYSYMBOL_YYEMPTY) {
        message = "invalid syntax";
    } else {
        message = "unexpected ";
        message += yysymbol_name(unexpected);
    }

    int n = yypcontext_expected_tokens(ctx, expected, MAX_EXPECTED);
    if (n > 0) {
        int shown = n < MAX_SHOWN ? n : MAX_SHOWN;
        message += ", expecting ";
        for (int i = 0; i < shown; ++i) {
            if (i > 0)
                message += (i + 1 == shown && shown == n) ? " or " : ", ";
            message += yysymbol_name(expected[i]);
        }
        if (shown < n)
            message += ", ...";
    }

    diag_report(g_token_line, g_token_column, "syntax error",
                message.c_str());
    clear_func_locations();
    return 0;
}

void yyerror(const char *s, ...) {
    if (g_interrupted)
        return;

    ++g_runtime_error_count;

    char buffer[2048];
    va_list ap;
    va_start(ap, s);
    std::vsnprintf(buffer, sizeof(buffer), s, ap);
    va_end(ap);

    const char *message = buffer;
    const char *category = "runtime error";
    if (std::strncmp(message, "syntax error", 12) == 0) {
        category = "syntax error";
        message += 12;
        while (*message == ',' || *message == ' ')
            ++message;
        if (*message == '\0')
            message = "invalid syntax";
    }

    diag_report(g_error_line, g_error_column, category, message);
}
