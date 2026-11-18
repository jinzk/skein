#ifndef SKEIN_TYPECHECK_H
#define SKEIN_TYPECHECK_H

#include <string>

struct ast;

namespace skein {
namespace typecheck {

enum class Mode {
    Off,
    Loose,
    Warn,
    Strict,
};

void set_mode(Mode mode);
Mode mode();
bool parse_mode(const char *text, Mode &mode);
const char *mode_name(Mode mode);

/* Check one top-level statement before evaluation.  Returns false when a
 * definite type error was reported; callers should skip evaluation. */
bool check_statement(struct ast *statement);

/* Drop persistent checker state associated with a source name.  Used when a
 * file is analyzed repeatedly (lint runs, language-server document versions). */
void forget_source(const std::string &source_name);

} // namespace typecheck
} // namespace skein

#endif
