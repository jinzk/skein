#ifndef SKEIN_COOKIE_H
#define SKEIN_COOKIE_H

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

/* Build one Set-Cookie header value.  Returns an empty string on invalid
 * input.  max_age < 0 omits Max-Age. */
std::string cookie_set_header(const std::string &name,
                              const std::string &value,
                              const std::string &path,
                              const std::string &domain, long long max_age,
                              bool secure, bool http_only,
                              const std::string &same_site);

#endif
