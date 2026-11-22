#include "db.h"

#include "builtin_ids.h"
#include "symbol.h"
#include "valuetype.h"

#include "sqlite3.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

value_t *dict_get_value(value_t *dict, const char *key) {
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

void dict_set_value(value_t *dict, const char *key, value_t *value) {
    if (!dict || dict->type != T_dict) {
        freeval(value);
        return;
    }
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            std::strcmp(((string_value_t *)item_key)->value, key) == 0) {
            freeval(object->payload->items[i].second);
            object->payload->items[i].second = value;
            return;
        }
    }
    object->payload->items.push_back(
        std::make_pair(newstrval(key), value));
}

struct db_connection {
    sqlite3 *handle = nullptr;
    std::mutex mutex;

    ~db_connection() {
        if (handle)
            sqlite3_close_v2(handle);
    }
};

std::string db_describe(const void *) {
    return "<db>";
}

const native_type kDbType = {
    "db",
    db_describe,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

bool db_from_value(value_t *value, std::shared_ptr<db_connection> &out) {
    if (!value || value->type != T_userdata) {
        yyerror("std.db expects a database handle, got %s",
                value ? typename_of(value) : "null");
        return false;
    }
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (userdata->meta != &kDbType) {
        yyerror("std.db expects a database handle, got %s",
                typename_of(value));
        return false;
    }
    out = std::static_pointer_cast<db_connection>(userdata->data);
    if (!out || !out->handle) {
        yyerror("std.db database handle is closed");
        return false;
    }
    return true;
}

value_t *db_failure(sqlite3 *handle, const char *action) {
    yyerror("std.db %s: %s", action,
            handle ? sqlite3_errmsg(handle) : "database is closed");
    return NULL;
}

bool sqlite_bind(sqlite3_stmt *stmt, value_t *params) {
    int index = 1;
    if (!params || params->type == T_null)
        return true;
    if (params->type != T_list) {
        yyerror("std.db parameters must be a list or null");
        return false;
    }
    list_value_t *list = (list_value_t *)params;
    for (value_t *value : list->payload->items) {
        int rc = SQLITE_OK;
        if (!value || value->type == T_null) {
            rc = sqlite3_bind_null(stmt, index);
        } else if (value->type == T_int) {
            rc = sqlite3_bind_int64(
                stmt, index, ((int_value_t *)value)->value);
        } else if (value->type == T_bool) {
            rc = sqlite3_bind_int(
                stmt, index, ((bool_value_t *)value)->value ? 1 : 0);
        } else if (value->type == T_float) {
            rc = sqlite3_bind_double(
                stmt, index, ((float_value_t *)value)->value);
        } else if (value->type == T_string) {
            const char *text = ((string_value_t *)value)->value;
            rc = sqlite3_bind_text(stmt, index, text, -1,
                                   SQLITE_TRANSIENT);
        } else if (value->type == T_bytes) {
            const std::string &data =
                ((bytes_value_t *)value)->value;
            rc = sqlite3_bind_blob(stmt, index, data.data(),
                                   (int)data.size(), SQLITE_TRANSIENT);
        } else {
            yyerror("std.db cannot bind %s parameters",
                    typename_of(value));
            return false;
        }
        if (rc != SQLITE_OK) {
            yyerror("std.db bind: %s", sqlite3_errstr(rc));
            return false;
        }
        ++index;
    }
    return true;
}

value_t *sqlite_row(sqlite3_stmt *stmt) {
    value_t *row = (value_t *)newdictval();
    int columns = sqlite3_column_count(stmt);
    for (int i = 0; i < columns; ++i) {
        const char *raw_name = sqlite3_column_name(stmt, i);
        const char *name = raw_name ? raw_name : "column";
        value_t *value = NULL;
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_INTEGER:
                value = newintval(sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                value = newfloatval(sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT: {
                const unsigned char *text =
                    sqlite3_column_text(stmt, i);
                int length = sqlite3_column_bytes(stmt, i);
                value = newstrval(std::string(
                    (const char *)text, (size_t)length).c_str());
                break;
            }
            case SQLITE_BLOB: {
                const void *data = sqlite3_column_blob(stmt, i);
                int length = sqlite3_column_bytes(stmt, i);
                value = newbytesval(std::string(
                    (const char *)data, (size_t)length));
                break;
            }
            default:
                value = newnull();
                break;
        }
        dict_set_value(row, name, value);
    }
    return row;
}

value_t *db_open(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 ||
        !args[0] || args[0]->type != T_string) {
        yyerror("std.db.open expects a path and optional options");
        return NULL;
    }
    const char *path = ((string_value_t *)args[0])->value;
    long long busy_timeout_ms = 5000;
    bool foreign_keys = true;
    if (args.size() == 2 && args[1] && args[1]->type != T_null) {
        if (args[1]->type != T_dict) {
            yyerror("std.db.open options must be a dict");
            return NULL;
        }
        value_t *option = dict_get_value(args[1], "busy_timeout_ms");
        if (option && option->type != T_null) {
            if (option->type != T_int ||
                ((int_value_t *)option)->value < 0) {
                yyerror("std.db.open busy_timeout_ms must be a "
                        "non-negative integer");
                return NULL;
            }
            busy_timeout_ms = ((int_value_t *)option)->value;
        }
        option = dict_get_value(args[1], "foreign_keys");
        if (option && option->type != T_null) {
            if (option->type != T_bool) {
                yyerror("std.db.open foreign_keys must be a bool");
                return NULL;
            }
            foreign_keys = ((bool_value_t *)option)->value;
        }
    }

    std::shared_ptr<db_connection> connection =
        std::make_shared<db_connection>();
    int rc = sqlite3_open_v2(
        path, &connection->handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        sqlite3 *handle = connection->handle;
        connection->handle = NULL;
        value_t *failure = db_failure(handle, "open");
        if (handle)
            sqlite3_close_v2(handle);
        return failure;
    }
    sqlite3_busy_timeout(connection->handle, (int)busy_timeout_ms);
    if (foreign_keys)
        sqlite3_exec(connection->handle, "PRAGMA foreign_keys = ON",
                     NULL, NULL, NULL);
    return newuserdataval(&kDbType, connection);
}

value_t *db_exec(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        !args[1] || args[1]->type != T_string) {
        yyerror("std.db.exec expects database, SQL and optional parameters");
        return NULL;
    }
    std::shared_ptr<db_connection> connection;
    if (!db_from_value(args[0], connection))
        return NULL;
    const char *sql = ((string_value_t *)args[1])->value;
    std::lock_guard<std::mutex> lock(connection->mutex);

    if (args.size() == 2 || !args[2] || args[2]->type == T_null) {
        char *message = NULL;
        int rc = sqlite3_exec(connection->handle, sql, NULL, NULL,
                              &message);
        if (rc != SQLITE_OK) {
            yyerror("std.db exec: %s", message ? message : sqlite3_errstr(rc));
            sqlite3_free(message);
            return NULL;
        }
        value_t *result = (value_t *)newdictval();
        dict_set_value(result, "changes",
                       newintval(sqlite3_changes(connection->handle)));
        dict_set_value(result, "last_insert_rowid",
                       newintval(sqlite3_last_insert_rowid(
                           connection->handle)));
        return result;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(connection->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return db_failure(connection->handle, "prepare");
    if (!sqlite_bind(stmt, args[2])) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    do {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            yyerror("std.db exec: %s", sqlite3_errmsg(connection->handle));
            sqlite3_finalize(stmt);
            return NULL;
        }
    } while (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);
    value_t *result = (value_t *)newdictval();
    dict_set_value(result, "changes",
                   newintval(sqlite3_changes(connection->handle)));
    dict_set_value(result, "last_insert_rowid",
                   newintval(sqlite3_last_insert_rowid(
                       connection->handle)));
    return result;
}

value_t *db_query(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        !args[1] || args[1]->type != T_string) {
        yyerror("std.db.query expects database, SQL and optional parameters");
        return NULL;
    }
    std::shared_ptr<db_connection> connection;
    if (!db_from_value(args[0], connection))
        return NULL;
    const char *sql = ((string_value_t *)args[1])->value;
    std::lock_guard<std::mutex> lock(connection->mutex);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(connection->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return db_failure(connection->handle, "prepare");
    if (!sqlite_bind(stmt, args.size() == 3 ? args[2] : NULL)) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    list_value_t *rows = (list_value_t *)newlistval();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        rows->payload->items.push_back(sqlite_row(stmt));
    if (rc != SQLITE_DONE) {
        yyerror("std.db query: %s", sqlite3_errmsg(connection->handle));
        sqlite3_finalize(stmt);
        freeval((value_t *)rows);
        return NULL;
    }
    sqlite3_finalize(stmt);
    return (value_t *)rows;
}

value_t *db_transaction(const std::vector<value_t *> &args,
                        const char *sql) {
    if (args.size() != 1) {
        yyerror("std.db transaction helper expects one database handle");
        return NULL;
    }
    std::shared_ptr<db_connection> connection;
    if (!db_from_value(args[0], connection))
        return NULL;
    std::lock_guard<std::mutex> lock(connection->mutex);
    char *message = NULL;
    int rc = sqlite3_exec(connection->handle, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        yyerror("std.db: %s", message ? message : sqlite3_errstr(rc));
        sqlite3_free(message);
        return NULL;
    }
    return newboolval(true);
}

value_t *db_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.db.close expects one database handle");
        return NULL;
    }
    if (!args[0] || args[0]->type != T_userdata ||
        ((userdata_value_t *)args[0])->meta != &kDbType) {
        yyerror("std.db.close expects a database handle");
        return NULL;
    }
    std::shared_ptr<db_connection> connection =
        std::static_pointer_cast<db_connection>(
            ((userdata_value_t *)args[0])->data);
    if (!connection)
        return newboolval(false);
    std::lock_guard<std::mutex> lock(connection->mutex);
    if (connection->handle) {
        sqlite3_close_v2(connection->handle);
        connection->handle = nullptr;
    }
    return newboolval(true);
}

} // namespace

value_t *call_db_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_db_open:
            return db_open(args);
        case B_db_exec:
            return db_exec(args);
        case B_db_query:
            return db_query(args);
        case B_db_begin:
            return db_transaction(args, "BEGIN");
        case B_db_commit:
            return db_transaction(args, "COMMIT");
        case B_db_rollback:
            return db_transaction(args, "ROLLBACK");
        case B_db_close:
            return db_close(args);
        default:
            yyerror("unknown db function");
            return NULL;
    }
}
