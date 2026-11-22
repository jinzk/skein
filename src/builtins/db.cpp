#include "db.h"

#include "builtin_ids.h"
#include "db_internal.h"
#include "symbol.h"
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct db_pool;

struct db_lease {
    std::shared_ptr<db_connection> connection;
    std::weak_ptr<db_pool> pool;
    bool released = false;

    bool release();
    ~db_lease();
};

const native_type kDbType = {
    "db",
    [](const void *) { return std::string("<db>"); },
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const native_type kDbLeaseType = {
    "db_lease",
    [](const void *) { return std::string("<db_lease>"); },
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
    if (userdata->meta == &kDbLeaseType) {
        std::shared_ptr<db_lease> lease =
            std::static_pointer_cast<db_lease>(userdata->data);
        if (!lease || lease->released || !lease->connection) {
            yyerror("std.db pooled database handle has been released");
            return false;
        }
        out = lease->connection;
    } else if (userdata->meta == &kDbType) {
        out = std::static_pointer_cast<db_connection>(userdata->data);
    } else {
        yyerror("std.db expects a database handle, got %s",
                typename_of(value));
        return false;
    }
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

bool db_is_mysql_uri(const std::string &path) {
    return path.rfind("mysql://", 0) == 0 ||
           path.rfind("mariadb://", 0) == 0;
}

bool db_valid_identifier(const std::string &name) {
    if (name.empty() || name.size() > 64)
        return false;
    unsigned char first = (unsigned char)name[0];
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (size_t i = 1; i < name.size(); ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(std::isalnum(c) || c == '_'))
            return false;
    }
    return true;
}

struct db_migration {
    long long version = 0;
    std::string name;
    std::vector<std::string> statements;
};

bool db_parse_migrations(struct value_t *value,
                         std::vector<db_migration> &out) {
    if (!value || value->type != T_list) {
        yyerror("std.db.migrate migrations must be a list");
        return false;
    }
    list_value_t *list = (list_value_t *)value;
    out.clear();
    for (size_t i = 0; i < list->payload->items.size(); ++i) {
        struct value_t *item = list->payload->items[i];
        if (!item || item->type != T_dict) {
            yyerror("std.db.migrate migration %d must be a dict",
                    (int)i + 1);
            return false;
        }
        struct value_t *version = db_dict_get(item, "version");
        struct value_t *name = db_dict_get(item, "name");
        struct value_t *up = db_dict_get(item, "up");
        if (!version || version->type != T_int ||
            ((int_value_t *)version)->value <= 0) {
            yyerror("std.db.migrate migration %d version must be a "
                    "positive integer", (int)i + 1);
            return false;
        }
        if (!name || name->type != T_string ||
            ((string_value_t *)name)->value[0] == '\0') {
            yyerror("std.db.migrate migration %d name must be a "
                    "non-empty string", (int)i + 1);
            return false;
        }
        db_migration migration;
        migration.version = ((int_value_t *)version)->value;
        migration.name = ((string_value_t *)name)->value;
        if (up && up->type == T_string) {
            migration.statements.push_back(
                ((string_value_t *)up)->value);
        } else if (up && up->type == T_list) {
            list_value_t *statements = (list_value_t *)up;
            for (size_t j = 0; j < statements->payload->items.size();
                 ++j) {
                struct value_t *statement =
                    statements->payload->items[j];
                if (!statement || statement->type != T_string) {
                    yyerror("std.db.migrate migration %d up[%d] must be "
                            "a string", (int)i + 1, (int)j + 1);
                    return false;
                }
                migration.statements.push_back(
                    ((string_value_t *)statement)->value);
            }
        } else {
            yyerror("std.db.migrate migration %d up must be a string "
                    "or list of strings", (int)i + 1);
            return false;
        }
        out.push_back(std::move(migration));
    }

    std::sort(out.begin(), out.end(),
              [](const db_migration &left, const db_migration &right) {
                  return left.version < right.version;
              });
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i - 1].version == out[i].version) {
            yyerror("std.db.migrate duplicate version %lld",
                    out[i].version);
            return false;
        }
    }
    return true;
}

