#include "tls.h"

#include "ast.h"
#include "fiber.h"
#include "builtins_io.h"
#include "repl.h"
#include "task.h"
#include "valuetype.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct stream_view {
    const stream_ops *ops = NULL;
    void *data = NULL;
    std::shared_ptr<void> owner;
};

bool get_stream(value_t *value, stream_view &view, const char *who) {
    if (!value || value->type != T_userdata) {
        yyerror("%s expects a stream, got %s", who, typename_of(value));
        return false;
    }
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (!userdata->meta || !userdata->meta->stream || !userdata->data) {
        yyerror("%s expects a stream, got %s", who, typename_of(value));
        return false;
    }
    view.ops = userdata->meta->stream;
    view.data = userdata->data.get();
    view.owner = userdata->data;
    return true;
}

value_t *dict_get(value_t *dict, const char *key) {
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

void ssl_error_string(std::string &error, const char *prefix) {
    unsigned long code = ERR_get_error();
    char buffer[256];
    if (code != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
        error = prefix;
        error += ": ";
        error += buffer;
    } else {
        error = prefix;
    }
}

struct tls_stream {
    value_t *source = NULL;
    const stream_ops *source_ops = NULL;
    void *source_data = NULL;
    std::shared_ptr<void> source_owner;

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    BIO *rbio = NULL;
    BIO *wbio = NULL;
    std::vector<std::string> *server_alpn = NULL;
    std::string selected_alpn;

    bool closed = false;
    bool eof = false;
    bool async_pending = false;

    ~tls_stream() {
        if (ssl)
            SSL_free(ssl);
        if (ctx)
            SSL_CTX_free(ctx);
        delete server_alpn;
        freeval(source);
    }
};

std::string describe_tls(const void *data) {
    (void)data;
    return "<tls_stream>";
}

int select_alpn(SSL *, const unsigned char **out, unsigned char *outlen,
                const unsigned char *in, unsigned int inlen, void *arg) {
    std::vector<std::string> *preferences =
        static_cast<std::vector<std::string> *>(arg);
    if (!preferences)
        return SSL_TLSEXT_ERR_NOACK;
    for (size_t p = 0; p < preferences->size(); ++p) {
        const std::string &wanted = (*preferences)[p];
        unsigned int offset = 0;
        while (offset < inlen) {
            unsigned int length = in[offset++];
            if (offset + length > inlen)
                break;
            if (length == wanted.size() &&
                std::memcmp(in + offset, wanted.data(), length) == 0) {
                *out = in + offset;
                *outlen = (unsigned char)length;
                return SSL_TLSEXT_ERR_OK;
            }
            offset += length;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

void capture_alpn(tls_stream *stream) {
    const unsigned char *protocol = NULL;
    unsigned int length = 0;
    SSL_get0_alpn_selected(stream->ssl, &protocol, &length);
    if (protocol && length > 0)
        stream->selected_alpn.assign((const char *)protocol, length);
}

using tls_read_fn = std::function<bool(long long, std::string &, bool &,
                                       std::string &)>;
using tls_write_fn = std::function<bool(const std::string &, long long,
                                        std::size_t &, std::string &)>;

bool flush_write_bio(tls_stream *stream, const tls_write_fn &write,
                     long long timeout_ms, std::string &error) {
    if (!stream->wbio)
        return true;
    char buffer[16384];
    for (;;) {
        int n = BIO_read(stream->wbio, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        std::size_t written = 0;
        while (written < (std::size_t)n) {
            std::size_t count = 0;
            if (!write(std::string(buffer + written,
                                   (std::size_t)n - written),
                       timeout_ms, count, error))
                return false;
            if (count == 0) {
                error = "tls: underlying write returned 0 bytes";
                return false;
            }
            written += count;
        }
    }
    return true;
}

bool pump_read(tls_stream *stream, const tls_read_fn &read,
               long long timeout_ms, bool &source_eof,
               std::string &error) {
    std::string data;
    source_eof = false;
    if (!read(timeout_ms, data, source_eof, error))
        return false;
    if (!data.empty()) {
        int written = BIO_write(stream->rbio, data.data(), (int)data.size());
        if (written != (int)data.size()) {
            error = "tls: cannot feed encrypted input";
            return false;
        }
    }
    return true;
}

bool tls_handshake_impl(tls_stream *stream, const tls_read_fn &read,
                        const tls_write_fn &write, long long timeout_ms,
                        std::string &error) {
    for (;;) {
        ERR_clear_error();
        int rc = SSL_do_handshake(stream->ssl);
        if (rc == 1) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            return true;
        }
        int ssl_error = SSL_get_error(stream->ssl, rc);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, read, timeout_ms, source_eof, error))
                return false;
            if (source_eof) {
                error = "tls: handshake ended unexpectedly";
                return false;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            continue;
        }
        ssl_error_string(error, "tls: handshake failed");
        return false;
    }
}

bool tls_read_impl(tls_stream *stream, const tls_read_fn &read,
                   const tls_write_fn &write, std::size_t max_bytes,
                   long long timeout_ms, std::string &out, bool &eof,
                   std::string &error) {
    out.clear();
    eof = false;
    if (!stream || stream->closed) {
        error = "stream is closed";
        return false;
    }
    if (stream->eof) {
        eof = true;
        return true;
    }

    int request = max_bytes > 16384 ? 16384 : (int)max_bytes;
    out.resize((std::size_t)request);
    for (;;) {
        ERR_clear_error();
        int n = SSL_read(stream->ssl, &out[0], request);
        if (n > 0) {
            out.resize((std::size_t)n);
            return true;
        }
        int ssl_error = SSL_get_error(stream->ssl, n);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, read, timeout_ms, source_eof, error))
                return false;
            if (source_eof) {
                stream->eof = true;
                eof = true;
                return true;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            continue;
        }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            stream->eof = true;
            eof = true;
            return true;
        }
        if (ssl_error == SSL_ERROR_SYSCALL && n == 0) {
            stream->eof = true;
            eof = true;
            return true;
        }
        ssl_error_string(error, "tls: read failed");
        return false;
    }
}

