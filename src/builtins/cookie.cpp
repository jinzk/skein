#include "cookie.h"

#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace {

void dict_set(value_t *dict, const std::string &key, value_t *value) {
    if (!dict || dict->type != T_dict) {
        freeval(value);
        return;
    }
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            key == ((string_value_t *)item_key)->value) {
            freeval(object->payload->items[i].second);
            object->payload->items[i].second = value;
            return;
        }
    }
    object->payload->items.push_back(
        std::make_pair(newstrval(key.c_str()), value));
}

value_t *dict_get(value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return NULL;
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            std::strcmp(((string_value_t *)item_key)->value, key) == 0)
            return object->payload->items[i].second;
    }
    return NULL;
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i)
        value[i] = (char)std::tolower((unsigned char)value[i]);
    return value;
}

std::string trim_ascii_ws(const std::string &value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace((unsigned char)value[start]))
        ++start;
    size_t end = value.size();
    while (end > start &&
           std::isspace((unsigned char)value[end - 1]))
        --end;
    return value.substr(start, end - start);
}

std::string normalize_cookie_domain(std::string domain) {
    domain = lower_ascii(trim_ascii_ws(domain));
    while (!domain.empty() && domain[0] == '.')
        domain.erase(domain.begin());
    return domain;
}

std::string default_cookie_path(const std::string &request_path) {
    if (request_path.empty() || request_path[0] != '/')
        return "/";
    size_t slash = request_path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return request_path.substr(0, slash);
}

bool cookie_domain_matches(const std::string &host,
                           const std::string &domain, bool host_only) {
    if (domain.empty())
        return false;
    if (host == domain)
        return true;
    if (host_only)
        return false;
    if (host.size() <= domain.size())
        return false;
    size_t offset = host.size() - domain.size();
    return host.compare(offset, domain.size(), domain) == 0 &&
           host[offset - 1] == '.';
}

bool cookie_path_matches(const std::string &request_path,
                         const std::string &cookie_path) {
    if (request_path == cookie_path)
        return true;
    if (request_path.size() < cookie_path.size())
        return false;
    if (request_path.compare(0, cookie_path.size(), cookie_path) != 0)
        return false;
    if (!cookie_path.empty() && cookie_path.back() == '/')
        return true;
    return request_path.size() > cookie_path.size() &&
           request_path[cookie_path.size()] == '/';
}

long long days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

