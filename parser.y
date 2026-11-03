%{

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"
#include "eval_context.h"
#include "repl.h"
#include "scopestack.h"
#include "gc.h"
#include "eval.h"

extern int yylex();
extern int yyparse();

static int flow_keyword_line = 0;
static int flow_keyword_column = 0;
static std::vector<int> func_yield_counts;

static void start_function() {
    func_yield_counts.push_back(0);
}

static void note_yield() {
    if (!func_yield_counts.empty())
        ++func_yield_counts.back();
}

static bool finish_function() {
    bool generator =
        !func_yield_counts.empty() && func_yield_counts.back() > 0;
    if (!func_yield_counts.empty())
        func_yield_counts.pop_back();
    return generator;
}

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

static char *join_import_prefix(const char *prefix, char *path) {
    size_t prefix_len = std::strlen(prefix);
    size_t path_len = std::strlen(path);
    char *result = (char *)std::malloc(prefix_len + path_len + 1);
    if (!result) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(result, prefix, prefix_len);
    std::memcpy(result + prefix_len, path, path_len + 1);
    std::free(path);
    return result;
}

static char *copy_text(const char *text) {
    size_t len = std::strlen(text) + 1;
    char *copy = (char *)std::malloc(len);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text, len);
    return copy;
}

static const char *builtin_field_name(int functype) {
    switch (functype) {
        case B_sqrt: return "sqrt";
        case B_exp: return "exp";
        case B_log: return "log";
        case B_abs: return "abs";
        case B_sin: return "sin";
        case B_cos: return "cos";
        case B_tan: return "tan";
        case B_asin: return "asin";
        case B_acos: return "acos";
        case B_atan: return "atan";
        case B_atan2: return "atan2";
        case B_pow: return "pow";
        case B_floor: return "floor";
        case B_ceil: return "ceil";
        case B_round: return "round";
        case B_trunc: return "trunc";
        case B_log2: return "log2";
        case B_log10: return "log10";
        case B_hypot: return "hypot";
        case B_min: return "min";
        case B_max: return "max";
        case B_len: return "len";
        case B_range: return "range";
        case B_print: return "print";
        case B_list: return "list";
        case B_dict: return "dict";
        case B_int: return "int";
        case B_float: return "float";
        case B_bool: return "bool";
        case B_str: return "str";
        case B_char: return "char";
        case B_byte: return "byte";
        case B_kind_of: return "kind_of";
        case B_type_name: return "type_name";
        case B_type_module: return "type_module";
        case B_type_source: return "type_source";
        case B_format: return "format";
        case B_command: return "command";
        case B_command_status: return "command_status";
        case B_read_file: return "read_file";
        case B_write_file: return "write_file";
        case B_append_file: return "append_file";
        case B_file_exists: return "file_exists";
        case B_file_open: return "file_open";
        case B_file_read: return "file_read";
        case B_file_write: return "file_write";
        case B_file_seek: return "file_seek";
        case B_file_tell: return "file_tell";
        case B_file_eof: return "file_eof";
        case B_file_close: return "file_close";
        case B_file_flush: return "file_flush";
        case B_input: return "input";
        case B_split: return "split";
        case B_split_ws: return "split_ws";
        case B_trim: return "trim";
        case B_find: return "find";
        case B_substr: return "substr";
        case B_bytes_from_string: return "bytes_from_string";
        case B_bytes_to_string: return "bytes_to_string";
        case B_bytes_len: return "bytes_len";
        case B_bytes_get: return "bytes_get";
        case B_bytes_slice: return "bytes_slice";
        case B_bytes_concat: return "bytes_concat";
        case B_bytes_hex: return "bytes_hex";
        case B_bytes_from_hex: return "bytes_from_hex";
        case B_bytes_to_int: return "bytes_to_int";
        case B_int_to_bytes: return "int_to_bytes";
        case B_read_bytes: return "read_bytes";
        case B_write_bytes: return "write_bytes";
        case B_append_bytes: return "append_bytes";
        case B_file_read_bytes: return "file_read_bytes";
        case B_file_write_bytes: return "file_write_bytes";
        default: return NULL;
    }
}

