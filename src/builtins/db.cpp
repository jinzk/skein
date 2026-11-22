#include "db.h"

#include "builtin_ids.h"
#include "db_internal.h"
#include "symbol.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

const native_type kDbType = {
    "db",
    [](const void *) { return std::string("<db>"); },
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

bool db_from_value(struct value_t *value,
                   std::shared_ptr<db_connection> &out) {
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
    if (!out || !out->backend || !out->backend->is_open()) {
        yyerror("std.db database handle is closed");
        return false;
    }
    return true;
}

bool db_is_postgres_uri(const std::string &path) {
    return path.rfind("postgres://", 0) == 0 ||
           path.rfind("postgresql://", 0) == 0;
}

} // namespace

struct value_t *db_dict_get(struct value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return NULL;
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        struct value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            std::strcmp(((string_value_t *)item_key)->value, key) == 0) {
            return object->payload->items[i].second;
        }
    }
    return NULL;
}

void db_dict_set(struct value_t *dict, const char *key,
                 struct value_t *value) {
    if (!dict || dict->type != T_dict) {
        freeval(value);
        return;
    }
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        struct value_t *item_key = object->payload->items[i].first;
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

struct value_t *db_stats(long long changes, long long last_insert_rowid) {
    struct value_t *result = (struct value_t *)newdictval();
    db_dict_set(result, "changes", newintval(changes));
    db_dict_set(result, "last_insert_rowid",
                newintval(last_insert_rowid));
    return result;
}

namespace {

struct value_t *db_open(const std::vector<struct value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 ||
        !args[0] || args[0]->type != T_string) {
        yyerror("std.db.open expects a path and optional options");
        return NULL;
    }
    struct value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type == T_null)
        options = NULL;
    if (options && options->type != T_dict) {
        yyerror("std.db.open options must be a dict");
        return NULL;
    }

    const std::string path = ((string_value_t *)args[0])->value;
    std::unique_ptr<db_backend> backend;
    if (db_is_postgres_uri(path)) {
#ifdef SKEIN_ENABLE_POSTGRES
        backend = db_open_postgres(path, options);
#else
        yyerror("std.db: postgres driver is not enabled in this build");
        return NULL;
#endif
    } else {
#ifdef SKEIN_ENABLE_SQLITE
        backend = db_open_sqlite(path, options);
#else
        yyerror("std.db: sqlite driver is not enabled in this build");
        return NULL;
#endif
    }
    if (!backend)
        return NULL;

    std::shared_ptr<db_connection> connection =
        std::make_shared<db_connection>();
    connection->backend = std::move(backend);
    return newuserdataval(&kDbType, connection);
}

struct value_t *db_exec(const std::vector<struct value_t *> &args) {
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
    if (!connection->backend || !connection->backend->is_open()) {
        yyerror("std.db database handle is closed");
        return NULL;
    }
    return connection->backend->exec(
        sql, args.size() == 3 ? args[2] : NULL);
}

struct value_t *db_query(const std::vector<struct value_t *> &args) {
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
    if (!connection->backend || !connection->backend->is_open()) {
        yyerror("std.db database handle is closed");
        return NULL;
    }
    return connection->backend->query(
        sql, args.size() == 3 ? args[2] : NULL);
}

struct value_t *db_transaction(const std::vector<struct value_t *> &args,
                               const char *sql) {
    if (args.size() != 1) {
        yyerror("std.db transaction helper expects one database handle");
        return NULL;
    }
    std::shared_ptr<db_connection> connection;
    if (!db_from_value(args[0], connection))
        return NULL;
    std::lock_guard<std::mutex> lock(connection->mutex);
    if (!connection->backend || !connection->backend->is_open()) {
        yyerror("std.db database handle is closed");
        return NULL;
    }
    return connection->backend->transaction(sql);
}

struct value_t *db_close(const std::vector<struct value_t *> &args) {
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
    if (connection->backend) {
        connection->backend->close();
        connection->backend.reset();
    }
    return newboolval(true);
}

} // namespace

struct value_t *call_db_builtin(
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