struct value_t *db_version_list(const std::vector<long long> &versions) {
    list_value_t *list = newlistval();
    for (long long version : versions)
        list->payload->items.push_back(newintval(version));
    return (struct value_t *)list;
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

std::unique_ptr<db_backend> db_create_backend(const std::string &path,
                                              struct value_t *options) {
    if (db_is_postgres_uri(path)) {
#ifdef SKEIN_ENABLE_POSTGRES
        return db_open_postgres(path, options);
#else
        yyerror("std.db: postgres driver is not enabled in this build");
        return NULL;
#endif
    }
    if (db_is_mysql_uri(path)) {
#ifdef SKEIN_ENABLE_MYSQL
        return db_open_mysql(path, options);
#else
        yyerror("std.db: mysql driver is not enabled in this build");
        return NULL;
#endif
    }
#ifdef SKEIN_ENABLE_SQLITE
    return db_open_sqlite(path, options);
#else
    yyerror("std.db: sqlite driver is not enabled in this build");
    return NULL;
#endif
}

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
    std::unique_ptr<db_backend> backend =
        db_create_backend(path, options);
    if (!backend)
        return NULL;

    std::shared_ptr<db_connection> connection =
        std::make_shared<db_connection>();
    connection->backend = std::move(backend);
    return newuserdataval(&kDbType, connection);
}

struct db_pool : std::enable_shared_from_this<db_pool> {
    std::mutex mutex;
    std::condition_variable cv;
    std::string uri;
    struct value_t *options = nullptr;
    size_t max = 8;
    size_t open_count = 0;
    bool closed = false;
    std::vector<std::shared_ptr<db_connection>> idle;

    ~db_pool() {
        for (std::shared_ptr<db_connection> &connection : idle) {
            if (connection && connection->backend)
                connection->backend->close();
        }
        if (options)
            freeval(options);
    }
};

void db_pool_close_connections(const std::shared_ptr<db_pool> &pool,
                               bool close_idle) {
    std::lock_guard<std::mutex> lock(pool->mutex);
    pool->closed = true;
    if (close_idle) {
        for (std::shared_ptr<db_connection> &connection : pool->idle) {
            if (connection && connection->backend) {
                std::lock_guard<std::mutex> connection_lock(
                    connection->mutex);
                connection->backend->close();
                connection->backend.reset();
            }
            if (pool->open_count > 0)
                --pool->open_count;
        }
        pool->idle.clear();
    }
    pool->cv.notify_all();
}

void db_pool_return_connection(const std::shared_ptr<db_pool> &pool,
                               std::shared_ptr<db_connection> connection) {
    if (!connection)
        return;
    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->closed || !connection->backend ||
        !connection->backend->is_open()) {
        if (connection->backend) {
            std::lock_guard<std::mutex> connection_lock(
                connection->mutex);
            connection->backend->close();
            connection->backend.reset();
        }
        if (pool->open_count > 0)
            --pool->open_count;
    } else {
        pool->idle.push_back(connection);
    }
    pool->cv.notify_one();
}

bool db_lease::release() {
    if (released)
        return false;
    released = true;
    std::shared_ptr<db_pool> owner = pool.lock();
    if (owner) {
        db_pool_return_connection(owner, connection);
    } else if (connection && connection->backend) {
        std::lock_guard<std::mutex> lock(connection->mutex);
        connection->backend->close();
        connection->backend.reset();
    }
    connection.reset();
    return true;
}

db_lease::~db_lease() {
    if (!released)
        release();
}

struct value_t *db_pool_acquire(const std::shared_ptr<db_pool> &pool) {
    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->cv.wait(lock, [&pool] {
        return pool->closed || !pool->idle.empty() ||
               pool->open_count < pool->max;
    });
    if (pool->closed) {
        yyerror("std.db pool is closed");
        return NULL;
    }

    std::shared_ptr<db_connection> connection;
    if (!pool->idle.empty()) {
        connection = pool->idle.back();
        pool->idle.pop_back();
    } else {
        ++pool->open_count;
        lock.unlock();
        std::unique_ptr<db_backend> backend =
            db_create_backend(pool->uri, pool->options);
        if (!backend) {
            lock.lock();
            --pool->open_count;
            pool->cv.notify_one();
            return NULL;
        }
        connection = std::make_shared<db_connection>();
        connection->backend = std::move(backend);
        lock.lock();
        if (pool->closed) {
            if (connection->backend)
                connection->backend->close();
            connection->backend.reset();
            --pool->open_count;
            pool->cv.notify_all();
            yyerror("std.db pool is closed");
            return NULL;
        }
    }

    std::shared_ptr<db_lease> lease = std::make_shared<db_lease>();
    lease->connection = connection;
    lease->pool = pool;
    return newuserdataval(&kDbLeaseType, lease);
}

