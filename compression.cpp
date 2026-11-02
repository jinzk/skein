#include "compression.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef CALC_ENABLE_ZLIB
#include <zlib.h>
#endif

#ifdef CALC_ENABLE_BROTLI
#include <brotli/decode.h>
#endif

namespace {

const std::size_t MAX_DECOMPRESSED_SIZE = 256 * 1024 * 1024;

std::string trim_ascii(std::string value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace((unsigned char)value[start]))
        ++start;
    size_t end = value.size();
    while (end > start &&
           std::isspace((unsigned char)value[end - 1]))
        --end;
    return value.substr(start, end - start);
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i)
        value[i] = (char)std::tolower((unsigned char)value[i]);
    return value;
}

#ifdef CALC_ENABLE_ZLIB
bool zlib_decode(const std::string &input, int window_bits,
                 std::string &output, std::string &error) {
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        error = "compression: cannot initialize zlib";
        return false;
    }
    stream.next_in = (Bytef *)input.data();
    stream.avail_in = (uInt)input.size();
    char buffer[64 * 1024];
    int result = Z_OK;
    for (;;) {
        stream.next_out = (Bytef *)buffer;
        stream.avail_out = (uInt)sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            error = stream.msg ? stream.msg : "compression: inflate failed";
            inflateEnd(&stream);
            return false;
        }
        size_t produced = sizeof(buffer) - stream.avail_out;
        if (produced > 0) {
            if (output.size() + produced > MAX_DECOMPRESSED_SIZE) {
                error = "compression: decompressed body is too large";
                inflateEnd(&stream);
                return false;
            }
            output.append(buffer, produced);
        }
        if (result == Z_STREAM_END)
            break;
        if (stream.avail_in == 0 && produced == 0) {
            error = "compression: truncated compressed data";
            inflateEnd(&stream);
            return false;
        }
    }
    inflateEnd(&stream);
    return true;
}
#endif

#ifdef CALC_ENABLE_BROTLI
bool brotli_decode(const std::string &input, std::string &output,
                   std::string &error) {
    std::size_t capacity = input.size() * 4 + 1024;
    if (capacity < 4096)
        capacity = 4096;
    for (;;) {
        if (capacity > MAX_DECOMPRESSED_SIZE) {
            error = "compression: decompressed body is too large";
            return false;
        }
        output.resize(capacity);
        std::size_t decoded = capacity;
        BrotliDecoderResult result = BrotliDecoderDecompress(
            input.size(), (const uint8_t *)input.data(), &decoded,
            (uint8_t *)output.data());
        if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            output.resize(decoded);
            return true;
        }
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            capacity *= 2;
            continue;
        }
        error = "compression: brotli decode failed";
        return false;
    }
}
#endif

bool decode_one(const std::string &encoding, const std::string &input,
                std::string &output, std::string &error) {
    std::string name = lower_ascii(trim_ascii(encoding));
    if (name.empty() || name == "identity") {
        output = input;
        return true;
    }
    if (name == "gzip" || name == "x-gzip") {
#ifdef CALC_ENABLE_ZLIB
        return zlib_decode(input, 15 + 32, output, error);
#else
        error = "compression: gzip support is disabled";
        return false;
#endif
    }
    if (name == "deflate") {
#ifdef CALC_ENABLE_ZLIB
        std::string zlib_error;
        if (zlib_decode(input, 15 + 32, output, zlib_error))
            return true;
        output.clear();
        return zlib_decode(input, -15, output, error);
#else
        error = "compression: deflate support is disabled";
        return false;
#endif
    }
    if (name == "br") {
#ifdef CALC_ENABLE_BROTLI
        return brotli_decode(input, output, error);
#else
        error = "compression: brotli support is disabled";
        return false;
#endif
    }
    error = "compression: unsupported encoding '" + name + "'";
    return false;
}

} // namespace

std::string compression_accept_encoding() {
    std::string encodings;
#ifdef CALC_ENABLE_ZLIB
    encodings = "gzip, deflate";
#endif
#ifdef CALC_ENABLE_BROTLI
    if (!encodings.empty())
        encodings += ", ";
    encodings += "br";
#endif
    return encodings;
}

bool decompress_content_encoding(const std::string &encoding,
                                 const std::string &input,
                                 std::string &output, std::string &error) {
    std::vector<std::string> encodings;
    size_t position = 0;
    while (position <= encoding.size()) {
        size_t comma = encoding.find(',', position);
        std::string item = trim_ascii(
            encoding.substr(position, comma == std::string::npos
                                         ? std::string::npos
                                         : comma - position));
        if (!item.empty())
            encodings.push_back(item);
        if (comma == std::string::npos)
            break;
        position = comma + 1;
    }
    if (encodings.empty()) {
        output = input;
        return true;
    }
    std::string current = input;
    for (size_t i = encodings.size(); i > 0; --i) {
        std::string next;
        if (!decode_one(encodings[i - 1], current, next, error))
            return false;
        current.swap(next);
    }
    output.swap(current);
    return true;
}
