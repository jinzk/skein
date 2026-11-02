#ifndef CALC_COOKIE_H
#define CALC_COOKIE_H

#include "valuetype.h"

#include <string>

std::string cookie_header_value(value_t *jar, const std::string &host,
                                const std::string &path, bool secure);

void cookie_jar_update(value_t *jar, value_t *response,
                       const std::string &host, const std::string &path,
                       bool secure);

value_t *parse_cookie_header(const std::string &raw,
                             const std::string &request_host,
                             const std::string &request_path, bool &ok);

#endif
