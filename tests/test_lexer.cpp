#include "diagnostics.h"
#include "parser.hpp"
#include "repl.h"

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
            kind == SKEIN_TOK_XSTRING || kind == SKEIN_TOK_PLACEHOLDER) {
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

    std::printf("lexer_test: ok\n");
    return 0;
}