static struct type_constraint *type_constraint_from_name(const char *name) {
    if (!name)
        return new_type_constraint(TYPE_INVALID);
    if (std::strcmp(name, "any") == 0) return new_type_constraint(TYPE_ANY);
    if (std::strcmp(name, "int") == 0) return new_type_constraint(TYPE_INT);
    if (std::strcmp(name, "float") == 0) return new_type_constraint(TYPE_FLOAT);
    if (std::strcmp(name, "number") == 0) return new_type_constraint(TYPE_NUMBER);
    if (std::strcmp(name, "bool") == 0) return new_type_constraint(TYPE_BOOL);
    if (std::strcmp(name, "string") == 0) return new_type_constraint(TYPE_STRING);
    if (std::strcmp(name, "char") == 0) return new_type_constraint(TYPE_CHAR);
    if (std::strcmp(name, "byte") == 0) return new_type_constraint(TYPE_BYTE);
    if (std::strcmp(name, "bytes") == 0) return new_type_constraint(TYPE_BYTES);
    if (std::strcmp(name, "list") == 0) return new_type_constraint(TYPE_LIST);
    if (std::strcmp(name, "dict") == 0) return new_type_constraint(TYPE_DICT);
    if (std::strcmp(name, "function") == 0) return new_type_constraint(TYPE_FUNCTION);
    if (std::strcmp(name, "builtin") == 0) return new_type_constraint(TYPE_BUILTIN);
    if (std::strcmp(name, "callable") == 0) return new_type_constraint(TYPE_CALLABLE);
    if (std::strcmp(name, "null") == 0) return new_type_constraint(TYPE_NULL);
    if (std::strcmp(name, "userdata") == 0) return new_type_constraint(TYPE_USERDATA);
    if (std::strcmp(name, "result") == 0) return new_type_constraint(TYPE_RESULT);
    return new_named_type_constraint(name,
                                     diag_current_source_name().c_str());
}

static struct type_constraint *type_constraint_from_builtin(int functype) {
    switch (functype) {
        case B_int: return new_type_constraint(TYPE_INT);
        case B_float: return new_type_constraint(TYPE_FLOAT);
        case B_bool: return new_type_constraint(TYPE_BOOL);
        case B_str: return new_type_constraint(TYPE_STRING);
        case B_char: return new_type_constraint(TYPE_CHAR);
        case B_byte: return new_type_constraint(TYPE_BYTE);
        case B_list: return new_type_constraint(TYPE_LIST);
        case B_dict: return new_type_constraint(TYPE_DICT);
        default: {
            const char *name = builtin_field_name(functype);
            yyerror("unknown type name %s", name ? name : "builtin");
            return new_type_constraint(TYPE_INVALID);
        }
    }
}
%}

%define parse.error custom
%define parse.lac full

