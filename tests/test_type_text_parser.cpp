#include "type_text_parser.h"

#include "static_type.h"
#include "symbol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using skein::typecheck::parse_type_list_text;
using skein::typecheck::type_from_constraint;
using skein::typecheck::type_display;

static int failures = 0;

static void expect(bool condition, const char *message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

static void free_types(std::vector<struct type_constraint *> &types) {
    for (struct type_constraint *type : types)
        free_type_constraint(type);
    types.clear();
}

static void check(const char *text, const char *expected,
                  bool allow_named_generic = false) {
    std::vector<struct type_constraint *> types;
    std::string error;
    bool ok = parse_type_list_text(text, "test.skein", types, error,
                                   allow_named_generic);
    if (!ok) {
        std::fprintf(stderr, "FAIL: parse %s: %s\n", text, error.c_str());
        ++failures;
        free_types(types);
        return;
    }
    expect(types.size() == 1, "expected one parsed type");
    if (types.size() == 1) {
        const char *display = type_display(type_from_constraint(types[0]));
        if (std::strcmp(display, expected) != 0) {
            std::fprintf(stderr, "FAIL: %s -> %s, expected %s\n", text,
                         display, expected);
            ++failures;
        }
    }
    free_types(types);
}

static void check_list(const char *text, size_t expected_count) {
    std::vector<struct type_constraint *> types;
    std::string error;
    bool ok = parse_type_list_text(text, "test.skein", types, error);
    expect(ok, "type list should parse");
    expect(types.size() == expected_count, "unexpected type count");
    free_types(types);
}

static void check_error(const char *text) {
    std::vector<struct type_constraint *> types;
    std::string error;
    bool ok = parse_type_list_text(text, "test.skein", types, error);
    if (ok)
        std::fprintf(stderr, "FAIL: invalid type accepted: %s\n", text);
    expect(!ok, "invalid type text should fail");
    if (error.empty())
        std::fprintf(stderr, "FAIL: invalid type without error: %s\n", text);
    expect(!error.empty(), "invalid type text should report an error");
    free_types(types);
}

int main() {
    check("int", "int");
    check("string", "string");
    check("Foo", "Foo");
    check("pkg.Foo", "pkg.Foo");
    check("list[int]", "list[int]");
    check("dict[string, list[int]]", "dict[string, list[int]]");
    check("list[int]?", "list[int]?");
    check("Box[int]?", "Box[int]?", true);
    check("ref Foo", "ref Foo");
    check("weak Foo", "weak Foo");
    check("(int, string) -> bool", "(int, string) -> bool");
    check("(list[int]) -> dict[string, int]", "(list[int]) -> dict[string, int]");
    check("int | string", "union[int, string]");
    check("int | string | bool", "union[int, string, bool]");
    check("(int | string)?", "union[int, string, null]");
    check("int | string?", "union[int, string, null]");
    check("list[int | string]", "list[union[int, string]]");
    check("((int | string) -> bool)", "(union[int, string]) -> bool");
    check_list("int, string, bool", 3);
    check_list("int | string, bool", 2);
    check_list("/* comment */ int, // line\n string", 2);

    check_error("");
    check_error("list[]");
    check_error("list[int");
    check_error("int string");
    check_error("(int -> bool");
    check_error("(int, string)");

    if (failures == 0)
        std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
