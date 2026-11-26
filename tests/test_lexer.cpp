#include "diagnostics.h"
#include "parser.hpp"
#include "repl.h"
#include "user_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern int yylex();

namespace {

struct token {
    int kind = 0;
    std::string text;
};

std::vector<token> lex_source(const std::string &source) {
    reset_scanner_state_memory(source.data(), source.size());
    std::vector<token> tokens;
    for (;;) {
        int kind = yylex();
        if (kind == 0)
            break;
        token current;
        current.kind = kind;
        if (kind == SKEIN_TOK_NAME || kind == SKEIN_TOK_LABEL ||
            kind == SKEIN_TOK_XSTRING || kind == SKEIN_TOK_PLACEHOLDER ||
            kind == SKEIN_TOK_MACRO_ARG_EXPAND) {
            if (yylval.str)
                current.text = yylval.str;
            std::free(yylval.str);
            yylval.str = nullptr;
        }
        tokens.push_back(std::move(current));
    }
    return tokens;
}

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "lexer_test: %s\n", message);
        std::exit(1);
    }
}

int count_kind(const std::vector<token> &tokens, int kind) {
    int count = 0;
    for (const token &item : tokens) {
        if (item.kind == kind)
            ++count;
    }
    return count;
}

bool has_text(const std::vector<token> &tokens, int kind,
              const std::string &text) {
    for (const token &item : tokens) {
        if (item.kind == kind && item.text == text)
            return true;
    }
    return false;
}

bool has_member_name(const std::vector<token> &tokens,
                     const std::string &name) {
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i - 1].kind == '.' && tokens[i].kind == SKEIN_TOK_NAME &&
            tokens[i].text == name)
            return true;
    }
    return false;
}

void check_multiline_label() {
    std::vector<token> tokens = lex_source(
        "func f() {\n"
        "  done: return 1;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "multiline label was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "done"),
            "multiline label name mismatch");
}

void check_inline_label_is_name() {
    std::vector<token> tokens = lex_source(
        "func f() { done: return 1; }\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "inline label must not be a label token");
    require(has_text(tokens, SKEIN_TOK_NAME, "done"),
            "inline label name was not a normal identifier");
}

void check_dict_key_is_not_label() {
    std::vector<token> tokens = lex_source(
        "let d = {\n"
        "  key: 1\n"
        "};\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "dict key was tokenized as a label");
    require(has_text(tokens, SKEIN_TOK_NAME, "key"),
            "dict key name missing");
}

void check_struct_field_is_not_label() {
    std::vector<token> tokens = lex_source(
        "struct S {\n"
        "  key: int;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "struct field was tokenized as a label");
    require(has_text(tokens, SKEIN_TOK_NAME, "key"),
            "struct field name missing");
}

void check_typed_function_label() {
    std::vector<token> tokens = lex_source(
        "func f(): int {\n"
        "  done: return 1;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "label inside typed function was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "done"),
            "typed function label name mismatch");
}

void check_unicode_label() {
    std::string source = "func f() {\n  caf";
    source += "\xC3\xA9";
    source += ": return 1;\n}\n";
    std::vector<token> tokens = lex_source(source);
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "unicode label was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "caf\xC3\xA9"),
            "unicode label name mismatch");
}

void check_label_named_label() {
    std::vector<token> tokens = lex_source(
        "func f() {\n"
        "  label: return 1;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "label named label was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "label"),
            "label named label has wrong text");
}

