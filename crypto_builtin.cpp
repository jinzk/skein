#include "crypto_builtin.h"

#include "ast.h"
#include "valuetype.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

struct sha256_context {
    uint32_t state[8];
    uint64_t bit_length;
    unsigned char buffer[64];
    size_t buffer_length;
};

void sha256_init(sha256_context &context) {
    context.state[0] = 0x6a09e667;
    context.state[1] = 0xbb67ae85;
    context.state[2] = 0x3c6ef372;
    context.state[3] = 0xa54ff53a;
    context.state[4] = 0x510e527f;
    context.state[5] = 0x9b05688c;
    context.state[6] = 0x1f83d9ab;
    context.state[7] = 0x5be0cd19;
    context.bit_length = 0;
    context.buffer_length = 0;
}

void sha256_transform(sha256_context &context, const unsigned char *block) {
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(words[i - 15], 7) ^
                            rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = rotate_right(words[i - 2], 17) ^
                            rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = context.state[0];
    uint32_t b = context.state[1];
    uint32_t c = context.state[2];
    uint32_t d = context.state[3];
    uint32_t e = context.state[4];
    uint32_t f = context.state[5];
    uint32_t g = context.state[6];
    uint32_t h = context.state[7];

    for (size_t i = 0; i < 64; ++i) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                            rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + choose + kSha256K[i] + words[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                            rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
    context.state[5] += f;
    context.state[6] += g;
    context.state[7] += h;
}

void sha256_update(sha256_context &context, const unsigned char *data,
                   size_t length) {
    for (size_t i = 0; i < length; ++i) {
        context.buffer[context.buffer_length++] = data[i];
        if (context.buffer_length == 64) {
            sha256_transform(context, context.buffer);
            context.bit_length += 512;
            context.buffer_length = 0;
        }
    }
}

void sha256_final(sha256_context &context, unsigned char digest[32]) {
    size_t i = context.buffer_length;
    context.buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64)
            context.buffer[i++] = 0;
        sha256_transform(context, context.buffer);
        i = 0;
    }
    while (i < 56)
        context.buffer[i++] = 0;

    context.bit_length += (uint64_t)context.buffer_length * 8;
    for (int j = 7; j >= 0; --j)
        context.buffer[56 + (7 - j)] = (unsigned char)(context.bit_length >> (j * 8));
    sha256_transform(context, context.buffer);

    for (size_t j = 0; j < 8; ++j) {
        digest[j * 4] = (unsigned char)(context.state[j] >> 24);
        digest[j * 4 + 1] = (unsigned char)(context.state[j] >> 16);
        digest[j * 4 + 2] = (unsigned char)(context.state[j] >> 8);
        digest[j * 4 + 3] = (unsigned char)context.state[j];
    }
}

std::string sha256_bytes(const std::string &data) {
    sha256_context context;
    sha256_init(context);
    sha256_update(context,
                  reinterpret_cast<const unsigned char *>(data.data()),
                  data.size());
    unsigned char digest[32];
    sha256_final(context, digest);
    return std::string(reinterpret_cast<const char *>(digest), sizeof digest);
}

std::string hex_encode(const std::string &data) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (unsigned char byte : data) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

std::string hmac_sha256_bytes(const std::string &key,
                              const std::string &data) {
    std::string normalized_key = key;
    if (normalized_key.size() > 64)
        normalized_key = sha256_bytes(normalized_key);
    normalized_key.resize(64, '\0');

    std::string inner(64, '\0');
    std::string outer(64, '\0');
    for (size_t i = 0; i < 64; ++i) {
        inner[i] = (char)((unsigned char)normalized_key[i] ^ 0x36);
        outer[i] = (char)((unsigned char)normalized_key[i] ^ 0x5C);
    }
    std::string inner_hash = sha256_bytes(inner + data);
    return sha256_bytes(outer + inner_hash);
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
    yyerror("unknown crypto function");
    return NULL;
}