bool parse_cookie_date(const std::string &text, long long &expires) {
    size_t comma = text.find(',');
    if (comma == std::string::npos)
        return false;
    std::string rest = text.substr(comma + 1);
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char month_text[4] = {0};
    if (std::sscanf(rest.c_str(), " %d %3s %d %d:%d:%d", &day,
                    month_text, &year, &hour, &minute, &second) != 6)
        return false;
    std::string month = lower_ascii(month_text);
    static const char *months[] = {"jan", "feb", "mar", "apr",
                                   "may", "jun", "jul", "aug",
                                   "sep", "oct", "nov", "dec"};
    int month_number = 0;
    for (int i = 0; i < 12; ++i) {
        if (month == months[i]) {
            month_number = i + 1;
            break;
        }
    }
    if (month_number == 0 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60)
        return false;
    long long days = days_from_civil(year, month_number, day);
    expires = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

value_t *parse_set_cookie(const std::string &raw,
                          const std::string &request_host,
                          const std::string &request_path, bool &ok) {
    ok = false;
    size_t semicolon = raw.find(';');
    std::string first = trim_ascii_ws(
        raw.substr(0, semicolon == std::string::npos ? raw.size()
                                                     : semicolon));
    size_t equals = first.find('=');
    if (equals == std::string::npos || equals == 0)
        return NULL;
    std::string name = trim_ascii_ws(first.substr(0, equals));
    std::string value = trim_ascii_ws(first.substr(equals + 1));
    if (name.empty())
        return NULL;

    std::string domain;
    bool host_only = true;
    std::string path = default_cookie_path(request_path);
    bool secure = false;
    bool http_only = false;
    long long max_age = -1;
    long long expires = 0;
    std::string same_site;

    size_t position =
        semicolon == std::string::npos ? raw.size() : semicolon + 1;
    while (position < raw.size()) {
        size_t next = raw.find(';', position);
        std::string attribute = trim_ascii_ws(
            raw.substr(position, next == std::string::npos
                                     ? std::string::npos
                                     : next - position));
        position = next == std::string::npos ? raw.size() : next + 1;
        if (attribute.empty())
            continue;
        size_t attribute_equals = attribute.find('=');
        std::string attribute_name = lower_ascii(trim_ascii_ws(
            attribute.substr(0, attribute_equals)));
        std::string attribute_value =
            attribute_equals == std::string::npos
                ? std::string()
                : trim_ascii_ws(attribute.substr(attribute_equals + 1));
        if (attribute_name == "domain") {
            domain = normalize_cookie_domain(attribute_value);
            host_only = false;
        } else if (attribute_name == "path") {
            if (!attribute_value.empty() && attribute_value[0] == '/')
                path = attribute_value;
        } else if (attribute_name == "secure") {
            secure = true;
        } else if (attribute_name == "httponly") {
            http_only = true;
        } else if (attribute_name == "max-age") {
            char *end = NULL;
            long long parsed =
                std::strtoll(attribute_value.c_str(), &end, 10);
            if (end && *end == '\0')
                max_age = parsed;
        } else if (attribute_name == "expires") {
            long long parsed = 0;
            if (parse_cookie_date(attribute_value, parsed))
                expires = parsed;
        } else if (attribute_name == "samesite") {
            same_site = attribute_value;
        }
    }

    std::string host = lower_ascii(request_host);
    if (host_only) {
        domain = host;
    } else if (domain.empty()) {
        return NULL;
    } else if (!host.empty() &&
               !cookie_domain_matches(host, domain, false)) {
        return NULL;
    }

    value_t *cookie = (value_t *)newdictval();
    dict_set(cookie, "name", newstrval(name.c_str()));
    dict_set(cookie, "value", newstrval(value.c_str()));
    dict_set(cookie, "domain", newstrval(domain.c_str()));
    dict_set(cookie, "path", newstrval(path.c_str()));
    dict_set(cookie, "secure", newboolval(secure));
    dict_set(cookie, "http_only", newboolval(http_only));
    dict_set(cookie, "max_age", newintval(max_age));
    dict_set(cookie, "expires", newintval(expires));
    dict_set(cookie, "same_site", newstrval(same_site.c_str()));
    dict_set(cookie, "host_only", newboolval(host_only));
    ok = true;
    return cookie;
}

void dict_delete(value_t *dict, const std::string &key) {
    if (!dict || dict->type != T_dict)
        return;
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            key == ((string_value_t *)item_key)->value) {
            freeval(object->payload->items[i].first);
            freeval(object->payload->items[i].second);
            object->payload->items.erase(
                object->payload->items.begin() + (std::ptrdiff_t)i);
            return;
        }
    }
}

