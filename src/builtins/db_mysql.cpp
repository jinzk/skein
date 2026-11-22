#include "db_internal.h"

#include "symbol.h"
#include "valuetype.h"

#include <mysql.h>

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr unsigned int kDefaultPort = 3306;

/*
 * Older MySQL clients used my_bool, while MariaDB Connector/C and MySQL 8
 * use bool.  Derive the exact null-indicator element type from MYSQL_BIND so
 * the backend builds against both client families.
 */
using mysql_bool =
    std::remove_pointer_t<decltype(((MYSQL_BIND *)nullptr)->is_null)>;

struct mysql_param {
    long long integer = 0;
    double real = 0;
    signed char boolean = 0;
    std::string text;
    unsigned long length = 0;
    mysql_bool is_null = 0;
};

struct mysql_result_binding {
    std::vector<MYSQL_BIND> binds;
    std::vector<unsigned long> lengths;
    std::deque<mysql_bool> nulls;
    std::vector<char> dummies;
    std::vector<long long> integers;
    std::vector<double> reals;
};

struct mysql_uri {
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    unsigned int port = kDefaultPort;
};

struct mysql_backend final : db_backend {
    MYSQL *conn = nullptr;

    explicit mysql_backend(MYSQL *connection) : conn(connection) {}

    ~mysql_backend() override { close(); }

    db_dialect dialect() const override { return db_dialect::mysql; }

    bool is_open() const override { return conn != nullptr; }

    void close() override {
        if (conn) {
            mysql_close(conn);
            conn = nullptr;
        }
    }

    struct value_t *exec(const char *sql,
                         struct value_t *params) override;
    struct value_t *query(const char *sql,
                          struct value_t *params) override;
    struct value_t *transaction(const char *sql) override;

  private:
    struct value_t *simple_exec(const char *sql);
    struct value_t *simple_query(const char *sql);
    struct value_t *prepared_exec(const char *sql, struct value_t *params);
    struct value_t *prepared_query(const char *sql, struct value_t *params);
};

struct value_t *mysql_failure(MYSQL *conn, const char *action) {
    yyerror("std.db %s: %s", action,
            conn ? mysql_error(conn) : "connection failed");
    return NULL;
}

struct value_t *mysql_stmt_failure(MYSQL_STMT *stmt, const char *action) {
    yyerror("std.db %s: %s", action,
            stmt ? mysql_stmt_error(stmt) : "statement failed");
    return NULL;
}

void mysql_ensure_thread() {
    static std::once_flag library_once;
    std::call_once(library_once, [] { mysql_library_init(0, NULL, NULL); });
    static thread_local bool initialized = false;
    if (!initialized) {
        mysql_thread_init();
        initialized = true;
    }
}

bool mysql_hex_digit(char c, int &value) {
    if (c >= '0' && c <= '9') {
        value = c - '0';
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
        return true;
    }
    return false;
}

bool mysql_percent_decode(const std::string &input, std::string &output,
                          std::string &error) {
    output.clear();
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            output.push_back(input[i]);
            continue;
        }
        if (i + 2 >= input.size()) {
            error = "invalid percent escape in MySQL URI";
            return false;
        }
        int high = 0;
        int low = 0;
        if (!mysql_hex_digit(input[i + 1], high) ||
            !mysql_hex_digit(input[i + 2], low)) {
            error = "invalid percent escape in MySQL URI";
            return false;
        }
        output.push_back((char)((high << 4) | low));
        i += 2;
    }
    return true;
}