struct value_t *db_pool_release(const std::shared_ptr<db_pool> &pool,
                                struct value_t *value) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &kDbLeaseType) {
        yyerror("std.db pool release expects a pooled database handle");
        return NULL;
    }
    std::shared_ptr<db_lease> lease =
        std::static_pointer_cast<db_lease>(
            ((userdata_value_t *)value)->data);
    if (!lease) {
        yyerror("std.db pool release expects a pooled database handle");
        return NULL;
    }
    std::shared_ptr<db_pool> owner = lease->pool.lock();
    if (owner.get() != pool.get()) {
        yyerror("std.db pool release received a handle from another pool");
        return NULL;
    }
    if (!lease->release()) {
        yyerror("std.db pool release received an already released handle");
        return NULL;
    }
    return newboolval(true);
}

struct value_t *db_pool_stats(const std::shared_ptr<db_pool> &pool) {
    std::lock_guard<std::mutex> lock(pool->mutex);
    struct value_t *result = (struct value_t *)newdictval();
    db_dict_set(result, "open", newintval((long long)pool->open_count));
    db_dict_set(result, "idle", newintval((long long)pool->idle.size()));
    db_dict_set(result, "leased",
                newintval((long long)(pool->open_count -
                                      pool->idle.size())));
    db_dict_set(result, "max", newintval((long long)pool->max));
    db_dict_set(result, "closed", newboolval(pool->closed));
    return result;
}

struct value_t *db_pool_close(const std::shared_ptr<db_pool> &pool) {
    db_pool_close_connections(pool, true);
    return newboolval(true);
}

struct value_t *db_pool_get_field(void *data, const char *name) {
    db_pool *raw = static_cast<db_pool *>(data);
    if (!raw || !name)
        return NULL;
    std::shared_ptr<db_pool> pool = raw->shared_from_this();
    if (std::strcmp(name, "acquire") == 0) {
        return newnativeval([pool](const std::vector<struct value_t *> &args) {
            if (!args.empty()) {
                yyerror("std.db pool acquire expects no arguments");
                return (struct value_t *)NULL;
            }
            return db_pool_acquire(pool);
        });
    }
    if (std::strcmp(name, "release") == 0) {
        return newnativeval([pool](const std::vector<struct value_t *> &args) {
            if (args.size() != 1) {
                yyerror("std.db pool release expects one database handle");
                return (struct value_t *)NULL;
            }
            return db_pool_release(pool, args[0]);
        });
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval([pool](const std::vector<struct value_t *> &args) {
            if (!args.empty()) {
                yyerror("std.db pool stats expects no arguments");
                return (struct value_t *)NULL;
            }
            return db_pool_stats(pool);
        });
    }
    if (std::strcmp(name, "close") == 0) {
        return newnativeval([pool](const std::vector<struct value_t *> &args) {
            if (!args.empty()) {
                yyerror("std.db pool close expects no arguments");
                return (struct value_t *)NULL;
            }
            return db_pool_close(pool);
        });
    }
    return NULL;
}

const native_type kDbPoolType = {
    "db_pool",
    [](const void *) { return std::string("<db_pool>"); },
    nullptr,
    db_pool_get_field,
    nullptr,
    nullptr,
};

struct value_t *db_pool_create(const std::vector<struct value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 ||
        !args[0] || args[0]->type != T_string) {
        yyerror("std.db.pool expects a path or URI and optional options");
        return NULL;
    }
    struct value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type == T_null)
        options = NULL;
    if (options && options->type != T_dict) {
        yyerror("std.db.pool options must be a dict");
        return NULL;
    }
    long long max = 8;
    if (options) {
        struct value_t *option = db_dict_get(options, "max");
        if (option && option->type != T_null) {
            if (option->type != T_int ||
                ((int_value_t *)option)->value <= 0) {
                yyerror("std.db.pool max must be a positive integer");
                return NULL;
            }
            max = ((int_value_t *)option)->value;
        }
    }

    std::shared_ptr<db_pool> pool = std::make_shared<db_pool>();
    pool->uri = ((string_value_t *)args[0])->value;
    pool->options = options ? dupval(options) : NULL;
    pool->max = (size_t)max;
    return newuserdataval(&kDbPoolType, pool);
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

