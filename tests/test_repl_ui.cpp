#include "completion_index.h"
#include "repl_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using skein::repl_ui::completion_context;
using skein::repl_ui::token_kind;
using skein::repl_ui::token_span;

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "repl_ui_test: %s\n", message);
        std::exit(1);
    }
}

const token_span *find(const std::vector<token_span> &spans,
                       token_kind kind, const std::string &line) {
    for (const token_span &span : spans) {
        if (span.kind == kind)
            return &span;
    }
    (void)line;
    return nullptr;
}

void test_tokens() {
    const std::string line = "let x = \"hi\" // note";
    const std::vector<token_span> spans = skein::repl_ui::tokenize(line);
    require(find(spans, token_kind::keyword, line) != nullptr,
            "keyword token missing");
    require(find(spans, token_kind::string, line) != nullptr,
            "string token missing");
    require(find(spans, token_kind::comment, line) != nullptr,
            "comment token missing");

    const std::string unclosed = "\"unterminated";
    const std::vector<token_span> broken =
        skein::repl_ui::tokenize(unclosed);
    require(find(broken, token_kind::error, unclosed) != nullptr,
            "unclosed string should be an error token");

    const std::string command = ":load demo.skein";
    const std::vector<token_span> command_spans =
        skein::repl_ui::tokenize(command);
    require(command_spans.size() == 1 &&
                command_spans[0].kind == token_kind::command,
            "REPL command should be one command token");
}

void test_render() {
    const std::string line = "let x = 42";
    require(skein::repl_ui::render(line, false) == line,
            "plain render changed the line");
    const std::string colored = skein::repl_ui::render(line, true);
    require(colored.find("\x1b[36m") != std::string::npos,
            "keyword color missing");
    require(colored.find("\x1b[33m") != std::string::npos,
            "number color missing");
    require(colored.find(line) == std::string::npos,
            "colored render should not equal plain line");
}

void test_completion_context() {
    completion_context ctx = skein::repl_ui::completion_context_at(
        "std.str", 7);
    require(ctx.start == 4, "qualified completion start");
    require(ctx.prefix == "str", "qualified completion prefix");
    require(ctx.qualifier == "std", "qualified completion qualifier");
    require(!ctx.command, "qualified completion should not be a command");

    ctx = skein::repl_ui::completion_context_at("pri", 3);
    require(ctx.start == 0 && ctx.prefix == "pri", "bare completion");
    require(ctx.qualifier.empty(), "bare completion should have no qualifier");

    ctx = skein::repl_ui::completion_context_at(":lo", 3);
    require(ctx.command && ctx.start == 0 && ctx.prefix == ":lo",
            "command completion");

    ctx = skein::repl_ui::completion_context_at(":load fi", 8);
    require(ctx.command && ctx.start == 6 && ctx.prefix == "fi",
            "command argument context");

    ctx = skein::repl_ui::completion_context_at("let x = \"abc", 11);
    require(ctx.suppressed, "completion inside string should be suppressed");

    ctx = skein::repl_ui::completion_context_at("let x = 1 // note", 18);
    require(ctx.suppressed, "completion inside comment should be suppressed");
}

void test_indentation() {
    require(skein::repl_ui::indentation(0) == "", "zero indentation");
    require(skein::repl_ui::indentation(3) == "      ",
            "three indentation levels");
    require(skein::repl_ui::indentation(100).size() == 80,
            "indentation should be capped");

    std::string line = "      value";
    std::size_t cursor = 6;
    require(skein::repl_ui::dedent_for_closing(line, cursor, '}') == 4,
            "closing brace should dedent");
    require(line == "    value", "dedent should remove two spaces");

    line = "    x";
    cursor = 5;
    require(skein::repl_ui::dedent_for_closing(line, cursor, '}') == 5,
            "closing brace after code should not dedent");
    require(line == "    x", "non-leading code must stay unchanged");
}

void test_path_completions() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const fs::path root =
        fs::temp_directory_path() /
        ("skein-repl-ui-" + std::to_string((long long)stamp));
    fs::create_directories(root / "alps");
    std::ofstream(root / "alpha.skein") << "x";
    std::ofstream(root / "alpine.txt") << "x";

    const std::string prefix = (root / "al").u8string();
    const std::vector<std::string> candidates =
        skein::repl_ui::path_completions(prefix);
    require(candidates.size() == 3, "path completion count");
    require(std::is_sorted(candidates.begin(), candidates.end()),
            "path completions should be sorted");
    bool have_dir = false;
    for (const std::string &candidate : candidates) {
        if (candidate.back() == '/' || candidate.back() == '\\')
            have_dir = true;
    }
    require(have_dir, "directory completion should end with a separator");
    fs::remove_all(root);
}

void test_completion_index() {
    skein::completion::index index;
    index.add_top({"alpha", 6, "variable", false});
    index.add_top({"beta", 6, "variable", false});
    index.add_member("p", {"name", 6, "field", false});
    index.add_member("p", {"nick", 6, "field", false});

    const auto top = index.complete("a", "");
    require(top.size() == 1 && top[0].label == "alpha",
            "top-level completion index");
    const auto members = index.complete("n", "p");
    require(members.size() == 2, "member completion index");
    require(members[0].label == "name" && members[1].label == "nick",
            "member completion order");
    require(index.complete("x", "p").empty(),
            "empty completion index result");
}

void test_candidates() {
    std::vector<std::string> candidates = {"std.", "string", "std", "string"};
    candidates = skein::repl_ui::filter_and_sort(candidates, "st");
    require(candidates.size() == 3, "filter should remove duplicates");
    require(candidates[0] == "std" && candidates[1] == "std." &&
                candidates[2] == "string",
            "candidates should be sorted");

    const std::string common =
        skein::repl_ui::common_prefix({"std.", "string"});
    require(common == "st", "common prefix");
    require(skein::repl_ui::common_prefix({}).empty(),
            "empty common prefix");
    require(skein::repl_ui::common_prefix({"\xC3\xA9" "a", "\xC3\xA9" "b"}) ==
                "\xC3\xA9",
            "UTF-8 common prefix");
    require(skein::repl_ui::common_prefix({"\xC3\xA9", "\xC3\xAA"}).empty(),
            "common prefix must not split UTF-8");
}

}  // namespace

int main() {
    test_tokens();
    test_render();
    test_completion_context();
    test_indentation();
    test_path_completions();
    test_completion_index();
    test_candidates();
    return 0;
}
