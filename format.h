#ifndef CALC_FORMAT_H
#define CALC_FORMAT_H

#include <string>
#include <vector>

#include "valuetype.h"

/*
 * Python-style positional formatting with a supported subset of the format
 * mini-language: {}, {0}, {{, }}, {:s}, {:d}, {:f}, {:.2f}, {:x}, {:X},
 * and optional [fill]align/width.  Extra arguments are ignored.
 */
bool format_values(const std::string &fmt,
                   const std::vector<struct value_t *> &args,
                   std::string &out,
                   std::string &error);

#endif