struct value_t *db_migrate(const std::vector<struct value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.db.migrate expects database, migrations and optional "
                "options");
        return NULL;
    }
    std::shared_ptr<db_connection> connection;
    if (!db_from_value(args[0], connection))
        return NULL;

    std::vector<db_migration> migrations;
    if (!db_parse_migrations(args[1], migrations))
        return NULL;

    std::string table = "skein_schema_migrations";
    if (args.size() == 3 && args[2] && args[2]->type != T_null) {
        if (args[2]->type != T_dict) {
            yyerror("std.db.migrate options must be a dict");
            return NULL;
        }
        struct value_t *option = db_dict_get(args[2], "table");
        if (option && option->type != T_null) {
            if (option->type != T_string) {
                yyerror("std.db.migrate table must be a string");
                return NULL;
            }
            table = ((string_value_t *)option)->value;
        }
        if (!db_valid_identifier(table)) {
            yyerror("std.db.migrate table must be a valid identifier");
            return NULL;
        }
    }

    std::lock_guard<std::mutex> lock(connection->mutex);
    if (!connection->backend || !connection->backend->is_open()) {
        yyerror("std.db database handle is closed");
        return NULL;
    }
    db_backend *backend = connection->backend.get();

    const std::string create =
        "CREATE TABLE IF NOT EXISTS " + table +
        " (version BIGINT PRIMARY KEY, name TEXT NOT NULL, "
        "applied_at BIGINT NOT NULL)";
    if (!backend->exec(create.c_str(), NULL))
        return NULL;

    const std::string select =
        "SELECT version, name FROM " + table + " ORDER BY version";
    struct value_t *rows = backend->query(select.c_str(), NULL);
    if (!rows)
        return NULL;
    std::map<long long, std::string> applied;
    if (rows->type == T_list) {
        list_value_t *list = (list_value_t *)rows;
        for (struct value_t *row : list->payload->items) {
            struct value_t *version = db_dict_get(row, "version");
            struct value_t *name = db_dict_get(row, "name");
            if (!version || version->type != T_int ||
                !name || name->type != T_string) {
                yyerror("std.db.migrate cannot read migration state");
                freeval(rows);
                return NULL;
            }
            applied[((int_value_t *)version)->value] =
                ((string_value_t *)name)->value;
        }
    }
    freeval(rows);

    const long long now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::vector<long long> newly_applied;
    for (const db_migration &migration : migrations) {
        auto existing = applied.find(migration.version);
        if (existing != applied.end()) {
            if (existing->second != migration.name) {
                yyerror("std.db.migrate version %lld was already applied "
                        "with a different name", migration.version);
                return NULL;
            }
            continue;
        }

        if (!backend->transaction("BEGIN"))
            return NULL;
        bool failed = false;
        for (const std::string &statement : migration.statements) {
            if (statement.empty())
                continue;
            if (!backend->exec(statement.c_str(), NULL)) {
                failed = true;
                break;
            }
        }
        if (!failed) {
            list_value_t *params = newlistval();
            params->payload->items.push_back(newintval(migration.version));
            params->payload->items.push_back(
                newstrval(migration.name.c_str()));
            params->payload->items.push_back(newintval(now_ms));
            std::string insert =
                "INSERT INTO " + table +
                "(version, name, applied_at) VALUES ";
            if (backend->dialect() == db_dialect::postgres)
                insert += "($1, $2, $3)";
            else
                insert += "(?, ?, ?)";
            if (!backend->exec(insert.c_str(), (struct value_t *)params))
                failed = true;
            freeval((struct value_t *)params);
        }
        if (failed) {
            backend->transaction("ROLLBACK");
            return NULL;
        }
        if (!backend->transaction("COMMIT")) {
            backend->transaction("ROLLBACK");
            return NULL;
        }
        newly_applied.push_back(migration.version);
        applied[migration.version] = migration.name;
    }

    struct value_t *result = (struct value_t *)newdictval();
    db_dict_set(result, "applied", db_version_list(newly_applied));
    long long current = 0;
    if (!applied.empty())
        current = applied.rbegin()->first;
    db_dict_set(result, "current", newintval(current));
    return result;
}

struct value_t *db_close(const std::vector<struct value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.db.close expects one database handle");
        return NULL;
    }
    if (!args[0] || args[0]->type != T_userdata) {
        yyerror("std.db.close expects a database handle");
        return NULL;
    }
    userdata_value_t *userdata = (userdata_value_t *)args[0];
    if (userdata->meta == &kDbLeaseType) {
        yyerror("std.db.close cannot close a pooled database handle; "
                "release it to the pool instead");
        return NULL;
    }
    if (userdata->meta != &kDbType) {
        yyerror("std.db.close expects a database handle");
        return NULL;
    }
    std::shared_ptr<db_connection> connection =
        std::static_pointer_cast<db_connection>(userdata->data);
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
        case B_db_migrate:
            return db_migrate(args);
        case B_db_pool:
            return db_pool_create(args);
        default:
            yyerror("unknown db function");
            return NULL;
    }
}
