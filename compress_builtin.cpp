#include "compress_builtin.h"

#include "ast.h"
#include "valuetype.h"

#include <cstring>
#include <string>
#include <vector>

#ifdef SKEIN_ENABLE_ZLIB
#include <zlib.h>
#endif

namespace {

#ifdef SKEIN_ENABLE_ZLIB
constexpr size_t kMaxDecompressed = 256 * 1024 * 1024;

bool zlib_encode(const std::string &input, int window_bits,
                 std::string &output, std::string &error) {
    z_stream stream;
    std::memset(&stream, 0, sizeof stream);
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
        error = "cannot initialize zlib";
        return false;
    }
    stream.next_in = (Bytef *)input.data();
    stream.avail_in = (uInt)input.size();
    char buffer[64 * 1024];
    for (;;) {
        stream.next_out = (Bytef *)buffer;
        stream.avail_out = (uInt)sizeof buffer;
        const int result = deflate(&stream, Z_FINISH);
        if (result != Z_OK && result != Z_STREAM_END) {
            error = stream.msg ? stream.msg : "deflate failed";
            deflateEnd(&stream);
            return false;
        }
        const size_t produced = sizeof buffer - stream.avail_out;
        if (produced > 0)
            output.append(buffer, produced);
        if (result == Z_STREAM_END)
            break;
    }
    deflateEnd(&stream);
    return true;
}

bool zlib_decode(const std::string &input, int window_bits,
                 std::string &output, std::string &error) {
    z_stream stream;
    std::memset(&stream, 0, sizeof stream);
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        error = "cannot initialize zlib";
        return false;
    }
    stream.next_in = (Bytef *)input.data();
    stream.avail_in = (uInt)input.size();
    char buffer[64 * 1024];
    for (;;) {
        stream.next_out = (Bytef *)buffer;
        stream.avail_out = (uInt)sizeof buffer;
        const int result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            error = stream.msg ? stream.msg : "inflate failed";
            inflateEnd(&stream);
            return false;
        }
        const size_t produced = sizeof buffer - stream.avail_out;
        if (produced > 0) {
            if (output.size() + produced > kMaxDecompressed) {
                error = "decompressed data is too large";
                inflateEnd(&stream);
                return false;
            }
            output.append(buffer, produced);
        }
        if (result == Z_STREAM_END)
            break;
        if (stream.avail_in == 0 && produced == 0) {
            error = "truncated compressed data";
            inflateEnd(&stream);
            return false;
        }
    }
    inflateEnd(&stream);
    return true;
}
#endif

const char *compress_name(int functype) {
    switch (functype) {
        case B_compress_gzip: return "std.compress.gzip";
        case B_compress_gunzip: return "std.compress.gunzip";
        case B_compress_zlib: return "std.compress.zlib";
        case B_compress_unzlib: return "std.compress.unzlib";
        case B_compress_deflate: return "std.compress.deflate";
        case B_compress_inflate: return "std.compress.inflate";
        default: return "std.compress";
    }
}

}  // namespace

struct value_t *call_compress_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    const char *name = compress_name(functype);
    if (args.size() != 1) {
        yyerror("%s expects 1 argument, got %d", name, (int)args.size());
        return NULL;
    }
    if (args[0]->type != T_bytes) {
        yyerror("%s expects bytes, got %s", name, typename_of(args[0]));
        return NULL;
    }
#ifdef SKEIN_ENABLE_ZLIB
    const std::string &input = ((struct bytes_value_t *)args[0])->value;
    std::string output;
    std::string error;
    bool zlib = false;
    int window_bits = 0;
    switch (functype) {
        case B_compress_gzip:
            window_bits = 15 + 16;
            break;
        case B_compress_gunzip:
            window_bits = 15 + 16;
            zlib = true;
            break;
        case B_compress_zlib:
            window_bits = 15;
            break;
        case B_compress_unzlib:
            window_bits = 15;
            zlib = true;
            break;
        case B_compress_deflate:
            window_bits = -15;
            break;
        case B_compress_inflate:
            window_bits = -15;
            zlib = true;
            break;
        default:
            yyerror("unknown compression function");
            return NULL;
    }
    const bool ok = zlib
                        ? zlib_decode(input, window_bits, output, error)
                        : zlib_encode(input, window_bits, output, error);
    if (!ok) {
        yyerror("%s: %s", name, error.c_str());
        return NULL;
    }
    return newbytesval(std::move(output));
#else
    yyerror("%s: zlib support is disabled in this build", name);
    return NULL;
#endif
}
