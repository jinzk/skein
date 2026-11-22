#include "crypto_builtin.h"

#include "sha256.h"
#include "symbol.h"
#include "builtin_ids.h"
#include "valuetype.h"

#include <string>
#include <vector>

namespace {

std::string sha256_bytes(const std::string &data) {
    return skein_sha256_bytes(data);
}

std::string hex_encode(const std::string &data) {
    return skein_hex_encode(data);
}

std::string hmac_sha256_bytes(const std::string &key,
                              const std::string &data) {
    return skein_hmac_sha256_bytes(key, data);
}

bool data_bytes(const struct value_t *value, const char *who, std::string &data) {
    if (value->type == T_string) {
        data = ((struct string_value_t *)value)->value;
        return true;
    }
    if (value->type == T_bytes) {
        data = ((struct bytes_value_t *)value)->value;
        return true;
    }
    yyerror("%s expects string or bytes, got %s", who, typename_of(value));
    return false;
}

}  // namespace

struct value_t *call_crypto_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype == B_crypto_sha256) {
        if (args.size() != 1) {
            yyerror("std.crypto.sha256 expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        std::string data;
        if (!data_bytes(args[0], "std.crypto.sha256", data))
            return NULL;
        return newstrval(hex_encode(sha256_bytes(data)).c_str());
    }
    if (functype == B_crypto_hmac_sha256) {
        if (args.size() != 2) {
            yyerror("std.crypto.hmac_sha256 expects 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        std::string key;
        std::string data;
        if (!data_bytes(args[0], "std.crypto.hmac_sha256", key) ||
            !data_bytes(args[1], "std.crypto.hmac_sha256", data))
            return NULL;
        return newstrval(
            hex_encode(hmac_sha256_bytes(key, data)).c_str());
    }
    if (functype == B_crypto_sha256_bytes) {
        if (args.size() != 1) {
            yyerror("std.crypto.sha256_bytes expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        std::string data;
        if (!data_bytes(args[0], "std.crypto.sha256_bytes", data))
            return NULL;
        return newbytesval(sha256_bytes(data));
    }
    if (functype == B_crypto_hmac_sha256_bytes) {
        if (args.size() != 2) {
            yyerror("std.crypto.hmac_sha256_bytes expects 2 arguments, "
                    "got %d",
                    (int)args.size());
            return NULL;
        }
        std::string key;
        std::string data;
        if (!data_bytes(args[0], "std.crypto.hmac_sha256_bytes", key) ||
            !data_bytes(args[1], "std.crypto.hmac_sha256_bytes", data))
            return NULL;
        return newbytesval(hmac_sha256_bytes(key, data));
    }
    yyerror("unknown crypto function");
    return NULL;
}
