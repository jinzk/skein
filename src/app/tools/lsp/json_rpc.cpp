#include "json_rpc.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "stdio_stream.h"

namespace skein {
namespace lsp {
namespace json_rpc {

namespace {

constexpr std::size_t MAX_MESSAGE_BYTES = 64u * 1024u * 1024u;

std::string lowercase(std::string text) {
    for (char &c : text)
        c = (char)std::tolower((unsigned char)c);
    return text;
}

std::string trim(const std::string &text) {
    std::size_t first = 0;
    while (first < text.size() &&
           std::isspace((unsigned char)text[first]))
        ++first;
    std::size_t last = text.size();
    while (last > first && std::isspace((unsigned char)text[last - 1]))
        --last;
    return text.substr(first, last - first);
}

bool append_available(FILE *input, std::string &buffer, std::string &error) {
    /* Do not use a multi-byte fread here: on a pipe it blocks until the
     * requested count is available or EOF, which would stall an interactive
     * LSP session after a single small request. */
    const int c = std::fgetc(input);
    if (c == EOF) {
        if (std::ferror(input)) {
            error = "failed to read LSP input";
            std::clearerr(input);
        }
        return false;
    }
    buffer.push_back((char)c);
    return true;
}

bool parse_content_length(const std::string &headers, std::size_t &length,
                          std::string &error) {
    bool found = false;
    std::size_t start = 0;
    while (start <= headers.size()) {
        std::size_t end = headers.find('\n', start);
        if (end == std::string::npos)
            end = headers.size();
        std::string line = headers.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos &&
            lowercase(trim(line.substr(0, colon))) == "content-length") {
            const std::string value = trim(line.substr(colon + 1));
            if (value.empty()) {
                error = "empty Content-Length header";
                return false;
            }
            char *endptr = NULL;
            errno = 0;
            unsigned long long parsed = std::strtoull(value.c_str(), &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0') {
                error = "invalid Content-Length header";
                return false;
            }
            if (parsed > MAX_MESSAGE_BYTES) {
                error = "LSP message exceeds size limit";
                return false;
            }
            length = (std::size_t)parsed;
            found = true;
        }
        if (end == headers.size())
            break;
        start = end + 1;
    }
    if (!found) {
        error = "missing Content-Length header";
        return false;
    }
    return true;
}

} // namespace

ReadStatus read_message(FILE *input, std::string &payload, std::string &error) {
    static thread_local std::string buffer;

    for (;;) {
        std::size_t header_end = buffer.find("\r\n\r\n");
        std::size_t separator_size = 4;
        if (header_end == std::string::npos) {
            header_end = buffer.find("\n\n");
            separator_size = 2;
        }
        if (header_end == std::string::npos) {
            if (!append_available(input, buffer, error)) {
                if (buffer.empty() && error.empty())
                    return ReadStatus::Eof;
                return error.empty() ? ReadStatus::Eof : ReadStatus::Error;
            }
            continue;
        }

        const std::size_t payload_start = header_end + separator_size;
        std::size_t content_length = 0;
        if (!parse_content_length(buffer.substr(0, header_end), content_length,
                                  error)) {
            buffer.clear();
            return ReadStatus::Error;
        }
        if (buffer.size() - payload_start < content_length) {
            if (!append_available(input, buffer, error)) {
                if (error.empty())
                    error = "unexpected EOF in LSP message body";
                return ReadStatus::Error;
            }
            continue;
        }

        payload = buffer.substr(payload_start, content_length);
        buffer.erase(0, payload_start + content_length);
        error.clear();
        return ReadStatus::Ok;
    }
}

bool write_message(FILE *output, const std::string &payload,
                   std::string &error) {
    const std::string header =
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    std::lock_guard<std::mutex> lock(stdio_output_mutex());
    if (std::fwrite(header.data(), 1, header.size(), output) != header.size() ||
        std::fwrite(payload.data(), 1, payload.size(), output) !=
            payload.size() ||
        std::fflush(output) != 0) {
        error = "failed to write LSP message";
        return false;
    }
    error.clear();
    return true;
}

} // namespace json_rpc
} // namespace lsp
} // namespace skein