%union {
    struct ast *a;
    struct symlist *sl;
    struct type_constraint *type;
    struct pattern_field *pf;
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
%token <i> CHAR "character"
%token EOL "end of line"
%token IF "if" ELSE "else" WHILE "while" DO "do" FOR "for" FUNC "func" LET "let" CONST "const"
%token ASYNC "async" AWAIT "await" YIELD "yield" DEFER "defer"
%token STRUCT "struct" IMPL "impl" TRAIT "trait" ENUM "enum"
%token SWITCH "switch" CASE "case" DEFAULT "default" ARROW "=>"
%token MATCH "match"
%token RETURN "return" BREAK "break" CONTINUE "continue"
%token FAIL "fail" ON "on" ERROR "error" RECOVER "recover" PASS "pass"
%token IS "is" LABEL "label"
%token COLONCOLON "::"
%token PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" MUL_ASSIGN "*=" DIV_ASSIGN "/="
%token MOD_ASSIGN "%=" AND_ASSIGN "&=" OR_ASSIGN "|=" XOR_ASSIGN "^="
%token SHL_ASSIGN "<<=" SHR_ASSIGN ">>="
%token NULLVAL "null" TRUE "true" FALSE "false"
%token IMPORT "import" PACKAGE "package" EXPORT "export" IN "in" AS "as"
%token LOGAND "&&" LOGOR "||" SHL "<<" SHR ">>"
%token INC "++" DEC "--"
%token ELLIPSIS "..."
%token EMPTY_DICT_CLOSE "empty dictionary"
%token EMPTY_BLOCK_CLOSE "empty block"

%right '=' PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%right AND_ASSIGN OR_ASSIGN XOR_ASSIGN SHL_ASSIGN SHR_ASSIGN
%right '?'
%left LOGOR
%left LOGAND
%left '|'
%left '^'
%left '&'
%nonassoc <fn> CMP
%precedence IS
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%precedence UMINUS NOT BITNOT
%right INC DEC
%precedence '['
%precedence '.'
%precedence '('
%precedence ON
%precedence NOELSE
%precedence ELSE

%type <a> exp value stmt list scope explist arg top_expression top_statement line valuelist pairlist
%type <a> forinit forcond forend
%type <a> switch_body switch_cases switch_case case_values
%type <a> match_arms match_arm pattern pattern_list pattern_items value_pattern match_values
%type <pf> pattern_fields pattern_field_items pattern_field
%type <a> let_pattern
%type <a> struct_decl impl_decl trait_decl struct_members struct_member
%type <a> enum_decl enum_variants enum_variant variant_fields variant_field
%type <a> impl_methods trait_methods trait_method method_member
%type <a> struct_fields struct_field
%type <sl> param symlist
%type <type> type type_name
%type <str> importref importpath fieldname

%destructor { treefree($$); } <a>
%destructor { free($$); } <str>
%destructor { symlistfree($$); } <sl>
%destructor { free_type_constraint($$); } <type>

%start program
%%

stmt: IF '(' exp ')' scope %prec NOELSE   { $$ = newflow('I', $3, $5, NULL); }
    | IF '(' exp ')' scope ELSE scope     { $$ = newflow('I', $3, $5, $7); }
    | WHILE '(' exp ')' scope             { $$ = newflow('W', $3, $5, NULL); }
    | DO scope eols WHILE '(' exp ')'     { $$ = newflow('E', $6, $2, NULL); }
    | SWITCH '(' exp ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newswitch($3, NULL); }
    | SWITCH '(' exp ')' '{' switch_body '}'
                                           { $$ = newswitch($3, $6); }
    | FOR '(' forinit ';' forcond ';' forend ')' scope
                                           { $$ = newfor($3, $5, $7, $9); }
    | FOR '(' LET NAME IN exp ')' scope   { $$ = newforin($4, NULL, $6, $8); }
    | FOR '(' LET NAME ',' NAME IN exp ')' scope
                                           { $$ = newforin($4, $6, $8, $10); }
    | DEFER exp ';'                       { $$ = newdefer($2); }
    | funcstart NAME '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($2, $4, NULL, false, false, finish_function()); }
    | funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($2, $4, $7, false, false, finish_function()); }
    | ASYNC funcstart NAME '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($3, $5, NULL,
                                                          false, true,
                                                          finish_function()); }
    | ASYNC funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($3, $5, $8,
                                                          false, true,
                                                          finish_function()); }
    | EXPORT LET NAME '=' exp ';'         { $$ = newvar($3, $5, true); }
    | EXPORT LET NAME ':' type '=' exp ';' {
        $$ = newvar($3, $7, true, false, $5);
      }
    | EXPORT LET NAME ':' type ';'        { $$ = newvar($3, NULL, true, false, $5); }
    | EXPORT CONST NAME '=' exp ';'       { $$ = newvar($3, $5, true, true); }
    | EXPORT CONST NAME ':' type '=' exp ';' {
        $$ = newvar($3, $7, true, true, $5);
      }
    | EXPORT NAME '=' exp ';'             { $$ = newvar($2, $4, true); }
    | EXPORT STRUCT NAME '{' empty_body  {
        $$ = newstructdecl($3,
                           copy_text(diag_current_source_name().c_str()),
                           NULL, true);
      }
    | EXPORT STRUCT NAME '{' struct_members '}' {
        $$ = newstructdecl($3,
                           copy_text(diag_current_source_name().c_str()),
                           (struct struct_member *)$5, true);
      }
    | EXPORT TRAIT NAME '{' empty_body   {
        $$ = newtraitdecl($3,
                          copy_text(diag_current_source_name().c_str()),
                          NULL, true);
      }
    | EXPORT TRAIT NAME '{' trait_methods '}' {
        $$ = newtraitdecl($3,
                          copy_text(diag_current_source_name().c_str()),
                          (struct trait_method *)$5, true);
      }
    | EXPORT ENUM NAME '{' empty_body    {
        $$ = newenumdecl($3,
                         copy_text(diag_current_source_name().c_str()),
                         NULL, true);
      }
    | EXPORT ENUM NAME '{' enum_variants '}' {
        $$ = newenumdecl($3,
                         copy_text(diag_current_source_name().c_str()),
                         (struct enum_variant *)$5, true);
      }
    | EXPORT funcstart NAME '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($3, $5, NULL, true, false, finish_function()); }
    | EXPORT funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($3, $5, $8, true, false, finish_function()); }
    | EXPORT ASYNC funcstart NAME '(' symlist ')' '{'
      EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($4, $6, NULL,
                                                          true, true,
                                                          finish_function()); }
    | EXPORT ASYNC funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newfunc($4, $6, $9,
                                                          true, true,
                                                          finish_function()); }
    | LABEL NAME ':' stmt                { $$ = newlabel($2, $4); }
    | struct_decl
    | impl_decl
    | trait_decl
    | enum_decl
    | PACKAGE NAME '{' EMPTY_BLOCK_CLOSE  { $$ = newpackage($2, NULL); }
    | PACKAGE NAME '{' list '}'           { $$ = newpackage($2, $4); }
    | IMPORT importref ';'                { $$ = newimport($2, NULL); }
    | IMPORT importref AS NAME ';'        { $$ = newimport($2, $4); }
    | LET NAME '=' exp ';'                { $$ = newvar($2, $4); }
    | LET NAME ':' type '=' exp ';'       { $$ = newvar($2, $6, false, false, $4); }
    | LET NAME ':' type ';'               { $$ = newvar($2, NULL, false, false, $4); }
    | LET let_pattern '=' exp ';'          { $$ = newdestructure($2, $4); }
    | CONST NAME '=' exp ';'              { $$ = newvar($2, $4, false, true); }
    | CONST NAME ':' type '=' exp ';'     { $$ = newvar($2, $6, false, true, $4); }
    | CONST let_pattern '=' exp ';'        { $$ = newdestructure($2, $4, true); }
    ;