bool tls_write_impl(tls_stream *stream, const tls_read_fn &read,
                    const tls_write_fn &write, const std::string &bytes,
                    long long timeout_ms, std::size_t &written,
                    std::string &error) {
    written = 0;
    if (!stream || stream->closed) {
        error = "stream is closed";
        return false;
    }
    while (written < bytes.size()) {
        int request = (int)std::min<std::size_t>(bytes.size() - written,
                                                 16384);
        ERR_clear_error();
        int n = SSL_write(stream->ssl, bytes.data() + written, request);
        if (n > 0) {
            written += (std::size_t)n;
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            continue;
        }
        int ssl_error = SSL_get_error(stream->ssl, n);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!flush_write_bio(stream, write, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, read, timeout_ms, source_eof, error))
                return false;
            if (source_eof) {
                error = "tls: write failed: unexpected EOF";
                return false;
            }
            continue;
        }
        ssl_error_string(error, "tls: write failed");
        return false;
    }
    return true;
}

tls_read_fn sync_reader(tls_stream *stream) {
    return [stream](long long timeout, std::string &out, bool &eof,
                    std::string &error) {
        return stream->source_ops->read(stream->source_data, 16384, timeout,
                                        out, eof, error);
    };
}

tls_write_fn sync_writer(tls_stream *stream) {
    return [stream](const std::string &data, long long timeout,
                    std::size_t &written, std::string &error) {
        return stream->source_ops->write(stream->source_data, data, timeout,
                                         written, error);
    };
}

value_t *await_task_value(value_t *task_value) {
    if (!task_value)
        return NULL;
    task_ptr task;
    if (!get_task_value(task_value, task)) {
        freeval(task_value);
        return NULL;
    }
    freeval(task_value);
    return fiber_await_task(task);
}

value_t *call_io(int functype, std::vector<value_t *> args) {
    value_t *result = call_io_builtin(functype, args);
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    return result;
}

tls_read_fn async_reader(value_t *source) {
    return [source](long long timeout, std::string &out, bool &eof,
                    std::string &error) {
        std::vector<value_t *> args;
        args.push_back(dupval(source));
        args.push_back(newintval(16384));
        args.push_back(newintval(timeout));
        value_t *task_value = call_io(B_io_read_async, args);
        if (!task_value) {
            if (error.empty())
                error = "tls: underlying async read failed";
            return false;
        }
        value_t *result = await_task_value(task_value);
        if (!result) {
            if (error.empty())
                error = "tls: underlying async read failed";
            return false;
        }
        if (result->type == T_null) {
            freeval(result);
            out.clear();
            eof = true;
            return true;
        }
        if (result->type != T_bytes) {
            freeval(result);
            error = "tls: underlying async read returned non-bytes";
            return false;
        }
        out = ((bytes_value_t *)result)->value;
        freeval(result);
        return true;
    };
}

