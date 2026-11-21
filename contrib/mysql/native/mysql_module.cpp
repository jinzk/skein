/*
 * Example native module wrapping the MySQL C client library.
 *
 * This file only uses the public native module ABI from skein_module.h.  The
 * MySQL client is behind opaque userdata handles; script code talks to the
 * Skein wrapper in contrib/mysql/mysql.skein.
 */
#include "skein_module.h"

#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const skein_api *g_api = nullptr;

struct connection {
    std::mutex mutex;
    MYSQL *mysql = nullptr;
};

struct result_set {
    std::mutex mutex;
    MYSQL_RES *mysql_result = nullptr;
    skein_value *connection_ref = nullptr;
};

bool has_value_helpers() {
    return g_api &&
           g_api->size >= offsetof(skein_api, as_userdata) +
                              sizeof(g_api->as_userdata);
}

skein_status fail(skein_context *ctx, const std::string &message) {
    return g_api->error(ctx, "mysql", message.c_str());
}

std::string connection_error(connection *conn, const char *where) {
    std::string message = where;
    message += ": ";
    if (conn && conn->mysql)
        message += mysql_error(conn->mysql);
    else
        message += "connection is closed";
    return message;
}

skein_status fail_connection(skein_context *ctx, connection *conn,
                             const char *where) {
    return fail(ctx, connection_error(conn, where));
}

const char *connection_describe(void *) { return "MysqlConnection"; }

void connection_destroy(void *data) {
    connection *conn = static_cast<connection *>(data);
    if (!conn)
        return;
    if (conn->mysql) {
        mysql_close(conn->mysql);
        conn->mysql = nullptr;
    }
    delete conn;
}

const char *result_describe(void *) { return "MysqlResult"; }

void result_destroy(void *data) {
    result_set *result = static_cast<result_set *>(data);
    if (!result)
        return;
    if (result->mysql_result) {
        mysql_free_result(result->mysql_result);
        result->mysql_result = nullptr;
    }
    if (result->connection_ref) {
        g_api->free_value(nullptr, result->connection_ref);
        result->connection_ref = nullptr;
    }
    delete result;
}

void result_visit_children(void *data, void *ctx, skein_visit_fn visit) {
    result_set *result = static_cast<result_set *>(data);
    if (result && result->connection_ref)
        visit(ctx, result->connection_ref);
}

const skein_native_type connection_type = {
    "MysqlConnection", connection_describe, connection_destroy,
    nullptr, nullptr, nullptr,
};

const skein_native_type result_type = {
    "MysqlResult", result_describe, result_destroy,
    nullptr, nullptr, result_visit_children,
};

const skein_native_type *const types[] = {&connection_type, &result_type};

bool get_connection(const skein_value *value, connection **out) {
    void *data = nullptr;
    if (!g_api->as_userdata(value, &connection_type, &data))
        return false;
    *out = static_cast<connection *>(data);
    return true;
}

bool get_result(const skein_value *value, result_set **out) {
    void *data = nullptr;
    if (!g_api->as_userdata(value, &result_type, &data))
        return false;
    *out = static_cast<result_set *>(data);
    return true;
}

bool get_string(const skein_value *value, const char *name,
                std::string *out) {
    const char *text = g_api->as_string(value);
    if (!text)
        return false;
    *out = text;
    (void)name;
    return true;
}

bool push_list(skein_context *ctx, skein_value *list, skein_value *value) {
    if (!g_api->list_push(ctx, list, value)) {
        g_api->free_value(ctx, value);
        return false;
    }
    return true;
}

bool set_dict(skein_context *ctx, skein_value *dict, const char *key,
              skein_value *value) {
    if (!g_api->dict_set(ctx, dict, key, value)) {
        g_api->free_value(ctx, value);
        return false;
    }
    return true;
}