void check_multiline_parameter_is_not_label() {
    std::vector<token> tokens = lex_source(
        "func f(\n"
        "  x: int\n"
        ") {\n"
        "  return x;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "parameter name was tokenized as a label");
    require(has_text(tokens, SKEIN_TOK_NAME, "x"),
            "parameter name missing");
}

void check_multiline_named_argument_is_not_label() {
    std::vector<token> tokens = lex_source(
        "f(\n"
        "  x: 1\n"
        ");\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "named argument was tokenized as a label");
    require(has_text(tokens, SKEIN_TOK_NAME, "x"),
            "named argument name missing");
}

void check_boolean_dict_key_is_not_label() {
    std::vector<token> tokens = lex_source(
        "let d = {\n"
        "  true: 1\n"
        "};\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 0,
            "boolean dict key was tokenized as a label");
    require(count_kind(tokens, SKEIN_TOK_TRUE) == 1,
            "boolean dict key did not remain a literal");
}

void check_label_body_block() {
    std::vector<token> tokens = lex_source(
        "func f() {\n"
        "  done: { return 1; }\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "label with block body was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "done"),
            "label with block body name mismatch");
}

void check_switch_arm_label() {
    std::vector<token> tokens = lex_source(
        "switch (1) {\n"
        "  case 1 =>\n"
        "    done: return 1;\n"
        "}\n");
    require(count_kind(tokens, SKEIN_TOK_LABEL) == 1,
            "switch arm label was not tokenized");
    require(has_text(tokens, SKEIN_TOK_LABEL, "done"),
            "switch arm label name mismatch");
}

void check_contextual_task_keywords() {
    std::vector<token> tokens = lex_source(
        "launch f();\n"
        "let x = await f();\n"
        "let launch = 1;\n"
        "println(await);\n"
        "value.await;\n"
        "value.launch;\n"
        "value.wait;\n");
    require(count_kind(tokens, SKEIN_TOK_LAUNCH) == 1,
            "launch keyword count mismatch");
    require(count_kind(tokens, SKEIN_TOK_AWAIT) == 1,
            "await keyword count mismatch");
    require(count_kind(tokens, SKEIN_TOK_WAIT) == 0,
            "wait must stay an identifier here");
    require(has_text(tokens, SKEIN_TOK_NAME, "launch"),
            "launch declaration name must stay an identifier");
    require(has_text(tokens, SKEIN_TOK_NAME, "await"),
            "await variable reference must stay an identifier");
    require(has_text(tokens, SKEIN_TOK_NAME, "wait"),
            "wait member name must stay an identifier");
}

void check_contextual_handler_keywords() {
    std::vector<token> tokens = lex_source(
        "f() on error(e) { recover 1; }\n"
        "let on = 1;\n"
        "let error = 2;\n"
        "println(on + error);\n"
        "value.on;\n"
        "value.error;\n"
        "error!(1);\n"
        "let e: error = null;\n");
    require(count_kind(tokens, SKEIN_TOK_ON) == 1,
            "handler-scoped on keyword count mismatch");
    require(count_kind(tokens, SKEIN_TOK_ERROR) == 1,
            "handler-scoped error keyword count mismatch");
    require(count_kind(tokens, SKEIN_TOK_MACRO_BANG) == 1,
            "error! must use the ordinary macro bang path");
    require(has_text(tokens, SKEIN_TOK_NAME, "on"),
            "let on must stay an identifier");
    require(has_text(tokens, SKEIN_TOK_NAME, "error"),
            "let error must stay an identifier");
    require(has_member_name(tokens, "on"),
            ".on must stay an ordinary member name");
    require(has_member_name(tokens, "error"),
            ".error must stay an ordinary member name");
}

void check_contextual_as_keyword() {
    std::vector<token> tokens = lex_source(
        "let x = 1 as float;\n"
        "let as = 2;\n"
        "println(as);\n"
        "value.as;\n"
        "as!(3);\n");
    require(count_kind(tokens, SKEIN_TOK_AS) == 1,
            "cast as keyword count mismatch");
    require(has_text(tokens, SKEIN_TOK_NAME, "as"),
            "let as must stay an identifier");
    require(has_member_name(tokens, "as"),
            ".as must stay an ordinary member name");
    require(count_kind(tokens, SKEIN_TOK_MACRO_BANG) == 1,
            "as! must use the ordinary macro bang path");
}

void check_contextual_ref_keyword() {
    std::vector<token> tokens = lex_source(
        "let ref = 1;\n"
        "println(ref);\n"
        "value.ref;\n"
        "ref!(2);\n"
        "let a: ref Point = null;\n"
        "let b = ref(a);\n");
    require(count_kind(tokens, SKEIN_TOK_REF) == 2,
            "ref prefix keyword count mismatch");
    require(has_text(tokens, SKEIN_TOK_NAME, "ref"),
            "let ref must stay an identifier");
    require(has_member_name(tokens, "ref"),
            ".ref must stay an ordinary member name");
    require(count_kind(tokens, SKEIN_TOK_MACRO_BANG) == 1,
            "ref! must use the ordinary macro bang path");
}

void check_contextual_weak_keyword() {
    std::vector<token> tokens = lex_source(
        "let weak = 1;\n"
        "println(weak);\n"
        "value.weak;\n"
        "weak!(2);\n"
        "let a: weak Point = null;\n"
        "let b = x as weak Point;\n");
    require(count_kind(tokens, SKEIN_TOK_WEAK) == 2,
            "weak prefix keyword count mismatch");
    require(has_text(tokens, SKEIN_TOK_NAME, "weak"),
            "let weak must stay an identifier");
    require(has_member_name(tokens, "weak"),
            ".weak must stay an ordinary member name");
    require(count_kind(tokens, SKEIN_TOK_MACRO_BANG) == 1,
            "weak! must use the ordinary macro bang path");
}

void check_user_macro_tokens() {
    std::vector<token> tokens = lex_source(
        "macro twice(x: expr) { return #x * 2; }\n"
        "call!(#args...);\n");
    require(count_kind(tokens, SKEIN_TOK_MACRO) == 1,
            "macro keyword was not tokenized");
    require(has_text(tokens, SKEIN_TOK_NAME,
                     USER_MACRO_PARAM_PREFIX "x"),
            "expr placeholder was not encoded as a parameter name");
    require(count_kind(tokens, SKEIN_TOK_MACRO_ARG_EXPAND) == 1,
            "expr... placeholder was not tokenized");
    require(has_text(tokens, SKEIN_TOK_MACRO_ARG_EXPAND,
                     USER_MACRO_EXPAND_PREFIX "args"),
            "expr... placeholder name mismatch");
}

} // namespace

int main() {
    diag_set_source_name("lexer_test");
    diag_set_package_name("main");

    check_multiline_label();
    check_inline_label_is_name();
    check_dict_key_is_not_label();
    check_struct_field_is_not_label();
    check_typed_function_label();
    check_unicode_label();
    check_label_named_label();
    check_multiline_parameter_is_not_label();
    check_multiline_named_argument_is_not_label();
    check_boolean_dict_key_is_not_label();
    check_label_body_block();
    check_switch_arm_label();
    check_contextual_task_keywords();
    check_contextual_handler_keywords();
    check_contextual_as_keyword();
    check_contextual_ref_keyword();
    check_contextual_weak_keyword();
    check_user_macro_tokens();

    std::printf("lexer_test: ok\n");
    return 0;
}
