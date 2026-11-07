#ifndef SKEIN_TYPECHECK_H
#define SKEIN_TYPECHECK_H

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

} // namespace typecheck
} // namespace skein

#endif
