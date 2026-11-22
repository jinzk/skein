#include "db_internal.h"

#include "symbol.h"
#include "valuetype.h"

#include "sqlite3.h"

#include <memory>
#include <string>
#include <vector>

namespace {

struct sqlite_backend final : db_backend {
    sqlite3 *handle = nullptr;

    ~sqlite_backend() override { close(); }

    db_dialect dialect() const override { return db_dialect::sqlite; }

    bool is_open() const override { return handle != nullptr; }

    void close() override {
        if (handle) {
            sqlite3_close_v2(handle);
            handle = nullptr;
        }
    }

    struct value_t *exec(const char *sql,
                         struct value_t *params) override;
    struct value_t *query(const char *sql,
                          struct value_t *params) override;
    struct value_t *transaction(const char *sql) override;
};

struct value_t *sqlite_failure(sqlite3 *handle, const char *action) {
    yyerror("std.db %s: %s", action,
            handle ? sqlite3_errmsg(handle) : "database is closed");
    return NULL;
}

bool sqlite_bind(sqlite3_stmt *stmt, struct value_t *params) {
    int index = 1;
    if (!params || params->type == T_null)
        return true;
    if (params->type != T_list) {
        yyerror("std.db parameters must be a list or null");
        return false;
    }
    list_value_t *list = (list_value_t *)params;
    for (struct value_t *value : list->payload->items) {
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

struct value_t *sqlite_row(sqlite3_stmt *stmt) {
    struct value_t *row = (struct value_t *)newdictval();
    int columns = sqlite3_column_count(stmt);
    for (int i = 0; i < columns; ++i) {
        const char *raw_name = sqlite3_column_name(stmt, i);
        const char *name = raw_name ? raw_name : "column";
        struct value_t *value = NULL;
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
                value = newstrval((const char *)text, (size_t)length);
                break;
            }
            case SQLITE_BLOB: {
                const void *data = sqlite3_column_blob(stmt, i);
                int length = sqlite3_column_bytes(stmt, i);
                value = newbytesval(
                    length > 0
                        ? std::string((const char *)data, (size_t)length)
                        : std::string());
                break;
            }
            default:
                value = newnull();
                break;
        }
        db_dict_set(row, name, value);
    }
    return row;
}

struct value_t *sqlite_backend::exec(const char *sql,
                                     struct value_t *params) {
    if (!params || params->type == T_null) {
        char *message = NULL;
        int rc = sqlite3_exec(handle, sql, NULL, NULL, &message);
        if (rc != SQLITE_OK) {
            yyerror("std.db exec: %s",
                    message ? message : sqlite3_errstr(rc));
            sqlite3_free(message);
            return NULL;
        }
        return db_stats(sqlite3_changes(handle),
                        sqlite3_last_insert_rowid(handle));
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return sqlite_failure(handle, "prepare");
    if (!sqlite_bind(stmt, params)) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    do {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            yyerror("std.db exec: %s", sqlite3_errmsg(handle));
            sqlite3_finalize(stmt);
            return NULL;
        }
    } while (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return db_stats(sqlite3_changes(handle),
                    sqlite3_last_insert_rowid(handle));
}

struct value_t *sqlite_backend::query(const char *sql,
                                      struct value_t *params) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return sqlite_failure(handle, "prepare");
    if (!sqlite_bind(stmt, params)) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    list_value_t *rows = (list_value_t *)newlistval();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        rows->payload->items.push_back(sqlite_row(stmt));
    if (rc != SQLITE_DONE) {
        yyerror("std.db query: %s", sqlite3_errmsg(handle));
        sqlite3_finalize(stmt);
        freeval((struct value_t *)rows);
        return NULL;
    }
    sqlite3_finalize(stmt);
    return (struct value_t *)rows;
}

struct value_t *sqlite_backend::transaction(const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        yyerror("std.db: %s", message ? message : sqlite3_errstr(rc));
        sqlite3_free(message);
        return NULL;
    }
    return newboolval(true);
}

} // namespace

std::unique_ptr<db_backend> db_open_sqlite(const std::string &path,
                                           struct value_t *options) {
    long long busy_timeout_ms = 5000;
    bool foreign_keys = true;
    if (options && options->type == T_dict) {
        struct value_t *option = db_dict_get(options, "busy_timeout_ms");
        if (option && option->type != T_null) {
            if (option->type != T_int ||
                ((int_value_t *)option)->value < 0) {
                yyerror("std.db.open busy_timeout_ms must be a "
                        "non-negative integer");
                return NULL;
            }
            busy_timeout_ms = ((int_value_t *)option)->value;
        }
        option = db_dict_get(options, "foreign_keys");
        if (option && option->type != T_null) {
            if (option->type != T_bool) {
                yyerror("std.db.open foreign_keys must be a bool");
                return NULL;
            }
            foreign_keys = ((bool_value_t *)option)->value;
        }
    }

    std::unique_ptr<sqlite_backend> backend =
        std::make_unique<sqlite_backend>();
    int rc = sqlite3_open_v2(
        path.c_str(), &backend->handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        sqlite3 *handle = backend->handle;
        std::string message;
        if (handle)
            message = sqlite3_errmsg(handle);
        else
            message = "cannot open database";
        if (handle)
            sqlite3_close_v2(handle);
        backend->handle = nullptr;
        yyerror("std.db open: %s", message.c_str());
        return NULL;
    }
    sqlite3_busy_timeout(backend->handle, (int)busy_timeout_ms);
    if (foreign_keys)
        sqlite3_exec(backend->handle, "PRAGMA foreign_keys = ON",
                     NULL, NULL, NULL);
    return backend;
}
