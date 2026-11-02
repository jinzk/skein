#include "tls.h"

#include "ast.h"
#include "repl.h"
#include "valuetype.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
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

    bool closed = false;
    bool eof = false;

    ~tls_stream() {
        if (ssl)
            SSL_free(ssl);
        if (ctx)
            SSL_CTX_free(ctx);
        freeval(source);
    }
};

std::string describe_tls(const void *data) {
    (void)data;
    return "<tls_stream>";
}

bool flush_write_bio(tls_stream *stream, long long timeout_ms,
                     std::string &error) {
    if (!stream->wbio)
        return true;
    char buffer[16384];
    for (;;) {
        int n = BIO_read(stream->wbio, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        size_t written = 0;
        while (written < (size_t)n) {
            std::size_t count = 0;
            if (!stream->source_ops->write(
                    stream->source_data,
                    std::string(buffer + written, (size_t)n - written),
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

bool pump_read(tls_stream *stream, long long timeout_ms, bool &source_eof,
               std::string &error) {
    std::string data;
    source_eof = false;
    if (!stream->source_ops->read(stream->source_data, 16384, timeout_ms,
                                  data, source_eof, error))
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

bool tls_handshake(tls_stream *stream, long long timeout_ms,
                   std::string &error) {
    for (;;) {
        ERR_clear_error();
        int rc = SSL_do_handshake(stream->ssl);
        if (rc == 1) {
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            return true;
        }
        int ssl_error = SSL_get_error(stream->ssl, rc);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, timeout_ms, source_eof, error))
                return false;
            if (source_eof) {
                error = "tls: handshake ended unexpectedly";
                return false;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            continue;
        }
        ssl_error_string(error, "tls: handshake failed");
        return false;
    }
}

bool tls_read(void *data, std::size_t max_bytes, long long timeout_ms,
              std::string &out, bool &eof, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
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
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, timeout_ms, source_eof, error))
                return false;
            if (source_eof) {
                stream->eof = true;
                eof = true;
                return true;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, timeout_ms, error))
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

bool tls_write(void *data, const std::string &bytes, long long timeout_ms,
               std::size_t &written, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
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
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            continue;
        }
        int ssl_error = SSL_get_error(stream->ssl, n);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            if (!flush_write_bio(stream, timeout_ms, error))
                return false;
            bool source_eof = false;
            if (!pump_read(stream, timeout_ms, source_eof, error))
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

bool tls_flush(void *data, std::string &error) {
    tls_stream *stream = static_cast<tls_stream *>(data);
    if (!stream || stream->closed) {
        error = "stream is closed";
        return false;
    }
    if (!flush_write_bio(stream, 0, error))
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
    was_open = true;
    ERR_clear_error();
    SSL_shutdown(stream->ssl);
    std::string flush_error;
    flush_write_bio(stream, 0, flush_error);
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
    NULL,
    NULL,
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

    if (!tls_handshake(stream.get(), 0, error)) {
        yyerror("%s", error.c_str());
        return NULL;
    }
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

    if (!tls_handshake(stream.get(), 0, error)) {
        yyerror("%s", error.c_str());
        return NULL;
    }
    return make_tls_value(std::move(stream));
}

} // namespace

struct value_t *call_tls_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
    case B_tls_client:
        return tls_client(args);
    case B_tls_server:
        return tls_server(args);
    default:
        yyerror("unknown tls function");
        return NULL;
    }
}