bool mysql_parse_uri(const std::string &uri, mysql_uri &out,
                     std::string &error) {
    const size_t scheme = uri.find("://");
    if (scheme == std::string::npos) {
        error = "MySQL URI must contain ://";
        return false;
    }
    std::string rest = uri.substr(scheme + 3);
    std::string authority = rest;
    std::string path;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        authority = rest.substr(0, slash);
        path = rest.substr(slash + 1);
    }
    const size_t query = path.find('?');
    if (query != std::string::npos)
        path = path.substr(0, query);

    std::string userinfo;
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }

    std::string host = authority;
    std::string port_text;
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) {
            error = "invalid IPv6 host in MySQL URI";
            return false;
        }
        const std::string bracketed = host.substr(1, close - 1);
        const std::string tail = host.substr(close + 1);
        host = bracketed;
        if (!tail.empty()) {
            if (tail[0] != ':') {
                error = "invalid host in MySQL URI";
                return false;
            }
            port_text = tail.substr(1);
        }
    } else {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos &&
            host.find(':') == colon) {
            port_text = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
    }

    if (!port_text.empty()) {
        char *end = NULL;
        errno = 0;
        unsigned long port = std::strtoul(port_text.c_str(), &end, 10);
        if (errno != 0 || end == port_text.c_str() ||
            *end != '\0' || port == 0 || port > 65535) {
            error = "invalid port in MySQL URI";
            return false;
        }
        out.port = (unsigned int)port;
    }

    if (!userinfo.empty()) {
        const size_t colon = userinfo.find(':');
        if (colon == std::string::npos) {
            if (!mysql_percent_decode(userinfo, out.user, error))
                return false;
        } else {
            if (!mysql_percent_decode(userinfo.substr(0, colon), out.user,
                                      error) ||
                !mysql_percent_decode(userinfo.substr(colon + 1),
                                      out.password, error))
                return false;
        }
    }
    if (!mysql_percent_decode(host, out.host, error) ||
        !mysql_percent_decode(path, out.database, error))
        return false;
    return true;
}

bool mysql_is_binary_field(const MYSQL_FIELD &field) {
    return field.charsetnr == 63;
}

bool mysql_is_integer_type(enum enum_field_types type) {
    switch (type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
            return true;
        default:
            return false;
    }
}

bool mysql_is_float_type(enum enum_field_types type) {
    return type == MYSQL_TYPE_FLOAT || type == MYSQL_TYPE_DOUBLE;
}

struct value_t *mysql_value(const MYSQL_FIELD &field, const char *data,
                            unsigned long length) {
    const enum enum_field_types type = field.type;
    if (mysql_is_integer_type(type)) {
        std::string text(data, (size_t)length);
        char *end = NULL;
        errno = 0;
        long long value = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end != text.c_str() && *end == '\0')
            return newintval(value);
        return newstrval(text.c_str(), text.size());
    }
    if (mysql_is_float_type(type)) {
        std::string text(data, (size_t)length);
        char *end = NULL;
        errno = 0;
        double value = std::strtod(text.c_str(), &end);
        if (errno == 0 && end != text.c_str() && *end == '\0')
            return newfloatval(value);
        return newstrval(text.c_str(), text.size());
    }
    if (type == MYSQL_TYPE_DECIMAL || type == MYSQL_TYPE_NEWDECIMAL ||
        type == MYSQL_TYPE_DATE || type == MYSQL_TYPE_NEWDATE ||
        type == MYSQL_TYPE_TIME || type == MYSQL_TYPE_DATETIME ||
        type == MYSQL_TYPE_TIMESTAMP)
        return newstrval(data, (size_t)length);
    if (mysql_is_binary_field(field))
        return newbytesval(std::string(data, (size_t)length));
    return newstrval(data, (size_t)length);
}

