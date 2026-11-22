#include "db_internal.h"

#include "symbol.h"
#include "valuetype.h"

#include <libpq-fe.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kBoolOid = 16;
constexpr unsigned int kByteaOid = 17;
constexpr unsigned int kInt8Oid = 20;
constexpr unsigned int kInt2Oid = 21;
constexpr unsigned int kInt4Oid = 23;
constexpr unsigned int kFloat4Oid = 700;
constexpr unsigned int kFloat8Oid = 701;

struct postgres_backend final : db_backend {
    PGconn *conn = nullptr;

    explicit postgres_backend(PGconn *connection) : conn(connection) {}

    ~postgres_backend() override { close(); }

    db_dialect dialect() const override { return db_dialect::postgres; }

    bool is_open() const override { return conn != nullptr; }

    void close() override {
        if (conn) {
            PQfinish(conn);
            conn = nullptr;
        }
    }

    struct value_t *exec(const char *sql,
                         struct value_t *params) override;
    struct value_t *query(const char *sql,
                          struct value_t *params) override;
    struct value_t *transaction(const char *sql) override;

  private:
    PGresult *run(const char *sql, struct value_t *params,
                  bool &bind_failed);
};

struct value_t *postgres_failure(PGresult *result, const char *action) {
    const char *message = result ? PQresultErrorMessage(result) : NULL;
    yyerror("std.db %s: %s", action,
            message && *message ? message : "query failed");
    return NULL;
}

struct value_t *postgres_connection_failure(PGconn *conn,
                                            const char *action) {
    const char *message = conn ? PQerrorMessage(conn) : NULL;
    yyerror("std.db %s: %s", action,
            message && *message ? message : "connection failed");
    return NULL;
}

long long postgres_count(const char *text) {
    if (!text || !*text)
        return 0;
    char *end = NULL;
    errno = 0;
    long long value = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text)
        return 0;
    return value;
}

std::string postgres_double_text(double value) {
    if (std::isnan(value))
        return "NaN";
    if (std::isinf(value))
        return value < 0 ? "-Infinity" : "Infinity";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

bool postgres_bind(struct value_t *params, std::vector<const char *> &values,
                   std::vector<int> &lengths, std::vector<int> &formats,
                   std::vector<std::string> &storage) {
    if (!params || params->type == T_null)
        return true;
    if (params->type != T_list) {
        yyerror("std.db parameters must be a list or null");
        return false;
    }

    list_value_t *list = (list_value_t *)params;
    const size_t count = list->payload->items.size();
    values.reserve(count);
    lengths.reserve(count);
    formats.reserve(count);
    storage.reserve(count);
    for (struct value_t *value : list->payload->items) {
        if (!value || value->type == T_null) {
            values.push_back(NULL);
            lengths.push_back(0);
            formats.push_back(0);
        } else if (value->type == T_int) {
            storage.push_back(
                std::to_string(((int_value_t *)value)->value));
            values.push_back(storage.back().c_str());
            lengths.push_back(0);
            formats.push_back(0);
        } else if (value->type == T_bool) {
            storage.push_back(
                ((bool_value_t *)value)->value ? "true" : "false");
            values.push_back(storage.back().c_str());
            lengths.push_back(0);
            formats.push_back(0);
        } else if (value->type == T_float) {
            storage.push_back(postgres_double_text(
                ((float_value_t *)value)->value));
            values.push_back(storage.back().c_str());
            lengths.push_back(0);
            formats.push_back(0);
        } else if (value->type == T_string) {
            values.push_back(((string_value_t *)value)->value);
            lengths.push_back(0);
            formats.push_back(0);
        } else if (value->type == T_bytes) {
            const std::string &bytes = ((bytes_value_t *)value)->value;
            std::string text("\\x");
            static const char hex[] = "0123456789abcdef";
            text.reserve(2 + bytes.size() * 2);
            for (unsigned char byte : bytes) {
                text.push_back(hex[byte >> 4]);
                text.push_back(hex[byte & 0x0f]);
            }
            storage.push_back(std::move(text));
            values.push_back(storage.back().c_str());
            lengths.push_back(0);
            formats.push_back(0);
        } else {
            yyerror("std.db cannot bind %s parameters",
                    typename_of(value));
            return false;
        }
    }
    return true;
}

std::string postgres_bytea(const char *raw, int length) {
    if (length >= 2 && raw[0] == '\\' && raw[1] == 'x') {
        std::string out;
        out.reserve((size_t)(length - 2) / 2);
        int i = 2;
        while (i + 1 < length) {
            int high = 0;
            int low = 0;
            for (int part = 0; part < 2; ++part) {
                const char c = raw[i++];
                int value = -1;
                if (c >= '0' && c <= '9')
                    value = c - '0';
                else if (c >= 'a' && c <= 'f')
                    value = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    value = c - 'A' + 10;
                if (value < 0)
                    return std::string(raw, (size_t)length);
                if (part == 0)
                    high = value;
                else
                    low = value;
            }
            out.push_back((char)((high << 4) | low));
        }
        if (i != length)
            return std::string(raw, (size_t)length);
        return out;
    }
    return std::string(raw, (size_t)length);
}

struct value_t *postgres_value(PGresult *result, int row, int column) {
    if (PQgetisnull(result, row, column))
        return newnull();

    const char *raw = PQgetvalue(result, row, column);
    const int length = PQgetlength(result, row, column);
    switch (PQftype(result, column)) {
        case kBoolOid:
            return newboolval(length > 0 && (raw[0] == 't' || raw[0] == '1'));
        case kInt2Oid:
        case kInt4Oid:
        case kInt8Oid: {
            char *end = NULL;
            errno = 0;
            long long value = std::strtoll(raw, &end, 10);
            if (errno == 0 && end != raw)
                return newintval(value);
            return newstrval(raw, (size_t)length);
        }
        case kFloat4Oid:
        case kFloat8Oid: {
            char *end = NULL;
            errno = 0;
            double value = std::strtod(raw, &end);
            if (errno == 0 && end != raw)
                return newfloatval(value);
            return newstrval(raw, (size_t)length);
        }
        case kByteaOid:
            return newbytesval(postgres_bytea(raw, length));
        default:
            return newstrval(raw, (size_t)length);
    }
}

PGresult *postgres_backend::run(const char *sql, struct value_t *params,
                                bool &bind_failed) {
    bind_failed = false;
    std::vector<const char *> values;
    std::vector<int> lengths;
    std::vector<int> formats;
    std::vector<std::string> storage;
    if (!postgres_bind(params, values, lengths, formats, storage)) {
        bind_failed = true;
        return NULL;
    }
    if (values.empty())
        return PQexec(conn, sql);
    return PQexecParams(
        conn, sql, (int)values.size(), NULL, values.data(),
        lengths.data(), formats.data(), 0);
}

struct value_t *postgres_backend::exec(const char *sql,
                                       struct value_t *params) {
    bool bind_failed = false;
    PGresult *result = run(sql, params, bind_failed);
    if (!result) {
        if (bind_failed)
            return NULL;
        return postgres_connection_failure(conn, "exec");
    }
    const ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        postgres_failure(result, "exec");
        PQclear(result);
        return NULL;
    }
    const long long changes = postgres_count(PQcmdTuples(result));
    PQclear(result);
    return db_stats(changes, 0);
}

