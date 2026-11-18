#ifndef SKEIN_FORMATTER_H
#define SKEIN_FORMATTER_H

#include <string>

namespace skein {
namespace formatter {

/*
 * Format decoded UTF-8 Skein source.  Only trivia (whitespace, comments and
 * line breaks) is changed; every semantic token keeps its raw spelling.
 *
 * Returns false and fills `error` when the input cannot be tokenized or when
 * the formatter detects that the token stream would change.
 */
bool format_source(const std::string &source, std::string &formatted,
                   std::string &error);

} // namespace formatter
} // namespace skein

#endif