empty_body: EMPTY_DICT_CLOSE
    | EMPTY_BLOCK_CLOSE
    ;

let_pattern: NAME '{' pattern_fields '}'  {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              $3);
      }
    | NAME '{' EMPTY_DICT_CLOSE           {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              NULL);
      }
    | NAME COLONCOLON NAME                {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | NAME COLONCOLON NAME '{' pattern_fields '}' {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, $5);
      }
    | NAME COLONCOLON NAME '{' EMPTY_DICT_CLOSE {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | '{' pattern_fields '}'              { $$ = newpatterndict($2); }
    | '{' EMPTY_DICT_CLOSE                { $$ = newpatterndict(NULL); }
    | '[' pattern_list ']'                { $$ = newpatternlist($2); }
    ;

struct_decl: STRUCT NAME '{' empty_body  {
        $$ = newstructdecl($2,
                           copy_text(diag_current_source_name().c_str()),
                           NULL);
      }
    | STRUCT NAME '{' struct_members '}' {
        $$ = newstructdecl($2,
                           copy_text(diag_current_source_name().c_str()),
                           (struct struct_member *)$4);
      }
    ;

enum_decl: ENUM NAME '{' empty_body      {
        $$ = newenumdecl($2,
                         copy_text(diag_current_source_name().c_str()),
                         NULL);
      }
    | ENUM NAME '{' enum_variants '}'    {
        $$ = newenumdecl($2,
                         copy_text(diag_current_source_name().c_str()),
                         (struct enum_variant *)$4);
      }
    ;

enum_variants: enum_variant              { $$ = $1; }
    | enum_variants enum_variant         {
        $$ = (struct ast *)append_enumvariant(
            (struct enum_variant *)$1, (struct enum_variant *)$2);
      }
    ;

enum_variant: NAME ';'                   { $$ = newenumvariant($1, NULL, NULL); }
    | NAME '{' variant_fields '}'        {
        $$ = newenumvariant($1, (struct struct_member *)$3, NULL);
      }
    | NAME '{' variant_fields '}' ';'    {
        $$ = newenumvariant($1, (struct struct_member *)$3, NULL);
      }
    | NAME '{' EMPTY_DICT_CLOSE          {
        yyerror("enum variant %s has an empty payload; write %s;",
                $1, $1);
        $$ = newenumvariant($1, NULL, NULL);
      }
    | NAME '{' EMPTY_DICT_CLOSE ';'      {
        yyerror("enum variant %s has an empty payload; write %s;",
                $1, $1);
        $$ = newenumvariant($1, NULL, NULL);
      }
    ;

variant_fields: variant_field            { $$ = $1; }
    | variant_fields variant_field       {
        $$ = append_struct_member($1, $2);
      }
    ;

variant_field: NAME ':' type ';'         { $$ = newstructfield($1, $3); }
    ;

struct_members: struct_member            { $$ = $1; }
    | struct_members struct_member       { $$ = append_struct_member($1, $2); }
    ;

struct_member: NAME ':' type ';'         { $$ = newstructfield($1, $3); }
    | method_member                       { $$ = $1; }
    ;

method_member: funcstart NAME '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newstructmethod($2, newfunc(NULL, $4, NULL, false, false, finish_function())); }
    | funcstart NAME '(' symlist ')' '{' list '}'
                                           { $$ = newstructmethod($2, newfunc(NULL, $4, $7, false, false, finish_function())); }
    ;

impl_decl: IMPL NAME '{' empty_body      {
        $$ = newimpl($2, NULL,
                     copy_text(diag_current_source_name().c_str()), NULL);
      }
    | IMPL NAME '{' impl_methods '}'     {
        $$ = newimpl($2, NULL,
                     copy_text(diag_current_source_name().c_str()),
                     (struct struct_member *)$4);
      }
    | IMPL NAME FOR NAME '{' empty_body  {
        $$ = newimpl($4, $2,
                     copy_text(diag_current_source_name().c_str()), NULL);
      }
    | IMPL NAME FOR NAME '{' impl_methods '}' {
        $$ = newimpl($4, $2,
                     copy_text(diag_current_source_name().c_str()),
                     (struct struct_member *)$6);
      }
    | IMPL NAME FOR NAME ';'             {
        $$ = newimpl($4, $2,
                     copy_text(diag_current_source_name().c_str()), NULL);
      }
    ;

impl_methods: method_member              { $$ = $1; }
    | impl_methods method_member         { $$ = append_struct_member($1, $2); }
    ;

trait_decl: TRAIT NAME '{' empty_body    {
        $$ = newtraitdecl($2,
                          copy_text(diag_current_source_name().c_str()),
                          NULL);
      }
    | TRAIT NAME '{' trait_methods '}'   {
        $$ = newtraitdecl($2,
                          copy_text(diag_current_source_name().c_str()),
                          (struct trait_method *)$4);
      }
    ;

trait_methods: trait_method              { $$ = $1; }
    | trait_methods trait_method         { $$ = append_trait_method($1, $2); }
    ;

trait_method: FUNC NAME '(' symlist ')' ';' { $$ = newtraitmethod($2, $4); }
    ;

struct_fields: struct_field              { $$ = $1; }
    | struct_field ',' struct_fields     { $$ = append_struct_init($1, $3); }
    ;

struct_field: NAME ':' exp               { $$ = newstructinit($1, $3); }
    ;

importref: importpath                     { $$ = $1; }
    | '.' importpath                      { $$ = join_import_prefix(".", $2); }
    | '.' '.' importpath                  { $$ = join_import_prefix("..", $3); }
    | '@' importpath                      { $$ = join_import_prefix("@", $2); }
    ;

importpath: NAME                          { $$ = $1; }
    | importpath '.' NAME                 {
        $$ = join_import_path($1, $3);
        free($1);
        free($3);
      }
    ;

fieldname: NAME                           { $$ = $1; }
    | YIELD                                { $$ = copy_text("yield"); }
    | MATCH                                { $$ = copy_text("match"); }
    | ENUM                                 { $$ = copy_text("enum"); }
    | STRUCT                               { $$ = copy_text("struct"); }
    | ERROR                                { $$ = copy_text("error"); }
    | BUILTIN {
        const char *name = builtin_field_name($1);
        if (name) {
            $$ = copy_text(name);
        } else {
            yyerror("invalid field name");
            $$ = copy_text("");
        }
      }
    ;

line: RETURN exp                          { $$ = newret($2); }
    | RETURN                              { $$ = newret(NULL); }
    | breakstart                          {
        $$ = newbrk(NULL, flow_keyword_line, flow_keyword_column);
      }
    | breakstart NAME                     {
        $$ = newbrk($2, flow_keyword_line, flow_keyword_column);
      }
    | continuestart                       {
        $$ = newctn(NULL, flow_keyword_line, flow_keyword_column);
      }
    | continuestart NAME                  {
        $$ = newctn($2, flow_keyword_line, flow_keyword_column);
      }
    | FAIL exp                            { $$ = newfail($2); }
    | FAIL                                { $$ = newfail(NULL); }
    | RECOVER exp                         { $$ = newrecover($2); }
    | RECOVER                             { $$ = newrecover(NULL); }
    | PASS exp                            { $$ = newpass($2); }
    | exp                                 { $$ = $1; }
    ;

breakstart: BREAK {
        flow_keyword_line = g_token_line;
        flow_keyword_column = g_token_column;
      }
    ;

continuestart: CONTINUE {
        flow_keyword_line = g_token_line;
        flow_keyword_column = g_token_column;
      }
    ;

scope: stmt                               { $$ = $1; }
     | line ';'                           { $$ = $1; }
     | ';'                                { $$ = newscope(NULL); }
     | '{' EMPTY_BLOCK_CLOSE              { $$ = newscope(NULL); }
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
    | MATCH '(' exp ')' '{' EMPTY_BLOCK_CLOSE
                                           {
        yyerror("match requires at least one arm");
        $$ = newmatch($3, NULL);
      }
    | MATCH '(' exp ')' '{' match_arms '}'
                                           { $$ = newmatch($3, $6); }
    | MATCH '(' exp ',' match_values ')' '{' EMPTY_BLOCK_CLOSE
                                           {
        yyerror("match requires at least one arm");
        $$ = newmatch(newlist($3, $5), NULL);
      }
    | MATCH '(' exp ',' match_values ')' '{' match_arms '}'
                                           { $$ = newmatch(newlist($3, $5), $8); }
    | AWAIT exp %prec UMINUS              { $$ = newawait($2); }
    | YIELD exp %prec '?'                  { note_yield(); $$ = newyield($2); }
    | YIELD %prec '?'                      { note_yield(); $$ = newyield(newnullast()); }
    | '-' exp %prec UMINUS                { $$ = newast('M', $2, NULL); }
    | '!' exp %prec NOT                   { $$ = newast('!', $2, NULL); }
    | '~' exp %prec BITNOT                { $$ = newast('~', $2, NULL); }
    | INC exp                             { $$ = newincdec($2, 1, true); }
    | DEC exp                             { $$ = newincdec($2, -1, true); }
    | exp INC                             { $$ = newincdec($1, 1, false); }
    | exp DEC                             { $$ = newincdec($1, -1, false); }
    | exp CMP exp                         { $$ = newcmp($2, $1, $3); }
    | exp IS type %prec IS                { $$ = newis($1, $3); }
    | exp '?' exp ':' exp %prec '?'       { $$ = newflow('T', $1, $3, $5); }
    | exp ON ERROR '(' NAME ')' '{' empty_body %prec ON {
        $$ = newonerror($1, $5, NULL);
      }
    | exp ON ERROR '(' NAME ')' '{' list '}' %prec ON {
        $$ = newonerror($1, $5, $8);
      }
    | NAME                                { $$ = newref($1); }
    | exp '=' exp                         { $$ = newasgn($1, $3); }
    | exp PLUS_ASSIGN exp                 { $$ = newcompound($1, '+', $3); }
    | exp MINUS_ASSIGN exp                { $$ = newcompound($1, '-', $3); }
    | exp MUL_ASSIGN exp                  { $$ = newcompound($1, '*', $3); }
    | exp DIV_ASSIGN exp                  { $$ = newcompound($1, '/', $3); }
    | exp MOD_ASSIGN exp                  { $$ = newcompound($1, '%', $3); }
    | exp AND_ASSIGN exp                  { $$ = newcompound($1, '&', $3); }
    | exp OR_ASSIGN exp                   { $$ = newcompound($1, '|', $3); }
    | exp XOR_ASSIGN exp                  { $$ = newcompound($1, '^', $3); }
    | exp SHL_ASSIGN exp                  { $$ = newcompound($1, 's', $3); }
    | exp SHR_ASSIGN exp                  { $$ = newcompound($1, 'r', $3); }
    | exp '(' explist ')'      { $$ = newcall($1, $3); }
    | exp '[' exp ']'                     { $$ = newast('G', $1, $3); }
    | exp '.' fieldname                   { $$ = newast('j', $1, newstr($3)); free($3); }
    | '[' valuelist ']'                   { $$ = $2; }
    | '{' EMPTY_DICT_CLOSE                { $$ = newdict(NULL, NULL, NULL); }
    | '{' pairlist '}'                    { $$ = $2; }
    | NAME '{' empty_body                 {
        $$ = newstructlit($1,
                          copy_text(diag_current_source_name().c_str()),
                          NULL);
      }
    | NAME '{' struct_fields '}'          {
        $$ = newstructlit($1,
                          copy_text(diag_current_source_name().c_str()),
                          (struct struct_init *)$3);
      }
    | NAME COLONCOLON NAME                {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, NULL);
      }
    | NAME COLONCOLON NAME '{' struct_fields '}' {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, (struct struct_init *)$5);
      }
    | NAME COLONCOLON NAME '{' EMPTY_DICT_CLOSE {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, NULL);
      }
    | funcstart '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc(NULL, $3, NULL, false, false, finish_function()); }
    | funcstart '(' symlist ')' '{' list '}'
                                           { $$ = newfunc(NULL, $3, $6, false, false, finish_function()); }
    | ASYNC funcstart '(' symlist ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc(NULL, $4, NULL,
                                                          false, true,
                                                          finish_function()); }
    | ASYNC funcstart '(' symlist ')' '{' list '}'
                                           { $$ = newfunc(NULL, $4, $7,
                                                          false, true,
                                                          finish_function()); }
    | value
    ;