struct value_t *postgres_backend::query(const char *sql,
                                        struct value_t *params) {
    bool bind_failed = false;
    PGresult *result = run(sql, params, bind_failed);
    if (!result) {
        if (bind_failed)
            return NULL;
        return postgres_connection_failure(conn, "query");
    }
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        postgres_failure(result, "query");
        PQclear(result);
        return NULL;
    }

    list_value_t *rows = (list_value_t *)newlistval();
    const int count = PQntuples(result);
    const int columns = PQnfields(result);
    for (int row = 0; row < count; ++row) {
        struct value_t *values = (struct value_t *)newdictval();
        for (int column = 0; column < columns; ++column) {
            const char *name = PQfname(result, column);
            db_dict_set(values, name ? name : "column",
                        postgres_value(result, row, column));
        }
        rows->payload->items.push_back(values);
    }
    PQclear(result);
    return (struct value_t *)rows;
}

struct value_t *postgres_backend::transaction(const char *sql) {
    PGresult *result = PQexec(conn, sql);
    if (!result)
        return postgres_connection_failure(conn, "transaction");
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        postgres_failure(result, "transaction");
        PQclear(result);
        return NULL;
    }
    PQclear(result);
    return newboolval(true);
}

} // namespace

std::unique_ptr<db_backend> db_open_postgres(const std::string &conninfo,
                                             struct value_t *options) {
    long long connect_timeout_ms = 0;
    if (options && options->type == T_dict) {
        struct value_t *option =
            db_dict_get(options, "connect_timeout_ms");
        if (option && option->type != T_null) {
            if (option->type != T_int ||
                ((int_value_t *)option)->value < 0) {
                yyerror("std.db.open connect_timeout_ms must be a "
                        "non-negative integer");
                return NULL;
            }
            connect_timeout_ms = ((int_value_t *)option)->value;
        }
    }

    std::string timeout_text;
    std::vector<const char *> keywords;
    std::vector<const char *> values;
    keywords.push_back("dbname");
    values.push_back(conninfo.c_str());
    if (connect_timeout_ms > 0) {
        timeout_text = std::to_string((connect_timeout_ms + 999) / 1000);
        keywords.push_back("connect_timeout");
        values.push_back(timeout_text.c_str());
    }
    keywords.push_back(NULL);
    values.push_back(NULL);

    PGconn *conn = PQconnectdbParams(keywords.data(), values.data(), 1);
    if (!conn) {
        yyerror("std.db open: cannot allocate PostgreSQL connection");
        return NULL;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        const char *message = PQerrorMessage(conn);
        std::string text = message && *message ? message
                                               : "connection failed";
        PQfinish(conn);
        yyerror("std.db open: %s", text.c_str());
        return NULL;
    }

    PGresult *setup = PQexec(conn, "SET bytea_output = 'hex'");
    if (!setup || PQresultStatus(setup) != PGRES_COMMAND_OK) {
        const char *message =
            setup ? PQresultErrorMessage(setup) : PQerrorMessage(conn);
        std::string text =
            message && *message ? message : "cannot configure connection";
        if (setup)
            PQclear(setup);
        PQfinish(conn);
        yyerror("std.db open: %s", text.c_str());
        return NULL;
    }
    PQclear(setup);

    return std::unique_ptr<db_backend>(new postgres_backend(conn));
}