tls_write_fn async_writer(value_t *source) {
    return [source](const std::string &data, long long timeout,
                    std::size_t &written, std::string &error) {
        written = 0;
        while (written < data.size()) {
            std::vector<value_t *> args;
            args.push_back(dupval(source));
            args.push_back(newbytesval(data.substr(written)));
            args.push_back(newintval(timeout));
            value_t *task_value = call_io(B_io_write_async, args);
            if (!task_value) {
                if (error.empty())
                    error = "tls: underlying async write failed";
                return false;
            }
            value_t *result = await_task_value(task_value);
            if (!result) {
                if (error.empty())
                    error = "tls: underlying async write failed";
                return false;
            }
            if (result->type != T_int) {
                freeval(result);
                error = "tls: underlying async write returned non-int";
                return false;
            }
            long long count = ((int_value_t *)result)->value;
            freeval(result);
            if (count <= 0) {
                error = "tls: underlying async write returned 0 bytes";
                return false;
            }
            written += (std::size_t)count;
        }
        return true;
    };
}

bool tls_read(void *data, std::size_t max_bytes, long long timeout_ms,
              std::string &out, bool &eof, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (stream && stream->async_pending) {
        error = "stream has a pending async operation";
        return false;
    }
    return tls_read_impl(stream, sync_reader(stream), sync_writer(stream),
                         max_bytes, timeout_ms, out, eof, error);
}

bool tls_write(void *data, const std::string &bytes, long long timeout_ms,
               std::size_t &written, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (stream && stream->async_pending) {
        error = "stream has a pending async operation";
        return false;
    }
    return tls_write_impl(stream, sync_reader(stream), sync_writer(stream),
                          bytes, timeout_ms, written, error);
}

value_t *tls_read_async(void *data, const std::shared_ptr<void> &owner,
                        std::size_t max_bytes, long long timeout_ms,
                        std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        error = "stream is closed";
        return NULL;
    }
    if (stream->async_pending) {
        error = "stream has a pending async operation";
        return NULL;
    }
    stream->async_pending = true;
    task_ptr task = fiber_start_async([owner, max_bytes, timeout_ms]() {
        tls_stream *stream = static_cast<tls_stream *>(owner.get());
        std::string out;
        bool eof = false;
        std::string read_error;
        bool ok = tls_read_impl(stream, async_reader(stream->source),
                                async_writer(stream->source), max_bytes,
                                timeout_ms, out, eof, read_error);
        stream->async_pending = false;
        if (!ok) {
            yyerror("%s", read_error.empty() ? "tls: async read failed"
                                             : read_error.c_str());
            return (value_t *)NULL;
        }
        if (eof)
            return newnull();
        return newbytesval(std::move(out));
    });
    if (!task) {
        stream->async_pending = false;
        error = "tls: cannot start async read";
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *tls_write_async(void *data, const std::shared_ptr<void> &owner,
                         const std::string &bytes, long long timeout_ms,
                         std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        error = "stream is closed";
        return NULL;
    }
    if (stream->async_pending) {
        error = "stream has a pending async operation";
        return NULL;
    }
    stream->async_pending = true;
    task_ptr task = fiber_start_async([owner, bytes, timeout_ms]() {
        tls_stream *stream = static_cast<tls_stream *>(owner.get());
        std::size_t written = 0;
        std::string write_error;
        bool ok = tls_write_impl(stream, async_reader(stream->source),
                                 async_writer(stream->source), bytes,
                                 timeout_ms, written, write_error);
        stream->async_pending = false;
        if (!ok) {
            yyerror("%s", write_error.empty() ? "tls: async write failed"
                                              : write_error.c_str());
            return (value_t *)NULL;
        }
        return newintval((long long)written);
    });
    if (!task) {
        stream->async_pending = false;
        error = "tls: cannot start async write";
        return NULL;
    }
    return new_task_value(std::move(task));
}

bool tls_flush(void *data, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        error = "stream is closed";
        return false;
    }
    if (stream->async_pending) {
        error = "stream has a pending async operation";
        return false;
    }
    if (!flush_write_bio(stream, sync_writer(stream), 0, error))
        return false;
    if (stream->source_ops->flush &&
        !stream->source_ops->flush(stream->source_data, error))
        return false;
    return true;
}