funcstart: FUNC {
        push_func_location(g_token_line, g_token_column);
        start_function();
    }
    ;

value: INT                                { $$ = newint($1); }
    | FLOAT                               { $$ = newfloat($1); }
    | TRUE                                { $$ = newbool(true); }
    | FALSE                               { $$ = newbool(false); }
    | XSTRING                             { $$ = newstr($1); free($1); }
    | CHAR                                { $$ = newchar($1); }
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
    | LET NAME ':' type '=' exp           { $$ = newvar($2, $6, false, false, $4); }
    ;

forcond: %empty                           { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

forend: %empty                            { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

eols: %empty
    | EOL eols
    ;

switch_body: switch_cases                 { $$ = $1; }
    | DEFAULT ARROW scope                 { $$ = newcase(NULL, $3, NULL); }
    | switch_cases DEFAULT ARROW scope    {
        $$ = append_case($1, newcase(NULL, $4, NULL));
      }
    ;

switch_cases: switch_case                 { $$ = $1; }
    | switch_cases switch_case            { $$ = append_case($1, $2); }
    ;

switch_case: CASE case_values ARROW scope { $$ = newcase($2, $4, NULL); }
    ;

case_values: exp                          { $$ = newlist($1, NULL); }
    | exp ',' case_values                 { $$ = newlist($1, $3); }
    ;

match_arms: match_arm                     { $$ = $1; }
    | match_arms match_arm                { $$ = append_matcharm($1, $2); }
    ;

match_arm: CASE pattern ARROW scope       { $$ = newmatcharm($2, NULL, $4, NULL); }
    | CASE pattern IF exp ARROW scope     { $$ = newmatcharm($2, $4, $6, NULL); }
    | DEFAULT ARROW scope                 { $$ = newmatcharm(NULL, NULL, $3, NULL); }
    ;

pattern: INT                              { $$ = newpatternlit(newint($1)); }
    | XSTRING                             { $$ = newpatternlit(newstr($1)); free($1); }
    | CHAR                                { $$ = newpatternlit(newchar($1)); }
    | TRUE                                { $$ = newpatternlit(newbool(true)); }
    | FALSE                               { $$ = newpatternlit(newbool(false)); }
    | NULLVAL                             { $$ = newpatternlit(newnullast()); }
    | NAME                                { $$ = newpatternname($1); }
    | NAME '{' pattern_fields '}'         {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              $3);
      }
    | NAME '{' EMPTY_DICT_CLOSE           {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              NULL);
      }
    | NAME COLONCOLON NAME                {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | NAME COLONCOLON NAME '{' pattern_fields '}' {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, $5);
      }
    | NAME COLONCOLON NAME '{' EMPTY_DICT_CLOSE {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | '{' pattern_fields '}'              { $$ = newpatterndict($2); }
    | '{' EMPTY_DICT_CLOSE                { $$ = newpatterndict(NULL); }
    | '[' pattern_list ']'                { $$ = newpatternlist($2); }
    | pattern AS NAME                     { $$ = newpatternas($1, $3); }
    | CMP value_pattern                   {
        if ($1 != 4) {
            yyerror("only == is allowed in a value pattern");
            treefree($2);
            $$ = newpatternnever();
        } else {
            $$ = newpatternlit($2);
        }
      }
    ;

value_pattern: value                      { $$ = $1; }
    | NAME                                { $$ = newref($1); }
    | NAME COLONCOLON NAME                {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, NULL);
      }
    ;

