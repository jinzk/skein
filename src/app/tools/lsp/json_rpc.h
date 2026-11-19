#ifndef SKEIN_LSP_JSON_RPC_H
#define SKEIN_LSP_JSON_RPC_H

#include <cstdio>
#include <string>

namespace skein {
namespace lsp {
namespace json_rpc {

enum class ReadStatus {
    Ok,
    Eof,
    Error,
    Timeout,
};

/* Read one LSP base protocol frame.  The JSON payload is returned without a
 * trailing newline.  A thread-local read buffer retains bytes belonging to
 * following frames. */
ReadStatus read_message(FILE *input, std::string &payload, std::string &error);

/* Read with a timeout.  `timeout_ms < 0` means wait indefinitely. */
ReadStatus read_message_timeout(FILE *input, int timeout_ms,
                                std::string &payload, std::string &error);

/* Write one LSP base protocol frame.  The write is serialized with other
 * process stdout writers. */
bool write_message(FILE *output, const std::string &payload,
                   std::string &error);

} // namespace json_rpc
} // namespace lsp
} // namespace skein

#endif
