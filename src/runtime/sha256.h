#ifndef SKEIN_SHA256_H
#define SKEIN_SHA256_H

#include <string>

/* Raw 32-byte SHA-256 digest. */
std::string skein_sha256_bytes(const std::string &data);

/* Lowercase hexadecimal encoding of raw bytes. */
std::string skein_hex_encode(const std::string &data);

/* HMAC-SHA256, returned as raw 32 bytes. */
std::string skein_hmac_sha256_bytes(const std::string &key,
                                    const std::string &data);

#endif
