#ifndef SKEIN_TYPE_TEXT_PARSER_H
#define SKEIN_TYPE_TEXT_PARSER_H

#include <string>
#include <vector>

struct type_constraint;

namespace skein {
namespace typecheck {

/*
 * Parse a comma-separated type argument list from source text.  This is used
 * by the re-entrant bracket-apply reparse path; it must stay independent of
 * the global Bison parser state.  Returned constraints are owned by the
 * caller and freed with free_type_constraint().
 */
bool parse_type_list_text(const std::string &text, const char *source,
                          std::vector<struct type_constraint *> &out,
                          std::string &error,
                          bool allow_named_generic = false);

} // namespace typecheck
} // namespace skein

#endif