skein_status library_version(skein_context *ctx,
                             const skein_value *const *args, size_t argc,
                             skein_value **result) {
    (void)args;
    if (argc != 0)
        return fail(ctx, "library_version expects no arguments");
    *result = g_api->new_string(ctx, mysql_get_client_info());
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status connect(skein_context *ctx, const skein_value *const *args,
                     size_t argc, skein_value **result) {
    if (argc < 4 || argc > 5)
        return fail(ctx, "connect expects host, user, password, database "
                         "and an optional port");

    std::string host;
    std::string user;
    std::string password;
    std::string database;
    if (!get_string(args[0], "host", &host) ||
        !get_string(args[1], "user", &user) ||
        !get_string(args[2], "password", &password) ||
        !get_string(args[3], "database", &database))
        return fail(ctx, "connect expects string host, user, password and "
                         "database");

    int64_t port = 3306;
    if (argc == 5 && !g_api->as_int(args[4], &port))
        return fail(ctx, "connect expects an integer port");
    if (port < 1 || port > 65535)
        return fail(ctx, "connect port must be between 1 and 65535");

    MYSQL *raw = mysql_init(nullptr);
    if (!raw)
        return fail(ctx, "mysql_init failed");
    mysql_options(raw, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    unsigned int timeout = 5;
    mysql_options(raw, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!mysql_real_connect(raw, host.c_str(), user.c_str(), password.c_str(),
                            database.c_str(), static_cast<unsigned int>(port),
                            nullptr, 0)) {
        std::string message = "connect: ";
        message += mysql_error(raw);
        mysql_close(raw);
        return fail(ctx, message);
    }

    connection *conn = new connection();
    conn->mysql = raw;
    *result = g_api->new_userdata(ctx, &connection_type, conn);
    if (!*result) {
        connection_destroy(conn);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}

skein_status close_connection(skein_context *ctx,
                              const skein_value *const *args, size_t argc,
                              skein_value **result) {
    if (argc != 1)
        return fail(ctx, "close expects 1 argument");
    connection *conn = nullptr;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "close expects a MysqlConnection");
    {
        std::lock_guard<std::mutex> lock(conn->mutex);
        if (conn->mysql) {
            mysql_close(conn->mysql);
            conn->mysql = nullptr;
        }
    }
    *result = g_api->new_null(ctx);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status ping(skein_context *ctx, const skein_value *const *args,
                  size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "ping expects 1 argument");
    connection *conn = nullptr;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "ping expects a MysqlConnection");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "ping");
    if (mysql_ping(conn->mysql) != 0)
        return fail_connection(ctx, conn, "ping");
    *result = g_api->new_bool(ctx, true);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status select_database(skein_context *ctx,
                             const skein_value *const *args, size_t argc,
                             skein_value **result) {
    if (argc != 2)
        return fail(ctx, "select_db expects 2 arguments");
    connection *conn = nullptr;
    std::string database;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "select_db expects a MysqlConnection");
    if (!get_string(args[1], "database", &database))
        return fail(ctx, "select_db expects a string database");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "select_db");
    if (mysql_select_db(conn->mysql, database.c_str()) != 0)
        return fail_connection(ctx, conn, "select_db");
    *result = g_api->new_null(ctx);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status set_charset(skein_context *ctx, const skein_value *const *args,
                         size_t argc, skein_value **result) {
    if (argc != 2)
        return fail(ctx, "set_charset expects 2 arguments");
    connection *conn = nullptr;
    std::string charset;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "set_charset expects a MysqlConnection");
    if (!get_string(args[1], "charset", &charset))
        return fail(ctx, "set_charset expects a string charset");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "set_charset");
    if (mysql_set_character_set(conn->mysql, charset.c_str()) != 0)
        return fail_connection(ctx, conn, "set_charset");
    *result = g_api->new_null(ctx);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status last_error(skein_context *ctx, const skein_value *const *args,
                        size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "error expects 1 argument");
    connection *conn = nullptr;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "error expects a MysqlConnection");
    std::lock_guard<std::mutex> lock(conn->mutex);
    const char *message = conn->mysql ? mysql_error(conn->mysql) : "";
    *result = g_api->new_string(ctx, message);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status affected_rows(skein_context *ctx,
                           const skein_value *const *args, size_t argc,
                           skein_value **result) {
    if (argc != 1)
        return fail(ctx, "affected_rows expects 1 argument");
    connection *conn = nullptr;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "affected_rows expects a MysqlConnection");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "affected_rows");
    *result = g_api->new_int(
        ctx, static_cast<int64_t>(mysql_affected_rows(conn->mysql)));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status insert_id(skein_context *ctx, const skein_value *const *args,
                       size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "insert_id expects 1 argument");
    connection *conn = nullptr;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "insert_id expects a MysqlConnection");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "insert_id");
    *result = g_api->new_int(ctx, static_cast<int64_t>(
                                      mysql_insert_id(conn->mysql)));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status escape_string(skein_context *ctx,
                           const skein_value *const *args, size_t argc,
                           skein_value **result) {
    if (argc != 2)
        return fail(ctx, "escape_string expects 2 arguments");
    connection *conn = nullptr;
    std::string text;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "escape_string expects a MysqlConnection");
    if (!get_string(args[1], "text", &text))
        return fail(ctx, "escape_string expects a string");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "escape_string");
    std::vector<char> buffer(text.size() * 2 + 1, '\0');
    unsigned long written = mysql_real_escape_string(
        conn->mysql, buffer.data(), text.data(),
        static_cast<unsigned long>(text.size()));
    if (written == static_cast<unsigned long>(-1))
        return fail_connection(ctx, conn, "escape_string");
    *result = g_api->new_string(ctx, buffer.data());
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status execute(skein_context *ctx, const skein_value *const *args,
                     size_t argc, skein_value **result) {
    if (argc != 2)
        return fail(ctx, "execute expects 2 arguments");
    connection *conn = nullptr;
    std::string sql;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "execute expects a MysqlConnection");
    if (!get_string(args[1], "sql", &sql))
        return fail(ctx, "execute expects a string SQL statement");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "execute");
    if (mysql_real_query(conn->mysql, sql.data(),
                         static_cast<unsigned long>(sql.size())) != 0)
        return fail_connection(ctx, conn, "execute");

    MYSQL_RES *unused = mysql_store_result(conn->mysql);
    if (unused) {
        mysql_free_result(unused);
    } else if (mysql_field_count(conn->mysql) != 0) {
        return fail_connection(ctx, conn, "execute");
    }
    *result = g_api->new_null(ctx);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status query(skein_context *ctx, const skein_value *const *args,
                   size_t argc, skein_value **result) {
    if (argc != 2)
        return fail(ctx, "query expects 2 arguments");
    connection *conn = nullptr;
    std::string sql;
    if (!get_connection(args[0], &conn))
        return fail(ctx, "query expects a MysqlConnection");
    if (!get_string(args[1], "sql", &sql))
        return fail(ctx, "query expects a string SQL statement");
    std::lock_guard<std::mutex> lock(conn->mutex);
    if (!conn->mysql)
        return fail_connection(ctx, conn, "query");
    if (mysql_real_query(conn->mysql, sql.data(),
                         static_cast<unsigned long>(sql.size())) != 0)
        return fail_connection(ctx, conn, "query");
    if (mysql_field_count(conn->mysql) == 0) {
        MYSQL_RES *unused = mysql_store_result(conn->mysql);
        if (unused)
            mysql_free_result(unused);
        return fail(ctx, "query: statement returned no result set");
    }
    MYSQL_RES *stored = mysql_store_result(conn->mysql);
    if (!stored)
        return fail_connection(ctx, conn, "query");

    result_set *data = new result_set();
    data->mysql_result = stored;
    data->connection_ref = g_api->dup_value(ctx, args[0]);
    if (!data->connection_ref) {
        data->mysql_result = nullptr;
        result_destroy(data);
        return fail(ctx, "query: cannot retain connection reference");
    }
    *result = g_api->new_userdata(ctx, &result_type, data);
    if (!*result) {
        result_destroy(data);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}

bool is_binary_field(const MYSQL_FIELD *field) {
    return field->charsetnr == 63;
}

skein_status fetch_all(skein_context *ctx, const skein_value *const *args,
                       size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "fetch_all expects 1 argument");
    result_set *data = nullptr;
    if (!get_result(args[0], &data))
        return fail(ctx, "fetch_all expects a MysqlResult");
    std::lock_guard<std::mutex> lock(data->mutex);
    if (!data->mysql_result)
        return fail(ctx, "fetch_all: result is closed");

    MYSQL_RES *stored = data->mysql_result;
    unsigned int field_count = mysql_num_fields(stored);
    MYSQL_FIELD *fields = mysql_fetch_fields(stored);
    skein_value *list = g_api->new_list(ctx);
    if (!list)
        return SKEIN_ERROR;
    mysql_data_seek(stored, 0);
    for (my_ulonglong index = 0; index < mysql_num_rows(stored); ++index) {
        MYSQL_ROW row = mysql_fetch_row(stored);
        unsigned long *lengths = mysql_fetch_lengths(stored);
        if (!row || !lengths) {
            g_api->free_value(ctx, list);
            return fail(ctx, "fetch_all: cannot read row");
        }
        skein_value *dict = g_api->new_dict(ctx);
        if (!dict) {
            g_api->free_value(ctx, list);
            return SKEIN_ERROR;
        }
        for (unsigned int column = 0; column < field_count; ++column) {
            skein_value *value = nullptr;
            if (!row[column]) {
                value = g_api->new_null(ctx);
            } else if (is_binary_field(&fields[column])) {
                value = g_api->new_bytes(ctx, row[column], lengths[column]);
            } else {
                value = g_api->new_string(ctx, row[column]);
            }
            if (!value || !set_dict(ctx, dict, fields[column].name, value)) {
                if (!value)
                    g_api->free_value(ctx, dict);
                g_api->free_value(ctx, list);
                return SKEIN_ERROR;
            }
        }
        if (!push_list(ctx, list, dict)) {
            g_api->free_value(ctx, list);
            return SKEIN_ERROR;
        }
    }
    *result = list;
    return SKEIN_OK;
}

