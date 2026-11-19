#include "sha256.h"

#include <cstdint>
#include <cstring>

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
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotate_right(w[i - 15], 7) ^
                      rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotate_right(w[i - 2], 17) ^
                      rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = context.state[0];
    uint32_t b = context.state[1];
    uint32_t c = context.state[2];
    uint32_t d = context.state[3];
    uint32_t e = context.state[4];
    uint32_t f = context.state[5];
    uint32_t g = context.state[6];
    uint32_t h = context.state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                      rotate_right(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                      rotate_right(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
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
    context.bit_length += (uint64_t)length * 8;
    while (length > 0) {
        size_t take = 64 - context.buffer_length;
        if (take > length)
            take = length;
        std::memcpy(context.buffer + context.buffer_length, data, take);
        context.buffer_length += take;
        data += take;
        length -= take;
        if (context.buffer_length == 64) {
            sha256_transform(context, context.buffer);
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
    uint64_t bit_length = context.bit_length;
    for (int j = 7; j >= 0; --j)
        context.buffer[56 + (7 - j)] = (unsigned char)(bit_length >> (j * 8));
    sha256_transform(context, context.buffer);

    for (size_t j = 0; j < 8; ++j) {
        digest[j * 4] = (unsigned char)(context.state[j] >> 24);
        digest[j * 4 + 1] = (unsigned char)(context.state[j] >> 16);
        digest[j * 4 + 2] = (unsigned char)(context.state[j] >> 8);
        digest[j * 4 + 3] = (unsigned char)context.state[j];
    }
}

}  // namespace

std::string skein_sha256_bytes(const std::string &data) {
    sha256_context context;
    sha256_init(context);
    sha256_update(context, reinterpret_cast<const unsigned char *>(data.data()),
                  data.size());
    unsigned char digest[32];
    sha256_final(context, digest);
    return std::string(reinterpret_cast<const char *>(digest), sizeof digest);
}

std::string skein_hex_encode(const std::string &data) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (unsigned char byte : data) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

std::string skein_hmac_sha256_bytes(const std::string &key,
                                    const std::string &data) {
    std::string normalized_key = key;
    if (normalized_key.size() > 64)
        normalized_key = skein_sha256_bytes(normalized_key);
    normalized_key.resize(64, '\0');

    std::string inner(64, '\0');
    std::string outer(64, '\0');
    for (size_t i = 0; i < 64; ++i) {
        inner[i] = (char)((unsigned char)normalized_key[i] ^ 0x36);
        outer[i] = (char)((unsigned char)normalized_key[i] ^ 0x5C);
    }
    std::string inner_hash = skein_sha256_bytes(inner + data);
    return skein_sha256_bytes(outer + inner_hash);
}