bool tls_close(void *data, bool &was_open, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        was_open = false;
        return true;
    }
    if (stream->async_pending) {
        error = "stream has a pending async operation";
        return false;
    }
    was_open = true;
    ERR_clear_error();
    SSL_shutdown(stream->ssl);
    std::string flush_error;
    flush_write_bio(stream, sync_writer(stream), 0, flush_error);
    if (stream->source_ops->close) {
        bool source_was_open = false;
        if (!stream->source_ops->close(stream->source_data, source_was_open,
                                       error))
            return false;
    }
    stream->closed = true;
    stream->eof = true;
    return true;
}

bool tls_eof(void *data, bool &eof, std::string &error) {
    (void)error;
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        eof = true;
        return true;
    }
    eof = stream->eof;
    return true;
}

bool tls_seekable(void *data) {
    (void)data;
    return false;
}

const stream_ops tls_stream_ops = {
    tls_read,
    tls_write,
    tls_flush,
    tls_close,
    tls_eof,
    tls_seekable,
    NULL,
    NULL,
    tls_read_async,
    tls_write_async,
};

const native_type tls_native_type = {
    "tls_stream",
    describe_tls,
    &tls_stream_ops,
};

bool option_bool(value_t *options, const char *key, bool fallback) {
    value_t *value = dict_get(options, key);
    if (!value)
        return fallback;
    if (value->type == T_bool)
        return ((bool_value_t *)value)->value;
    return fallback;
}

std::string option_string(value_t *options, const char *key) {
    value_t *value = dict_get(options, key);
    if (!value || value->type != T_string)
        return std::string();
    return ((string_value_t *)value)->value;
}

bool build_alpn_wire(value_t *options, std::string &wire) {
    value_t *value = dict_get(options, "alpn");
    if (!value || value->type == T_null)
        return true;
    if (value->type != T_list) {
        yyerror("std.tls: alpn must be a list of strings");
        return false;
    }
    list_value_t *list = (list_value_t *)value;
    for (size_t i = 0; i < list->payload->items.size(); ++i) {
        value_t *item = list->payload->items[i];
        if (item->type != T_string) {
            yyerror("std.tls: alpn entries must be strings");
            return false;
        }
        std::string protocol = ((string_value_t *)item)->value;
        if (protocol.empty() || protocol.size() > 255) {
            yyerror("std.tls: invalid ALPN protocol");
            return false;
        }
        wire.push_back((char)protocol.size());
        wire += protocol;
    }
    return true;
}

bool prepare_ssl(tls_stream *stream, SSL_CTX *ctx, SSL *ssl,
                 std::string &error) {
    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        if (rbio)
            BIO_free(rbio);
        if (wbio)
            BIO_free(wbio);
        ssl_error_string(error, "tls: cannot create BIO");
        return false;
    }
    BIO_set_mem_eof_return(rbio, -1);
    SSL_set_bio(ssl, rbio, wbio);
    stream->rbio = rbio;
    stream->wbio = wbio;
    stream->ctx = ctx;
    stream->ssl = ssl;
    return true;
}

value_t *make_tls_value(std::shared_ptr<tls_stream> stream) {
    return newuserdataval(&tls_native_type, stream);
}

value_t *tls_client(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[1]->type != T_string) {
        yyerror("std.tls.client expects stream, server name, optional options");
        return NULL;
    }
    stream_view source;
    if (!get_stream(args[0], source, "std.tls.client"))
        return NULL;
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.tls.client options must be a dict");
        return NULL;
    }

    std::shared_ptr<tls_stream> stream = std::make_shared<tls_stream>();
    stream->source = dupval(args[0]);
    stream->source_ops = source.ops;
    stream->source_data = source.data;
    stream->source_owner = source.owner;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        std::string error;
        ssl_error_string(error, "tls: cannot create client context");
        yyerror("%s", error.c_str());
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    bool verify = option_bool(options, "verify", true);
    if (verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        std::string ca_file = option_string(options, "ca_file");
        if (!ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), NULL) != 1) {
                std::string error;
                ssl_error_string(error, "tls: cannot load CA file");
                yyerror("%s", error.c_str());
                SSL_CTX_free(ctx);
                return NULL;
            }
        } else {
            SSL_CTX_set_default_verify_paths(ctx);
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    std::string alpn;
    if (!build_alpn_wire(options, alpn)) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (!alpn.empty() &&
        SSL_CTX_set_alpn_protos(ctx, (const unsigned char *)alpn.data(),
                                (unsigned int)alpn.size()) != 0) {
        std::string error;
        ssl_error_string(error, "tls: cannot set ALPN");
        yyerror("%s", error.c_str());
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        std::string error;
        ssl_error_string(error, "tls: cannot create SSL");
        yyerror("%s", error.c_str());
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_set_connect_state(ssl);
    const char *server_name = ((string_value_t *)args[1])->value;
    SSL_set_tlsext_host_name(ssl, server_name);
    if (verify)
        SSL_set1_host(ssl, server_name);
    std::string error;
    if (!prepare_ssl(stream.get(), ctx, ssl, error)) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        yyerror("%s", error.c_str());
        return NULL;
    }

    if (!tls_handshake_impl(stream.get(), sync_reader(stream.get()),
                            sync_writer(stream.get()), 0, error)) {
        yyerror("%s", error.c_str());
        return NULL;
    }
    capture_alpn(stream.get());
    return make_tls_value(std::move(stream));
}