bool mysql_bind_params(MYSQL_STMT *stmt, struct value_t *params,
                       std::vector<MYSQL_BIND> &binds,
                       std::vector<mysql_param> &storage) {
    if (!params || params->type == T_null)
        return true;
    if (params->type != T_list) {
        yyerror("std.db parameters must be a list or null");
        return false;
    }

    list_value_t *list = (list_value_t *)params;
    const size_t count = list->payload->items.size();
    binds.assign(count, MYSQL_BIND());
    storage.assign(count, mysql_param());
    for (size_t i = 0; i < count; ++i) {
        struct value_t *value = list->payload->items[i];
        MYSQL_BIND &bind = binds[i];
        mysql_param &param = storage[i];
        std::memset(&bind, 0, sizeof(bind));
        bind.is_null = &param.is_null;
        if (!value || value->type == T_null) {
            param.is_null = 1;
            bind.buffer_type = MYSQL_TYPE_NULL;
        } else if (value->type == T_int) {
            param.integer = ((int_value_t *)value)->value;
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &param.integer;
            bind.buffer_length = sizeof(param.integer);
        } else if (value->type == T_bool) {
            param.boolean = ((bool_value_t *)value)->value ? 1 : 0;
            bind.buffer_type = MYSQL_TYPE_TINY;
            bind.buffer = &param.boolean;
            bind.buffer_length = sizeof(param.boolean);
        } else if (value->type == T_float) {
            param.real = ((float_value_t *)value)->value;
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = &param.real;
            bind.buffer_length = sizeof(param.real);
        } else if (value->type == T_string) {
            param.text = ((string_value_t *)value)->value;
            param.length = (unsigned long)param.text.size();
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = param.text.data();
            bind.buffer_length = param.length;
            bind.length = &param.length;
        } else if (value->type == T_bytes) {
            param.text = ((bytes_value_t *)value)->value;
            param.length = (unsigned long)param.text.size();
            bind.buffer_type = MYSQL_TYPE_BLOB;
            bind.buffer = param.text.data();
            bind.buffer_length = param.length;
            bind.length = &param.length;
        } else {
            yyerror("std.db cannot bind %s parameters",
                    typename_of(value));
            return false;
        }
    }
    if (!binds.empty() &&
        mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        mysql_stmt_failure(stmt, "bind");
        return false;
    }
    return true;
}

bool mysql_bind_results(MYSQL_STMT *stmt, MYSQL_RES *metadata,
                        mysql_result_binding &out) {
    const unsigned int count = mysql_num_fields(metadata);
    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    out.binds.assign(count, MYSQL_BIND());
    out.lengths.assign(count, 0);
    out.nulls.assign(count, 0);
    out.dummies.assign(count, 0);
    out.integers.assign(count, 0);
    out.reals.assign(count, 0.0);
    for (unsigned int i = 0; i < count; ++i) {
        MYSQL_BIND &bind = out.binds[i];
        std::memset(&bind, 0, sizeof(bind));
        if (mysql_is_integer_type(fields[i].type)) {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &out.integers[i];
            bind.buffer_length = sizeof(out.integers[i]);
        } else if (mysql_is_float_type(fields[i].type)) {
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = &out.reals[i];
            bind.buffer_length = sizeof(out.reals[i]);
        } else {
            bind.buffer_type =
                mysql_is_binary_field(fields[i]) ? MYSQL_TYPE_BLOB
                                                 : MYSQL_TYPE_STRING;
            bind.buffer = &out.dummies[i];
            bind.buffer_length = 1;
        }
        bind.length = &out.lengths[i];
        bind.is_null = &out.nulls[i];
    }
    if (count == 0)
        return true;
    if (mysql_stmt_bind_result(stmt, out.binds.data()) != 0) {
        mysql_stmt_failure(stmt, "bind result");
        return false;
    }
    return true;
}

struct value_t *mysql_simple_query(MYSQL *conn, const char *sql,
                                   bool rows) {
    const unsigned long length = (unsigned long)std::strlen(sql);
    if (mysql_real_query(conn, sql, length) != 0)
        return mysql_failure(conn, rows ? "query" : "exec");
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        if (mysql_field_count(conn) != 0)
            return mysql_failure(conn, rows ? "query" : "exec");
        if (rows)
            return (struct value_t *)newlistval();
        return db_stats((long long)mysql_affected_rows(conn),
                        (long long)mysql_insert_id(conn));
    }

    if (!rows) {
        mysql_free_result(result);
        return db_stats((long long)mysql_affected_rows(conn),
                        (long long)mysql_insert_id(conn));
    }

    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    const unsigned int columns = mysql_num_fields(result);
    list_value_t *list = (list_value_t *)newlistval();
    MYSQL_ROW row = NULL;
    while ((row = mysql_fetch_row(result)) != NULL) {
        unsigned long *lengths = mysql_fetch_lengths(result);
        struct value_t *item = (struct value_t *)newdictval();
        for (unsigned int i = 0; i < columns; ++i) {
            const char *name = fields[i].name;
            if (!row[i]) {
                db_dict_set(item, name ? name : "column", newnull());
            } else {
                db_dict_set(item, name ? name : "column",
                            mysql_value(fields[i], row[i],
                                        lengths ? lengths[i] : 0));
            }
        }
        list->payload->items.push_back(item);
    }
    mysql_free_result(result);
    return (struct value_t *)list;
}