skein_status row_count(skein_context *ctx, const skein_value *const *args,
                       size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "row_count expects 1 argument");
    result_set *data = nullptr;
    if (!get_result(args[0], &data))
        return fail(ctx, "row_count expects a MysqlResult");
    std::lock_guard<std::mutex> lock(data->mutex);
    if (!data->mysql_result)
        return fail(ctx, "row_count: result is closed");
    *result = g_api->new_int(
        ctx, static_cast<int64_t>(mysql_num_rows(data->mysql_result)));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status field_names(skein_context *ctx, const skein_value *const *args,
                         size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "field_names expects 1 argument");
    result_set *data = nullptr;
    if (!get_result(args[0], &data))
        return fail(ctx, "field_names expects a MysqlResult");
    std::lock_guard<std::mutex> lock(data->mutex);
    if (!data->mysql_result)
        return fail(ctx, "field_names: result is closed");
    MYSQL_RES *stored = data->mysql_result;
    skein_value *list = g_api->new_list(ctx);
    if (!list)
        return SKEIN_ERROR;
    unsigned int count = mysql_num_fields(stored);
    MYSQL_FIELD *fields = mysql_fetch_fields(stored);
    for (unsigned int index = 0; index < count; ++index) {
        skein_value *name = g_api->new_string(ctx, fields[index].name);
        if (!name || !push_list(ctx, list, name)) {
            g_api->free_value(ctx, list);
            return SKEIN_ERROR;
        }
    }
    *result = list;
    return SKEIN_OK;
}