match_values: exp                         { $$ = newlist($1, NULL); }
    | exp ',' match_values                { $$ = newlist($1, $3); }
    ;

pattern_fields: %empty                    { $$ = NULL; }
    | pattern_field_items
    | pattern_field_items ',' ELLIPSIS    {
        $$ = append_patternfield($1, newpatternrestfield(NULL, NULL));
      }
    | pattern_field_items ',' ELLIPSIS NAME {
        $$ = append_patternfield($1, newpatternrestfield($4, NULL));
      }
    | ELLIPSIS                            { $$ = newpatternrestfield(NULL, NULL); }
    | ELLIPSIS NAME                       { $$ = newpatternrestfield($2, NULL); }
    ;

pattern_field_items: pattern_field        { $$ = $1; }
    | pattern_field_items ',' pattern_field {
        $$ = append_patternfield($1, $3);
      }
    ;

pattern_field: NAME                       {
        $$ = newpatternfield($1, newpatternname(copy_text($1)), NULL);
      }
    | NAME ':' pattern                    { $$ = newpatternfield($1, $3, NULL); }
    | XSTRING ':' pattern                 { $$ = newpatternfield($1, $3, NULL); }
    ;

pattern_list: %empty                      { $$ = NULL; }
    | pattern_items
    | pattern_items ',' ELLIPSIS          {
        $$ = append_patternitem($1, newpatternrest(NULL));
      }
    | pattern_items ',' ELLIPSIS NAME     {
        $$ = append_patternitem($1, newpatternrest($4));
      }
    | ELLIPSIS                            { $$ = newpatternrest(NULL); }
    | ELLIPSIS NAME                       { $$ = newpatternrest($2); }
    ;

