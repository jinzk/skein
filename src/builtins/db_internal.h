#ifndef SKEIN_DB_INTERNAL_H
#define SKEIN_DB_INTERNAL_H

#include "valuetype.h"

#include <memory>
#include <mutex>
#include <string>

/*
 * Internal database driver interface.  It is deliberately C++ and not a
 * stable ABI: all drivers are linked into the same builtins target.
 */
struct db_backend {
    virtual ~db_backend() = default;

    virtual struct value_t *exec(const char *sql, struct value_t *params) = 0;
    virtual struct value_t *query(const char *sql, struct value_t *params) = 0;
    virtual struct value_t *transaction(const char *sql) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
};

struct db_connection {
    std::mutex mutex;
    std::unique_ptr<db_backend> backend;
};

/* Shared helpers for driver implementations. */
struct value_t *db_dict_get(struct value_t *dict, const char *key);
void db_dict_set(struct value_t *dict, const char *key, struct value_t *value);
struct value_t *db_stats(long long changes, long long last_insert_rowid);

std::unique_ptr<db_backend> db_open_sqlite(const std::string &path,
                                           struct value_t *options);
std::unique_ptr<db_backend> db_open_postgres(const std::string &conninfo,
                                             struct value_t *options);

#endif