skein_status close_result(skein_context *ctx, const skein_value *const *args,
                          size_t argc, skein_value **result) {
    if (argc != 1)
        return fail(ctx, "close_result expects 1 argument");
    result_set *data = nullptr;
    if (!get_result(args[0], &data))
        return fail(ctx, "close_result expects a MysqlResult");
    {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (data->mysql_result) {
            mysql_free_result(data->mysql_result);
            data->mysql_result = nullptr;
        }
    }
    *result = g_api->new_null(ctx);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

const skein_function functions[] = {
    {"library_version", library_version},
    {"connect", connect},
    {"close", close_connection},
    {"ping", ping},
    {"select_db", select_database},
    {"set_charset", set_charset},
    {"error", last_error},
    {"affected_rows", affected_rows},
    {"insert_id", insert_id},
    {"escape_string", escape_string},
    {"execute", execute},
    {"query", query},
    {"fetch_all", fetch_all},
    {"row_count", row_count},
    {"field_names", field_names},
    {"close_result", close_result},
};

const skein_constant constants[] = {
    {"VERSION", SKEIN_CONST_STRING, {.s = "0.1.0"}},
};

skein_status module_init(skein_context *ctx, const skein_api *api) {
    (void)ctx;
    g_api = api;
    if (!has_value_helpers()) {
        if (api && api->error)
            api->error(ctx, "mysql", "native module ABI is too old");
        g_api = nullptr;
        return SKEIN_ERROR;
    }
    mysql_library_init(0, nullptr, nullptr);
    return SKEIN_OK;
}

void module_fini(skein_context *ctx) {
    (void)ctx;
    mysql_library_end();
    g_api = nullptr;
}

} // namespace

const skein_module *skein_module_descriptor(void) {
    static const skein_module descriptor = {
        SKEIN_MODULE_ABI_VERSION,
        "skein.mysql",
        module_init,
        module_fini,
        functions,
        sizeof(functions) / sizeof(functions[0]),
        types,
        sizeof(types) / sizeof(types[0]),
        constants,
        sizeof(constants) / sizeof(constants[0]),
    };
    return &descriptor;
}
