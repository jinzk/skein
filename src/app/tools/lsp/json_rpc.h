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
};

/* Read one LSP base protocol frame.  The JSON payload is returned without a
 * trailing newline.  A thread-local read buffer retains bytes belonging to
 * following frames. */
ReadStatus read_message(FILE *input, std::string &payload, std::string &error);

/* Write one LSP base protocol frame.  The write is serialized with other
 * process stdout writers. */
bool write_message(FILE *output, const std::string &payload,
                   std::string &error);

} // namespace json_rpc
} // namespace lsp
} // namespace skein

#endif
