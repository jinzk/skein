#include "password.h"

#include "builtin_ids.h"
#include "symbol.h"
#include "valuetype.h"
#include "value_runtime.h"

#include "argon2.h"

#include <cstdint>
#include <cstring>
#include <random>
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

bool password_bytes(const value_t *value, const char *who,
                    std::string &out) {
    if (value && value->type == T_string) {
        out = ((const struct string_value_t *)value)->value;
        return true;
    }
    if (value && value->type == T_bytes) {
        out = ((const struct bytes_value_t *)value)->value;
        return true;
    }
    yyerror("%s expects a string or bytes password, got %s", who,
            value ? typename_of(value) : "null");
    return false;
}

bool option_uint(value_t *options, const char *name, uint32_t &out) {
    value_t *value = options ? dict_get_value(options, name) : NULL;
    if (!value || value->type == T_null)
        return true;
    if (value->type != T_int || ((int_value_t *)value)->value < 0 ||
        ((int_value_t *)value)->value > UINT32_MAX) {
        yyerror("std.password option %s must be a non-negative integer",
                name);
        return false;
    }
    out = (uint32_t)((int_value_t *)value)->value;
    return true;
}

std::string random_salt(std::size_t size) {
    std::random_device device;
    std::string salt(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        salt[i] = (char)(device() & 0xff);
    return salt;
}

} // namespace

value_t *call_password_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype == B_password_hash) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.password.hash expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        std::string password;
        if (!password_bytes(args[0], "std.password.hash", password))
            return NULL;
        if (args.size() == 2 && args[1] && args[1]->type != T_dict &&
            args[1]->type != T_null) {
            yyerror("std.password.hash options must be a dict");
            return NULL;
        }
        value_t *options =
            args.size() == 2 && args[1] && args[1]->type == T_dict
                ? args[1]
                : NULL;

        uint32_t memory_kib = 64 * 1024;
        uint32_t iterations = 3;
        uint32_t parallelism = 1;
        uint32_t hash_len = 32;
        uint32_t salt_len = 16;
        if (!option_uint(options, "memory_kib", memory_kib) ||
            !option_uint(options, "iterations", iterations) ||
            !option_uint(options, "parallelism", parallelism) ||
            !option_uint(options, "hash_len", hash_len) ||
            !option_uint(options, "salt_len", salt_len))
            return NULL;
        if (iterations < 1 || parallelism < 1 || hash_len < 16 ||
            salt_len < 8 || memory_kib < 8 * parallelism) {
            yyerror("std.password.hash parameters are outside safe ranges");
            return NULL;
        }

        std::string salt = random_salt(salt_len);
        std::size_t encoded_len = argon2_encodedlen(
            iterations, memory_kib, parallelism, salt_len, hash_len,
            Argon2_id);
        std::string encoded(encoded_len, '\0');
        int rc = argon2id_hash_encoded(
            iterations, memory_kib, parallelism, password.data(),
            password.size(), salt.data(), salt.size(), hash_len,
            &encoded[0], encoded.size());
        if (rc != ARGON2_OK) {
            yyerror("std.password.hash failed: %s",
                    argon2_error_message(rc));
            return NULL;
        }
        encoded.resize(std::char_traits<char>::length(encoded.c_str()));
        return newstrval(encoded.c_str());
    }

    if (functype == B_password_verify) {
        if (args.size() != 2) {
            yyerror("std.password.verify expects 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        std::string password;
        if (!password_bytes(args[0], "std.password.verify", password))
            return NULL;
        if (!args[1] || args[1]->type != T_string) {
            yyerror("std.password.verify encoded hash must be a string");
            return NULL;
        }
        const std::string &encoded =
            ((string_value_t *)args[1])->value;
        int rc = argon2id_verify(encoded.c_str(), password.data(),
                                 password.size());
        return newboolval(rc == ARGON2_OK);
    }

    yyerror("unknown password function");
    return NULL;
}