struct value_t *mysql_backend::simple_exec(const char *sql) {
    return mysql_simple_query(conn, sql, false);
}

struct value_t *mysql_backend::simple_query(const char *sql) {
    return mysql_simple_query(conn, sql, true);
}

struct value_t *mysql_backend::prepared_exec(const char *sql,
                                             struct value_t *params) {
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (!stmt)
        return mysql_failure(conn, "prepare");
    std::vector<MYSQL_BIND> binds;
    std::vector<mysql_param> storage;
    if (mysql_stmt_prepare(stmt, sql,
                           (unsigned long)std::strlen(sql)) != 0) {
        mysql_stmt_failure(stmt, "prepare");
        mysql_stmt_close(stmt);
        return NULL;
    }
    if (!mysql_bind_params(stmt, params, binds, storage)) {
        mysql_stmt_close(stmt);
        return NULL;
    }
    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_failure(stmt, "exec");
        mysql_stmt_close(stmt);
        return NULL;
    }
    long long changes = (long long)mysql_stmt_affected_rows(stmt);
    long long insert_id = (long long)mysql_stmt_insert_id(stmt);
    mysql_stmt_close(stmt);
    return db_stats(changes, insert_id);
}

struct value_t *mysql_backend::prepared_query(const char *sql,
                                              struct value_t *params) {
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (!stmt)
        return mysql_failure(conn, "prepare");
    std::vector<MYSQL_BIND> binds;
    std::vector<mysql_param> storage;
    if (mysql_stmt_prepare(stmt, sql,
                           (unsigned long)std::strlen(sql)) != 0) {
        mysql_stmt_failure(stmt, "prepare");
        mysql_stmt_close(stmt);
        return NULL;
    }
    if (!mysql_bind_params(stmt, params, binds, storage)) {
        mysql_stmt_close(stmt);
        return NULL;
    }

    MYSQL_RES *metadata = mysql_stmt_result_metadata(stmt);
    if (!metadata) {
        if (mysql_stmt_execute(stmt) != 0) {
            mysql_stmt_failure(stmt, "query");
            mysql_stmt_close(stmt);
            return NULL;
        }
        mysql_stmt_close(stmt);
        return (struct value_t *)newlistval();
    }

    MYSQL_FIELD *fields = mysql_fetch_fields(metadata);
    const unsigned int columns = mysql_num_fields(metadata);
    mysql_result_binding result;
    if (!mysql_bind_results(stmt, metadata, result)) {
        mysql_free_result(metadata);
        mysql_stmt_close(stmt);
        return NULL;
    }
    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_failure(stmt, "query");
        mysql_free_result(metadata);
        mysql_stmt_close(stmt);
        return NULL;
    }

    list_value_t *list = (list_value_t *)newlistval();
    for (;;) {
        const int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            mysql_stmt_failure(stmt, "fetch");
            freeval((struct value_t *)list);
            mysql_free_result(metadata);
            mysql_stmt_close(stmt);
            return NULL;
        }
        struct value_t *item = (struct value_t *)newdictval();
        for (unsigned int i = 0; i < columns; ++i) {
            const char *name = fields[i].name;
            struct value_t *value = NULL;
            if (result.nulls[i]) {
                value = newnull();
            } else if (mysql_is_integer_type(fields[i].type)) {
                value = newintval(result.integers[i]);
            } else if (mysql_is_float_type(fields[i].type)) {
                value = newfloatval(result.reals[i]);
            } else {
                std::string data(result.lengths[i], '\0');
                if (!data.empty()) {
                    MYSQL_BIND fetch = result.binds[i];
                    fetch.buffer = &data[0];
                    fetch.buffer_length = result.lengths[i];
                    if (mysql_stmt_fetch_column(stmt, &fetch, i, 0) != 0) {
                        mysql_stmt_failure(stmt, "fetch");
                        freeval(item);
                        freeval((struct value_t *)list);
                        mysql_free_result(metadata);
                        mysql_stmt_close(stmt);
                        return NULL;
                    }
                }
                value = mysql_value(fields[i], data.data(),
                                    result.lengths[i]);
            }
            db_dict_set(item, name ? name : "column", value);
        }
        list->payload->items.push_back(item);
    }
    mysql_free_result(metadata);
    mysql_stmt_close(stmt);
    return (struct value_t *)list;
}