std::string cookie_header(value_t *jar, const std::string &host,
                          const std::string &path, bool secure) {
    if (!jar || jar->type != T_dict)
        return std::string();
    struct cookie_entry {
        std::string text;
        std::size_t path_length;
    };
    std::vector<cookie_entry> entries;
    long long now = (long long)std::time(NULL);
    dict_value_t *dict = (dict_value_t *)jar;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *cookie = dict->payload->items[i].second;
        if (!cookie || cookie->type != T_dict)
            continue;
        value_t *name = dict_get(cookie, "name");
        value_t *value = dict_get(cookie, "value");
        value_t *domain = dict_get(cookie, "domain");
        value_t *cookie_path = dict_get(cookie, "path");
        value_t *secure_value = dict_get(cookie, "secure");
        value_t *expires_value = dict_get(cookie, "expires");
        value_t *host_only_value = dict_get(cookie, "host_only");
        if (!name || name->type != T_string || !value ||
            value->type != T_string || !domain ||
            domain->type != T_string || !cookie_path ||
            cookie_path->type != T_string)
            continue;
        bool secure_cookie = secure_value && secure_value->type == T_bool &&
                             ((bool_value_t *)secure_value)->value;
        if (secure_cookie && !secure)
            continue;
        long long expires =
            expires_value && expires_value->type == T_int
                ? ((int_value_t *)expires_value)->value
                : 0;
        if (expires > 0 && expires <= now)
            continue;
        bool host_only = host_only_value &&
                         host_only_value->type == T_bool &&
                         ((bool_value_t *)host_only_value)->value;
        if (!cookie_domain_matches(lower_ascii(host),
                                   ((string_value_t *)domain)->value,
                                   host_only))
            continue;
        std::string cookie_path_text =
            ((string_value_t *)cookie_path)->value;
        if (!cookie_path_matches(path, cookie_path_text))
            continue;
        cookie_entry entry;
        entry.text = ((string_value_t *)name)->value;
        entry.text += "=";
        entry.text += ((string_value_t *)value)->value;
        entry.path_length = cookie_path_text.size();
        entries.push_back(entry);
    }
    std::stable_sort(
        entries.begin(), entries.end(),
        [](const cookie_entry &left, const cookie_entry &right) {
            return left.path_length > right.path_length;
        });
    std::string header;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!header.empty())
            header += "; ";
        header += entries[i].text;
    }
    return header;
}

void update_cookie_jar(value_t *jar, value_t *response,
                       const std::string &host, const std::string &path,
                       bool secure) {
    (void)secure;
    if (!jar || jar->type != T_dict)
        return;
    value_t *list = dict_get(response, "set_cookie");
    if (!list || list->type != T_list)
        return;
    list_value_t *items = (list_value_t *)list;
    long long now = (long long)std::time(NULL);
    for (size_t i = 0; i < items->payload->items.size(); ++i) {
        value_t *raw = items->payload->items[i];
        if (!raw || raw->type != T_string)
            continue;
        bool ok = false;
        value_t *cookie = parse_set_cookie(
            ((string_value_t *)raw)->value, host, path, ok);
        if (!ok) {
            freeval(cookie);
            continue;
        }
        value_t *max_age_value = dict_get(cookie, "max_age");
        long long max_age =
            max_age_value && max_age_value->type == T_int
                ? ((int_value_t *)max_age_value)->value
                : -1;
        value_t *expires_value = dict_get(cookie, "expires");
        long long expires =
            expires_value && expires_value->type == T_int
                ? ((int_value_t *)expires_value)->value
                : 0;
        if (max_age >= 0) {
            if (max_age == 0)
                expires = 1;
            else
                expires = now + max_age;
        }
        value_t *domain_value = dict_get(cookie, "domain");
        value_t *path_value = dict_get(cookie, "path");
        value_t *name_value = dict_get(cookie, "name");
        std::string domain = ((string_value_t *)domain_value)->value;
        std::string cookie_path = ((string_value_t *)path_value)->value;
        std::string name = ((string_value_t *)name_value)->value;
        std::string key = domain + "\t" + cookie_path + "\t" + name;
        if (expires > 0 && expires <= now) {
            dict_delete(jar, key);
            freeval(cookie);
            continue;
        }
        dict_set(cookie, "expires", newintval(expires));
        dict_set(jar, key, cookie);
    }
}


} // namespace

std::string cookie_header_value(value_t *jar, const std::string &host,
                                const std::string &path, bool secure) {
    return cookie_header(jar, host, path, secure);
}

void cookie_jar_update(value_t *jar, value_t *response,
                       const std::string &host, const std::string &path,
                       bool secure) {
    update_cookie_jar(jar, response, host, path, secure);
}

value_t *parse_cookie_header(const std::string &raw,
                             const std::string &request_host,
                             const std::string &request_path, bool &ok) {
    return parse_set_cookie(raw, request_host, request_path, ok);
}
