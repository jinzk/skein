#ifndef SKEIN_REPL_UI_H
#define SKEIN_REPL_UI_H

#include <cstddef>
#include <string>
#include <vector>

namespace skein {
namespace repl_ui {

enum class token_kind {
    plain,
    keyword,
    string,
    comment,
    number,
    command,
    error
};

struct token_span {
    std::size_t start = 0;
    std::size_t end = 0;
    token_kind kind = token_kind::plain;
};

/* Language keywords, used by both highlighting and completion. */
const std::vector<std::string> &keywords();

/* Lexical spans for one REPL input line.  This is intentionally independent
 * of the parser and never executes or type-checks user code. */
std::vector<token_span> tokenize(const std::string &line);

/* Render one line with ANSI colors.  When `color` is false the line is
 * returned unchanged. */
std::string render(const std::string &line, bool color);

struct completion_context {
    std::size_t start = 0;
    std::string prefix;
    std::string qualifier;
    bool command = false;
    bool suppressed = false;
};

/* Find the word and optional dotted qualifier at `cursor`. */
completion_context completion_context_at(const std::string &line,
                                         std::size_t cursor);

/* Filter candidates by prefix, sort, and remove duplicates. */
std::vector<std::string> filter_and_sort(
    const std::vector<std::string> &candidates, const std::string &prefix);

/* Longest common prefix of all candidates. */
std::string common_prefix(const std::vector<std::string> &candidates);

/* Spaces for one or more indentation levels. */
std::string indentation(int level, int width = 2);

/* Dedent a line whose cursor is inside leading indentation when the user
 * types a closing delimiter.  Returns the new cursor. */
std::size_t dedent_for_closing(std::string &line, std::size_t cursor,
                               char closing, int width = 2);

}  // namespace repl_ui
}  // namespace skein

#endif