struct value_t *mysql_backend::exec(const char *sql,
                                    struct value_t *params) {
    if (!params || params->type == T_null)
        return simple_exec(sql);
    return prepared_exec(sql, params);
}

struct value_t *mysql_backend::query(const char *sql,
                                     struct value_t *params) {
    if (!params || params->type == T_null)
        return simple_query(sql);
    return prepared_query(sql, params);
}

struct value_t *mysql_backend::transaction(const char *sql) {
    const unsigned long length = (unsigned long)std::strlen(sql);
    if (mysql_real_query(conn, sql, length) != 0)
        return mysql_failure(conn, "transaction");
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result && mysql_field_count(conn) != 0)
        return mysql_failure(conn, "transaction");
    if (result)
        mysql_free_result(result);
    return newboolval(true);
}

bool mysql_option_string(struct value_t *options, const char *name,
                         std::string &out) {
    if (!options || options->type != T_dict)
        return true;
    struct value_t *option = db_dict_get(options, name);
    if (!option || option->type == T_null)
        return true;
    if (option->type != T_string) {
        yyerror("std.db.open %s must be a string", name);
        return false;
    }
    out = ((string_value_t *)option)->value;
    return true;
}

bool mysql_option_timeout(struct value_t *options, long long &timeout_ms) {
    if (!options || options->type != T_dict)
        return true;
    struct value_t *option = db_dict_get(options, "connect_timeout_ms");
    if (!option || option->type == T_null)
        return true;
    if (option->type != T_int ||
        ((int_value_t *)option)->value < 0) {
        yyerror("std.db.open connect_timeout_ms must be a "
                "non-negative integer");
        return false;
    }
    timeout_ms = ((int_value_t *)option)->value;
    return true;
}

} // namespace

std::unique_ptr<db_backend> db_open_mysql(const std::string &uri,
                                          struct value_t *options) {
    mysql_uri parsed;
    std::string error;
    if (!mysql_parse_uri(uri, parsed, error)) {
        yyerror("std.db open: %s", error.c_str());
        return NULL;
    }
    long long timeout_ms = 0;
    std::string unix_socket;
    std::string charset = "utf8mb4";
    if (!mysql_option_timeout(options, timeout_ms) ||
        !mysql_option_string(options, "unix_socket", unix_socket) ||
        !mysql_option_string(options, "charset", charset))
        return NULL;

    mysql_ensure_thread();
    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        yyerror("std.db open: cannot allocate MySQL connection");
        return NULL;
    }
    if (timeout_ms > 0) {
        unsigned int seconds =
            (unsigned int)((timeout_ms + 999) / 1000);
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &seconds);
    }
    if (!charset.empty())
        mysql_options(conn, MYSQL_SET_CHARSET_NAME, charset.c_str());

    const char *host = parsed.host.empty() ? NULL : parsed.host.c_str();
    const char *user = parsed.user.empty() ? NULL : parsed.user.c_str();
    const char *password =
        parsed.password.empty() ? NULL : parsed.password.c_str();
    const char *database =
        parsed.database.empty() ? NULL : parsed.database.c_str();
    const char *socket =
        unix_socket.empty() ? NULL : unix_socket.c_str();
    if (!mysql_real_connect(conn, host, user, password, database,
                            parsed.port, socket, 0)) {
        const char *message = mysql_error(conn);
        std::string text =
            message && *message ? message : "connection failed";
        mysql_close(conn);
        yyerror("std.db open: %s", text.c_str());
        return NULL;
    }
    if (!charset.empty() &&
        mysql_set_character_set(conn, charset.c_str()) != 0) {
        const char *message = mysql_error(conn);
        std::string text =
            message && *message ? message : "cannot set MySQL charset";
        mysql_close(conn);
        yyerror("std.db open: %s", text.c_str());
        return NULL;
    }
    return std::unique_ptr<db_backend>(new mysql_backend(conn));
}