value_t *tls_server(const std::vector<value_t *> &args) {
    if (args.size() < 3 || args.size() > 4 || args[1]->type != T_string ||
        args[2]->type != T_string) {
        yyerror("std.tls.server expects stream, cert file, key file, "
                "optional options");
        return NULL;
    }
    stream_view source;
    if (!get_stream(args[0], source, "std.tls.server"))
        return NULL;
    value_t *options = args.size() == 4 ? args[3] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.tls.server options must be a dict");
        return NULL;
    }

    std::shared_ptr<tls_stream> stream = std::make_shared<tls_stream>();
    stream->source = dupval(args[0]);
    stream->source_ops = source.ops;
    stream->source_data = source.data;
    stream->source_owner = source.owner;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        std::string error;
        ssl_error_string(error, "tls: cannot create server context");
        yyerror("%s", error.c_str());
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    const char *cert_file = ((string_value_t *)args[1])->value;
    const char *key_file = ((string_value_t *)args[2])->value;
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file,
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        std::string error;
        ssl_error_string(error, "tls: cannot load certificate or key");
        yyerror("%s", error.c_str());
        SSL_CTX_free(ctx);
        return NULL;
    }

    value_t *alpn_value = dict_get(options, "alpn");
    if (alpn_value && alpn_value->type != T_null) {
        if (alpn_value->type != T_list) {
            yyerror("std.tls.server: alpn must be a list of strings");
            SSL_CTX_free(ctx);
            return NULL;
        }
        std::vector<std::string> *preferences =
            new std::vector<std::string>();
        list_value_t *list = (list_value_t *)alpn_value;
        for (size_t i = 0; i < list->payload->items.size(); ++i) {
            value_t *item = list->payload->items[i];
            if (item->type != T_string || item->type == T_null) {
                delete preferences;
                yyerror("std.tls.server: alpn entries must be strings");
                SSL_CTX_free(ctx);
                return NULL;
            }
            preferences->push_back(((string_value_t *)item)->value);
        }
        stream->server_alpn = preferences;
        SSL_CTX_set_alpn_select_cb(ctx, select_alpn, preferences);
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        std::string error;
        ssl_error_string(error, "tls: cannot create SSL");
        yyerror("%s", error.c_str());
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_set_accept_state(ssl);
    std::string error;
    if (!prepare_ssl(stream.get(), ctx, ssl, error)) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        yyerror("%s", error.c_str());
        return NULL;
    }

    if (!tls_handshake_impl(stream.get(), sync_reader(stream.get()),
                            sync_writer(stream.get()), 0, error)) {
        yyerror("%s", error.c_str());
        return NULL;
    }
    capture_alpn(stream.get());
    return make_tls_value(std::move(stream));
}

value_t *tls_alpn(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.tls.alpn expects 1 argument");
        return NULL;
    }
    value_t *value = args[0];
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &tls_native_type) {
        yyerror("std.tls.alpn expects a TLS stream");
        return NULL;
    }
    tls_stream *stream = static_cast<tls_stream *>(
        ((userdata_value_t *)value)->data.get());
    if (!stream) {
        yyerror("std.tls.alpn expects a TLS stream");
        return NULL;
    }
    return newstrval(stream->selected_alpn.c_str());
}

} // namespace

struct value_t *call_tls_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
    case B_tls_client:
        return tls_client(args);
    case B_tls_server:
        return tls_server(args);
    case B_tls_alpn:
        return tls_alpn(args);
    default:
        yyerror("unknown tls function");
        return NULL;
    }
}