pattern_items: pattern                    { $$ = $1; }
    | pattern_items ',' pattern           { $$ = append_patternitem($1, $3); }
    ;

explist: %empty                            { $$ = NULL; }
    | arg
    | arg ',' explist                     { $$ = newast('L', $1, $3); }
    ;

arg: exp                                   { $$ = $1; }
    | ELLIPSIS exp                         { $$ = newspread($2); }
    ;

param: NAME {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, NULL, NULL);
      }
    | NAME ':' type {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, $3, NULL);
      }
    | ELLIPSIS NAME {
        struct symbol *s = newsym($2);
        free($2);
        $$ = newsymlist(s, NULL, NULL, true);
      }
    | ELLIPSIS NAME ':' type {
        struct symbol *s = newsym($2);
        free($2);
        $$ = newsymlist(s, $4, NULL, true);
      }
    ;

symlist: %empty                           { $$ = NULL; }
    | param                               { $$ = $1; }
    | param ',' symlist                   {
        $1->next = $3;
        $$ = $1;
      }
    ;

type: type_name                           { $$ = $1; }
    | type '?'                            { $$ = $1; $$->nullable = true; }
    ;

type_name: NAME                           {
        $$ = type_constraint_from_name($1);
        free($1);
      }
    | NAME COLONCOLON NAME                {
        std::string qualified = std::string($1) + "::" + $3;
        $$ = new_named_type_constraint(
            qualified.c_str(), diag_current_source_name().c_str());
        free($1);
        free($3);
      }
    | BUILTIN                             { $$ = type_constraint_from_builtin($1); }
    | FUNC                                { $$ = new_type_constraint(TYPE_FUNCTION); }
    | ERROR                               { $$ = new_type_constraint(TYPE_ERROR); }
    | NULLVAL                             { $$ = new_type_constraint(TYPE_NULL); }
    ;

top_expression: exp { $$ = $1; }
    | list { $$ = $1; }
    ;

top_statement: top_expression EOL {
        func_yield_counts.clear();
        active_eval_context().runtime_error_count = 0;
        diag_set_location(0, 0);

        bool invalid = g_statement_invalid;
        g_statement_invalid = false;
        bool skip_eval = invalid || (!g_interactive && g_script_stop);
        struct value_t *rslt = NULL;

        if (!skip_eval) {
            if (!g_package_loading)
                maybe_collect_cycles();

            rslt = eval($1);
            if (eval_control_pending())
                eval_finish_statement_control();
            if (g_interactive && rslt && rslt->type != T_null &&
                rslt->type != T_return && rslt->type != T_break &&
                rslt->type != T_continue) {
                repl_set_last_result(rslt);
            }
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
        $$ = NULL;
    }
    ;

program: %empty
    | program top_statement { (void)$2; }
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

    eval_context &context = active_eval_context();
    ++context.runtime_error_count;
    if (context.capture_errors &&
        std::strcmp(category, "runtime error") == 0) {
        if (!context.has_captured_error) {
            context.has_captured_error = true;
            context.captured_error = message;
        }
        return;
    }

    diag_report(g_error_line, g_error_column, category, message);
}
