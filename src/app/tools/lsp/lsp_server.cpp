#include "lsp_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "completion_index.h"
#include "dict.h"
#include "document_store.h"
#include "formatter.h"
#include "json.h"
#include "json_rpc.h"
#include "lsp_json.h"
#include "position.h"
#include "static_type.h"
#include "std_namespace.h"
#include "tools/tool_analysis.h"
#include "typecheck_builtins.h"
#include "valuetype.h"
#include "workspace_index.h"

namespace skein {
namespace lsp {

namespace {

namespace json = skein::lsp::json;

int lsp_debounce_ms() {
    const char *raw = std::getenv("SKEIN_LSP_DEBOUNCE_MS");
    if (!raw || !*raw)
        return 75;
    char *end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || parsed < 0)
        return 75;
    return (int)std::min<long>(parsed, 5000);
}

bool starts_with(const std::string &text, const std::string &prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool starts_with(const std::string &text, const char *prefix) {
    return starts_with(text, std::string(prefix));
}

bool is_ident_byte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

int utf16_length(const std::string &text) {
    std::size_t pos = 0;
    int units = 0;
    while (pos < text.size()) {
        unsigned int codepoint = 0;
        pos += decode_utf8(text, pos, codepoint);
        units += codepoint > 0xffff ? 2 : 1;
    }
    return units;
}

std::string percent_decode(const std::string &text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%' || i + 2 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = hex(text[i + 1]);
        const int low = hex(text[i + 2]);
        if (high < 0 || low < 0) {
            out.push_back(text[i]);
            continue;
        }
        out.push_back((char)((high << 4) | low));
        i += 2;
    }
    return out;
}

std::string uri_to_path(const std::string &uri) {
    if (!starts_with(uri, "file://"))
        return uri;
    std::string path = percent_decode(uri.substr(7));
    /* file://localhost/D:/file.skein and file:///D:/file.skein are both used. */
    if (starts_with(path, "localhost/"))
        path.erase(0, 10);
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' &&
        std::isalpha((unsigned char)path[1]) && path[2] == ':')
        path.erase(0, 1);
#endif
    return path;
}

std::size_t ident_start(const std::string &text, std::size_t offset) {
    offset = std::min(offset, text.size());
    while (offset > 0 && is_ident_byte((unsigned char)text[offset - 1]))
        --offset;
    return offset;
}

std::pair<std::size_t, std::size_t> word_range(const std::string &text,
                                               std::size_t offset) {
    offset = std::min(offset, text.size());
    const std::size_t start = ident_start(text, offset);
    std::size_t end = offset;
    while (end < text.size() && is_ident_byte((unsigned char)text[end]))
        ++end;
    return {start, end};
}

std::string qualifier_before(const std::string &text, std::size_t word_start) {
    if (word_start == 0 || text[word_start - 1] != '.')
        return std::string();
    std::size_t pos = word_start - 1;
    const std::size_t end = pos;
    while (pos > 0) {
        const unsigned char c = (unsigned char)text[pos - 1];
        if (!is_ident_byte(c) && c != '.')
            break;
        --pos;
    }
    std::string qualifier = text.substr(pos, end - pos);
    while (!qualifier.empty() && qualifier.back() == '.')
        qualifier.pop_back();
    while (!qualifier.empty() && qualifier.front() == '.')
        qualifier.erase(qualifier.begin());
    return qualifier;
}

/** Map an import alias (or bare qualifier) to its module path in a document. */
std::string resolve_import_alias(const std::string &text,
                                 const std::string &qualifier) {
    if (qualifier.empty() || qualifier == "std" ||
        starts_with(qualifier, "std."))
        return qualifier;

    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t line_end = text.find('\n', pos);
        const std::size_t end = line_end == std::string::npos ? text.size() : line_end;
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        std::size_t cursor = 0;
        while (cursor < line.size() &&
               std::isspace((unsigned char)line[cursor]))
            ++cursor;
        if (line.compare(cursor, 7, "import ") != 0)
            continue;
        cursor += 7;
        while (cursor < line.size() &&
               std::isspace((unsigned char)line[cursor]))
            ++cursor;
        const std::size_t path_start = cursor;
        while (cursor < line.size() &&
               (is_ident_byte((unsigned char)line[cursor]) ||
                line[cursor] == '.'))
            ++cursor;
        if (cursor == path_start)
            continue;
        std::string path = line.substr(path_start, cursor - path_start);
        std::string alias = path.substr(path.rfind('.') == std::string::npos
                                            ? 0
                                            : path.rfind('.') + 1);

        while (cursor < line.size() &&
               std::isspace((unsigned char)line[cursor]))
            ++cursor;
        if (line.compare(cursor, 3, "as ") == 0) {
            cursor += 3;
            while (cursor < line.size() &&
                   std::isspace((unsigned char)line[cursor]))
                ++cursor;
            const std::size_t alias_start = cursor;
            while (cursor < line.size() &&
                   is_ident_byte((unsigned char)line[cursor]))
                ++cursor;
            if (cursor > alias_start)
                alias = line.substr(alias_start, cursor - alias_start);
        }
        if (alias == qualifier)
            return path;
    }
    return qualifier;
}

struct std_entry {
    enum class kind_t { Module, Function, Value };
    kind_t kind = kind_t::Value;
    std::string signature;
    std::vector<std::string> children;
};

const char *keyword_detail(const std::string &word) {
    static const std::map<std::string, const char *> details = {
        {"if", "conditional"},        {"else", "conditional alternative"},
        {"while", "while loop"},      {"do", "do/while loop"},
        {"for", "for loop"},          {"in", "iteration"},
        {"switch", "switch"},         {"case", "switch case"},
        {"default", "switch default"},{"match", "pattern match"},
        {"let", "binding"},           {"const", "constant binding"},
        {"func", "function"},         {"struct", "struct type"},
        {"enum", "enum type"},        {"protocol", "protocol"},
        {"impl", "implementation"},   {"extern", "external declaration"},
        {"package", "package"},       {"module", "module"},
        {"import", "import"},         {"include", "include"},
        {"export", "export"},         {"return", "return"},
        {"break", "break"},           {"continue", "continue"},
        {"yield", "yield"},           {"defer", "defer"},
        {"launch", "launch"},         {"try", "exception handling"},
        {"on", "exception handler"},  {"error", "exception handling"},
        {"recover", "exception handling"}, {"fail", "failure"},
        {"pass", "no-op"},            {"is", "type test"},
        {"as", "cast"},
        {"true", "boolean literal"},  {"false", "boolean literal"},
        {"null", "null literal"},
    };
    auto it = details.find(word);
    return it == details.end() ? "keyword" : it->second;
}

struct doc_symbol {
    std::string name;
    int kind = 12;
    int line = 0;
    int start_character = 0;
};

int symbol_kind_for_keyword(const std::string &keyword) {
    if (keyword == "func") return 12;       // Function
    if (keyword == "let") return 13;        // Variable
    if (keyword == "const") return 14;      // Constant
    if (keyword == "struct" || keyword == "impl") return 5; // Class
    if (keyword == "enum") return 10;       // Enum
    if (keyword == "protocol") return 11;      // Interface
    if (keyword == "package" || keyword == "module") return 3; // Namespace
    return 12;
}

std::vector<doc_symbol> scan_document_symbols(const std::string &text) {
    static const std::vector<std::string> keywords = {
        "func", "struct", "enum", "protocol", "impl", "let", "const",
        "package", "module", "extern",
    };
    std::vector<doc_symbol> symbols;
    std::size_t pos = 0;
    int line = 0;
    while (pos <= text.size()) {
        std::size_t end = pos;
        while (end < text.size() && text[end] != '\r' && text[end] != '\n')
            ++end;
        const std::string raw = text.substr(pos, end - pos);

        std::size_t cursor = 0;
        while (cursor < raw.size() && std::isspace((unsigned char)raw[cursor]))
            ++cursor;
        const std::size_t export_word = std::string("export ").size();
        if (starts_with(raw.substr(cursor), "export ")) {
            cursor += export_word;
            while (cursor < raw.size() &&
                   std::isspace((unsigned char)raw[cursor]))
                ++cursor;
        }

        for (const std::string &keyword : keywords) {
            if (raw.compare(cursor, keyword.size(), keyword) != 0)
                continue;
            std::size_t after_keyword = cursor + keyword.size();
            if (after_keyword >= raw.size() ||
                !std::isspace((unsigned char)raw[after_keyword]))
                continue;
            while (after_keyword < raw.size() &&
                   std::isspace((unsigned char)raw[after_keyword]))
                ++after_keyword;
            std::size_t name_length = 0;
            while (after_keyword + name_length < raw.size() &&
                   is_ident_byte((unsigned char)raw[after_keyword + name_length]))
                ++name_length;
            if (name_length == 0)
                break;

            doc_symbol symbol;
            symbol.name = raw.substr(after_keyword, name_length);
            symbol.kind = symbol_kind_for_keyword(keyword);
            symbol.line = line;
            symbol.start_character = utf16_length(raw.substr(0, after_keyword));
            symbols.push_back(std::move(symbol));
            break;
        }

        if (end >= text.size())
            break;
        if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
            ++end;
        pos = end + 1;
        ++line;
    }
    return symbols;
}

struct doc_type_member {
    std::string name;
    bool method = false;
    bool constructor = false;
};

std::size_t skip_space(const std::string &text, std::size_t pos) {
    while (pos < text.size() && std::isspace((unsigned char)text[pos]))
        ++pos;
    return pos;
}

std::size_t skip_line(const std::string &text, std::size_t pos) {
    while (pos < text.size() && text[pos] != '\r' && text[pos] != '\n')
        ++pos;
    return pos;
}

std::size_t skip_quoted(const std::string &text, std::size_t pos) {
    const char quote = text[pos++];
    while (pos < text.size()) {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos += 2;
            continue;
        }
        if (text[pos] == quote)
            return pos + 1;
        ++pos;
    }
    return pos;
}

std::string read_ident(const std::string &text, std::size_t &pos) {
    const std::size_t start = pos;
    while (pos < text.size() && is_ident_byte((unsigned char)text[pos]))
        ++pos;
    return text.substr(start, pos - start);
}

/* Find the '}' matching the '{' at `open`; returns text.size() when
 * unbalanced.  Comments, strings and chars are skipped. */
std::size_t matching_brace(const std::string &text, std::size_t open) {
    int depth = 0;
    std::size_t pos = open;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos = skip_line(text, pos + 2);
            continue;
        }
        if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            const std::size_t end = text.find("*/", pos + 2);
            pos = end == std::string::npos ? text.size() : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            pos = skip_quoted(text, pos);
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0)
                return pos;
            if (depth < 0)
                return text.size();
        }
        ++pos;
    }
    return text.size();
}

void scan_type_body(const std::string &text, std::size_t begin, std::size_t end,
                    bool is_impl, std::vector<doc_type_member> &members) {
    int depth = 0;
    std::size_t pos = begin;
    while (pos < end) {
        const char c = text[pos];
        if (c == '/' && pos + 1 < end && text[pos + 1] == '/') {
            pos = skip_line(text, pos + 2);
            continue;
        }
        if (c == '/' && pos + 1 < end && text[pos + 1] == '*') {
            const std::size_t close = text.find("*/", pos + 2);
            pos = close == std::string::npos || close >= end ? end : close + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            pos = skip_quoted(text, pos);
            continue;
        }
        if (c == '{') {
            ++depth;
            ++pos;
            continue;
        }
        if (c == '}') {
            if (depth > 0)
                --depth;
            ++pos;
            continue;
        }
        if (depth != 0 || !is_ident_byte((unsigned char)c)) {
            ++pos;
            continue;
        }

        const std::string word = read_ident(text, pos);
        if (word == "export" || word == "pub" || word == "static" ||
            word == "const")
            continue;

        if (is_impl) {
            if (word == "func") {
                std::size_t name_pos = skip_space(text, pos);
                const std::string name = read_ident(text, name_pos);
                if (!name.empty()) {
                    doc_type_member member;
                    member.name = name;
                    member.method = true;
                    member.constructor = name == "new";
                    members.push_back(std::move(member));
                }
            }
            continue;
        }

        /* Struct bodies may declare methods inline as well as fields. */
        if (word == "func") {
            std::size_t name_pos = skip_space(text, pos);
            const std::string name = read_ident(text, name_pos);
            if (!name.empty()) {
                doc_type_member member;
                member.name = name;
                member.method = true;
                member.constructor = name == "new";
                members.push_back(std::move(member));
            }
            continue;
        }

        /* Struct body: `name: Type` declares a field. */
        std::size_t colon = skip_space(text, pos);
        if (colon < end && text[colon] == ':' &&
            (colon + 1 >= end || text[colon + 1] != ':')) {
            doc_type_member member;
            member.name = word;
            members.push_back(std::move(member));
        }
    }
}

std::map<std::string, std::vector<doc_type_member>> scan_type_members(
    const std::string &text) {
    std::map<std::string, std::vector<doc_type_member>> out;
    int depth = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos = skip_line(text, pos + 2);
            continue;
        }
        if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            const std::size_t close = text.find("*/", pos + 2);
            pos = close == std::string::npos ? text.size() : close + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            pos = skip_quoted(text, pos);
            continue;
        }
        if (c == '{') {
            ++depth;
            ++pos;
            continue;
        }
        if (c == '}') {
            if (depth > 0)
                --depth;
            ++pos;
            continue;
        }
        if (depth != 0 || !is_ident_byte((unsigned char)c)) {
            ++pos;
            continue;
        }

        const std::string word = read_ident(text, pos);
        if (word == "export" || word == "pub")
            continue;
        if (word != "struct" && word != "impl")
            continue;

        std::size_t name_pos = skip_space(text, pos);
        const std::string name = read_ident(text, name_pos);
        if (name.empty())
            continue;
        const std::size_t open = skip_space(text, name_pos);
        if (open >= text.size() || text[open] != '{')
            continue;
        const std::size_t close = matching_brace(text, open);
        scan_type_body(text, open + 1, close, word == "impl", out[name]);
        pos = close < text.size() ? close + 1 : text.size();
    }
    return out;
}

value_t *range_value(const lsp_position &start, const lsp_position &end) {
    value_t *range = json::object();
    value_t *start_value = json::object();
    json::set(start_value, "line", json::integer(start.line));
    json::set(start_value, "character", json::integer(start.character));
    value_t *end_value = json::object();
    json::set(end_value, "line", json::integer(end.line));
    json::set(end_value, "character", json::integer(end.character));
    json::set(range, "start", start_value);
    json::set(range, "end", end_value);
    return range;
}

value_t *location_value(const std::string &uri, const lsp_position &start,
                        const lsp_position &end) {
    value_t *location = json::object();
    json::set(location, "uri", json::string(uri));
    json::set(location, "range", range_value(start, end));
    return location;
}

} // namespace

class lsp_server_impl {
public:
    lsp_server_impl(const lsp_options &options, FILE *output)
        : options_(options), output_(output), debounce_ms_(lsp_debounce_ms()) {
        build_std_index();
    }

    void ensure_workspace_index() {
        if (index_built_)
            return;
        index_built_ = true;
        std::vector<std::string> roots = options_.package_dirs;
        for (const std::string &root : index_roots_) {
            if (std::find(roots.begin(), roots.end(), root) == roots.end())
                roots.push_back(root);
        }
        if (roots.empty())
            return;
        index_.rebuild(roots);
    }

    int run(FILE *input) {
        for (;;) {
            int timeout_ms = -1;
            if (pending_analysis_) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        pending_deadline_ - std::chrono::steady_clock::now())
                        .count();
                timeout_ms = remaining <= 0 ? 0 : (int)remaining;
            }

            std::string payload;
            std::string error;
            json_rpc::ReadStatus status =
                json_rpc::read_message_timeout(input, timeout_ms, payload,
                                                error);
            if (status == json_rpc::ReadStatus::Timeout) {
                flush_pending_analysis();
                continue;
            }
            if (status == json_rpc::ReadStatus::Eof)
                break;
            if (status == json_rpc::ReadStatus::Error) {
                std::fprintf(stderr, "skein lsp: %s\n", error.c_str());
                send_error(json::null(), -32700, error);
                continue;
            }

            bool should_exit = false;
            int exit_code = 0;
            handle_payload(payload, should_exit, exit_code);
            if (should_exit) {
                flush_pending_analysis();
                return exit_code;
            }
        }
        flush_pending_analysis();
        return 0;
    }

private:
    void build_std_index() {
        value_t *root = skein::stdlib::build_namespace();
        collect_std_node(root, "std");
        freeval(root);
    }

    void collect_std_node(const value_t *value, const std::string &path) {
        if (!value || value->type != T_dict)
            return;
        std_entry entry;
        entry.kind = std_entry::kind_t::Module;
        entry.signature = "namespace " + path;
        const dict_value_t *dict = (const dict_value_t *)value;
        std::vector<std::pair<std::string, const value_t *>> children;
        for (const auto &item : dict->payload->items) {
            if (!item.first || item.first->type != T_string)
                continue;
            const char *name = ((const string_value_t *)item.first)->value;
            if (!name)
                continue;
            const std::string child_path = path + "." + name;
            entry.children.push_back(name);
            children.emplace_back(name, item.second);
        }
        std_index_[path] = entry;
        for (const auto &child : children) {
            const std::string child_path = path + "." + child.first;
            const value_t *child_value = child.second;
            if (!child_value)
                continue;
            if (child_value->type == T_dict) {
                collect_std_node(child_value, child_path);
                continue;
            }

            std_entry child_entry;
            child_entry.kind = std_entry::kind_t::Value;
            child_entry.signature = "value " + child_path;
            if (child_value->type == T_builtin) {
                child_entry.kind = std_entry::kind_t::Function;
                const int functype =
                    ((const builtin_value_t *)child_value)->functype;
                const skein::typecheck::TypeNode *type =
                    skein::typecheck::detail::builtin_function_type(functype);
                if (!type)
                    type = skein::typecheck::detail::builtin_polymorphic_type(
                        child_path);
                const char *display =
                    type ? skein::typecheck::type_display(type) : nullptr;
                child_entry.signature =
                    display ? display : ("builtin " + child_path);
            }
            std_index_[child_path] = child_entry;
        }
    }

    void send_response(const value_t *id, value_t *result) {
        value_t *message = json::object();
        json::set(message, "jsonrpc", json::string("2.0"));
        json::set(message, "id", id ? dupval(const_cast<value_t *>(id)) : json::null());
        json::set(message, "result", result);
        send_message(message);
    }

    void send_error(const value_t *id, int code, const std::string &message) {
        value_t *error = json::object();
        json::set(error, "code", json::integer(code));
        json::set(error, "message", json::string(message));
        value_t *envelope = json::object();
        json::set(envelope, "jsonrpc", json::string("2.0"));
        json::set(envelope, "id", id ? dupval(const_cast<value_t *>(id)) : json::null());
        json::set(envelope, "error", error);
        send_message(envelope);
    }

    void send_notification(const std::string &method, value_t *params) {
        value_t *message = json::object();
        json::set(message, "jsonrpc", json::string("2.0"));
        json::set(message, "method", json::string(method));
        json::set(message, "params", params);
        send_message(message);
    }

    void send_message(value_t *message) {
        std::string error;
        const std::string payload = json::stringify(message, error);
        freeval(message);
        if (!error.empty()) {
            std::fprintf(stderr, "skein lsp: cannot encode response: %s\n",
                         error.c_str());
            return;
        }
        if (!json_rpc::write_message(output_, payload, error))
            std::fprintf(stderr, "skein lsp: %s\n", error.c_str());
    }

    analysis_options analysis_options_for(const std::string &uri) const {
        analysis_options result;
        result.source_name = uri_to_path(uri);
        result.encoding = options_.encoding;
        result.package_dirs = options_.package_dirs;
        result.typecheck_mode = options_.typecheck_mode;
        result.keep_going = true;
        result.suppress_text = true;
        return result;
    }

    void schedule_analysis(const std::string &uri) {
        pending_analysis_ = true;
        pending_uri_ = uri;
        pending_deadline_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(debounce_ms_);
    }

    void flush_pending_analysis() {
        if (!pending_analysis_)
            return;
        pending_analysis_ = false;
        const lsp_document *document = documents_.find(pending_uri_);
        if (!document)
            return;
        publish_diagnostics(pending_uri_, document->version, document->text);
    }

    void apply_content_changes(std::string &text, const value_t *changes) {
        if (!changes || changes->type != T_list)
            return;
        const auto &items = ((const list_value_t *)changes)->payload->items;
        for (const value_t *change : items) {
            if (!change)
                continue;
            if (change->type == T_string) {
                text = json::string_value(change);
                continue;
            }
            const std::string replacement =
                json::string_value(json::member(change, "text"));
            const value_t *range = json::member(change, "range");
            if (!range || range->type != T_dict) {
                text = replacement;
                continue;
            }
            const value_t *start_value = json::member(range, "start");
            const value_t *end_value = json::member(range, "end");
            if (!start_value || !end_value)
                continue;
            lsp_position start;
            start.line = (int)json::int_value(
                json::member(start_value, "line"));
            start.character = (int)json::int_value(
                json::member(start_value, "character"));
            lsp_position end;
            end.line = (int)json::int_value(json::member(end_value, "line"));
            end.character = (int)json::int_value(
                json::member(end_value, "character"));
            const std::size_t start_offset =
                byte_offset_from_position(text, start);
            const std::size_t end_offset =
                byte_offset_from_position(text, end);
            if (start_offset > end_offset || end_offset > text.size())
                continue;
            text.replace(start_offset, end_offset - start_offset,
                         replacement);
        }
    }

    void publish_diagnostics(const std::string &uri, int version,
                             const std::string &text) {
        analysis_result result =
            analyze_source_text(text, analysis_options_for(uri));

        lsp_document document;
        document.uri = uri;
        document.version = version;
        document.text = text;
        document.symbols = result.symbols;
        documents_.put(document);

        value_t *diagnostics = json::array();
        for (const diagnostic_record &record : result.diagnostics) {
            value_t *diagnostic = json::object();
            const int line = std::max(0, record.line - 1);
            const int character = std::max(0, (int)record.utf16_offset);
            const lsp_position start{line, character};
            const lsp_position end{line, character + 1};
            json::set(diagnostic, "range", range_value(start, end));
            int severity = 1;
            switch (record.severity) {
                case DiagnosticSeverity::Error: severity = 1; break;
                case DiagnosticSeverity::Warning: severity = 2; break;
                case DiagnosticSeverity::Information: severity = 3; break;
                case DiagnosticSeverity::Hint: severity = 4; break;
            }
            json::set(diagnostic, "severity", json::integer(severity));
            json::set(diagnostic, "code", json::string(record.code));
            json::set(diagnostic, "source", json::string("skein"));
            json::set(diagnostic, "message", json::string(record.message));
            json::push(diagnostics, diagnostic);
        }

        value_t *params = json::object();
        json::set(params, "uri", json::string(uri));
        json::set(params, "version", json::integer(version));
        json::set(params, "diagnostics", diagnostics);
        send_notification("textDocument/publishDiagnostics", params);
    }

    void collect_workspace_folders(const value_t *params) {
        if (!params)
            return;
        const value_t *folders = json::member(params, "workspaceFolders");
        if (!folders || folders->type != T_list)
            return;
        const auto &items = ((const list_value_t *)folders)->payload->items;
        for (const value_t *item : items) {
            const std::string uri =
                json::string_value(json::member(item, "uri"));
            if (uri.empty())
                continue;
            const std::string path = uri_to_path(uri);
            if (!path.empty() &&
                std::find(index_roots_.begin(), index_roots_.end(), path) ==
                    index_roots_.end())
                index_roots_.push_back(path);
        }
    }

    value_t *initialize_result() {
        value_t *sync = json::object();
        json::set(sync, "openClose", json::boolean(true));
        json::set(sync, "change", json::integer(2)); // TextDocumentSyncKind.Incremental
        json::set(sync, "willSave", json::boolean(false));
        json::set(sync, "willSaveWaitUntil", json::boolean(false));
        json::set(sync, "save", json::boolean(true));

        value_t *trigger_characters = json::array();
        json::push(trigger_characters, json::string("."));
        value_t *completion = json::object();
        json::set(completion, "resolveProvider", json::boolean(false));
        json::set(completion, "triggerCharacters", trigger_characters);

        value_t *capabilities = json::object();
        json::set(capabilities, "textDocumentSync", sync);
        json::set(capabilities, "publishDiagnosticsProvider",
                  json::boolean(true));
        json::set(capabilities, "documentFormattingProvider",
                  json::boolean(true));
        json::set(capabilities, "documentRangeFormattingProvider",
                  json::boolean(true));
        json::set(capabilities, "completionProvider", completion);
        json::set(capabilities, "hoverProvider", json::boolean(true));
        json::set(capabilities, "definitionProvider", json::boolean(true));
        json::set(capabilities, "documentSymbolProvider",
                  json::boolean(true));
        value_t *workspace = json::object();
        json::set(workspace, "workspaceFolders", json::boolean(true));
        json::set(capabilities, "workspace", workspace);

        value_t *server_info = json::object();
        json::set(server_info, "name", json::string("skein"));
        json::set(server_info, "version", json::string("1.0.0"));

        value_t *result = json::object();
        json::set(result, "capabilities", capabilities);
        json::set(result, "serverInfo", server_info);
        return result;
    }

    std::string document_uri(const value_t *params) const {
        const value_t *document = json::member(params, "textDocument");
        return json::string_value(json::member(document, "uri"));
    }

    std::string describe_name(const lsp_document &document,
                              const std::string &word,
                              const std::string &qualifier) const {
        std::string path = word;
        if (qualifier == "std" || starts_with(qualifier, "std."))
            path = qualifier + "." + word;
        auto it = std_index_.find(path);
        if (it != std_index_.end())
            return it->second.signature;

        for (const analysis_symbol &symbol : document.symbols) {
            if (symbol.name != word)
                continue;
            if (!symbol.type.empty())
                return (symbol.callable ? "func " : "let ") + word + ": " +
                       symbol.type;
            return symbol.callable ? "func " + word : "let " + word;
        }

        /* User module exports from the workspace index. */
        if (!qualifier.empty() &&
            !(qualifier == "std" || starts_with(qualifier, "std."))) {
            const std::string path = resolve_import_alias(document.text, qualifier);
            for (const index_entry &entry : index_.lookup(path + "." + word)) {
                if (!entry.type.empty())
                    return (entry.callable ? "func " : "let ") + word + ": " +
                           entry.type;
                return entry.callable ? "func " + word : "let " + word;
            }
        }

        const char *keyword = keyword_detail(word);
        if (keyword)
            return std::string(keyword) + " (" + word + ")";
        return "symbol " + word;
    }

    value_t *completion_result(const value_t *params) {
        value_t *items = json::array();
        const std::string uri = document_uri(params);
        const lsp_document *document = documents_.find(uri);
        if (!document) {
            value_t *result = json::object();
            json::set(result, "isIncomplete", json::boolean(false));
            json::set(result, "items", items);
            return result;
        }

        const value_t *position_value = json::member(params, "position");
        lsp_position position;
        position.line = (int)json::int_value(json::member(position_value, "line"));
        position.character =
            (int)json::int_value(json::member(position_value, "character"));
        const std::size_t offset =
            byte_offset_from_position(document->text, position);
        const std::size_t word_start = ident_start(document->text, offset);
        const std::string prefix = document->text.substr(word_start, offset - word_start);
        const std::string qualifier = qualifier_before(document->text, word_start);

        completion::index candidates;
        if (qualifier.empty()) {
            completion::add_keywords(candidates);
            for (const analysis_symbol &symbol : document->symbols)
                candidates.add_top(
                    {symbol.name, symbol.callable ? 3 : 6,
                     symbol.type.empty()
                         ? (symbol.callable ? "function" : "variable")
                         : symbol.type,
                     false});
            candidates.add_top({"std", 9, "standard library", true});

            /* Top-level module names and bare import aliases. */
            std::vector<std::string> module_names;
            for (const index_entry &entry : index_.all()) {
                const std::size_t dot = entry.qualified_name.find('.');
                if (dot == std::string::npos)
                    continue;
                const std::string top = entry.qualified_name.substr(0, dot);
                if (std::find(module_names.begin(), module_names.end(), top) ==
                    module_names.end())
                    module_names.push_back(top);
            }
            for (const std::string &name : module_names)
                candidates.add_top({name, 9, "module", true});
        }

        /* std members come from the same namespace index used by hover. */
        for (const auto &std_item : std_index_) {
            if (std_item.second.kind != std_entry::kind_t::Module)
                continue;
            for (const std::string &child : std_item.second.children) {
                auto child_it = std_index_.find(std_item.first + "." + child);
                const bool is_module =
                    child_it != std_index_.end() &&
                    child_it->second.kind == std_entry::kind_t::Module;
                const bool is_function =
                    child_it != std_index_.end() &&
                    child_it->second.kind == std_entry::kind_t::Function;
                const std::string detail =
                    child_it != std_index_.end()
                        ? child_it->second.signature
                        : "std." + child;
                candidates.add_member(
                    std_item.first,
                    {child, is_module ? 9 : (is_function ? 3 : 6), detail,
                     is_module});
            }
        }

        /* Imported module / package members from the workspace index. */
        if (!qualifier.empty() &&
            !(qualifier == "std" || starts_with(qualifier, "std."))) {
            const std::string path =
                resolve_import_alias(document->text, qualifier);
            for (const index_entry &entry : index_.complete(path)) {
                const std::size_t dot = entry.qualified_name.rfind('.');
                const std::string label =
                    dot == std::string::npos
                        ? entry.qualified_name
                        : entry.qualified_name.substr(dot + 1);
                candidates.add_member(
                    qualifier,
                    {label, entry.callable ? 3 : 6,
                     entry.type.empty()
                         ? (entry.callable ? "function" : "value")
                         : entry.type,
                     false});
            }
        }

        /* Fields, methods and the `new` constructor of struct types defined
         * in this document. */
        if (!qualifier.empty()) {
            const auto type_members = scan_type_members(document->text);
            auto type_it = type_members.find(qualifier);
            if (type_it != type_members.end()) {
                for (const doc_type_member &member : type_it->second) {
                    candidates.add_member(
                        qualifier,
                        {member.name, member.method ? 3 : 6,
                         member.constructor
                             ? "constructor"
                             : (member.method ? "method" : "field"),
                         false});
                }
            }
        }

        for (const completion::item &candidate :
             candidates.complete(prefix, qualifier)) {
            value_t *item = json::object();
            json::set(item, "label", json::string(candidate.label));
            json::set(item, "kind", json::integer(candidate.kind));
            json::set(item, "detail", json::string(candidate.detail));
            json::push(items, item);
        }

        value_t *result = json::object();
        json::set(result, "isIncomplete", json::boolean(false));
        json::set(result, "items", items);
        return result;
    }

    value_t *hover_result(const value_t *params) {
        const std::string uri = document_uri(params);
        const lsp_document *document = documents_.find(uri);
        if (!document)
            return json::object();

        const value_t *position_value = json::member(params, "position");
        lsp_position position;
        position.line = (int)json::int_value(json::member(position_value, "line"));
        position.character =
            (int)json::int_value(json::member(position_value, "character"));
        const std::size_t offset =
            byte_offset_from_position(document->text, position);
        const auto word_bounds = word_range(document->text, offset);
        const std::size_t word_start = word_bounds.first;
        const std::string word = document->text.substr(
            word_start, word_bounds.second - word_start);
        value_t *result = json::object();
        if (word.empty())
            return result;

        const std::string qualifier = qualifier_before(document->text, word_start);
        const std::string detail = describe_name(*document, word, qualifier);
        value_t *contents = json::object();
        json::set(contents, "kind", json::string("markdown"));
        json::set(contents, "value", json::string("`" + detail + "`"));
        json::set(result, "contents", contents);
        return result;
    }

    value_t *formatting_result(const value_t *params) {
        value_t *edits = json::array();
        const std::string uri = document_uri(params);
        const lsp_document *document = documents_.find(uri);
        if (!document)
            return edits;

        std::string formatted;
        std::string error;
        if (!skein::formatter::format_source(document->text, formatted, error)) {
            std::fprintf(stderr, "skein lsp: formatting failed: %s\n",
                         error.c_str());
            return edits;
        }

        analysis_options options = analysis_options_for(uri);
        options.typecheck_mode = skein::typecheck::Mode::Off;
        analysis_result validation = analyze_source_text(formatted, options);
        if (validation.error_count != 0) {
            std::fprintf(stderr,
                         "skein lsp: formatted output failed parse validation\n");
            return edits;
        }

        const lsp_position start{0, 0};
        const lsp_position end =
            position_from_byte_offset(document->text, document->text.size());
        value_t *edit = json::object();
        json::set(edit, "range", range_value(start, end));
        json::set(edit, "newText", json::string(formatted));
        json::push(edits, edit);
        return edits;
    }

    value_t *document_symbol_result(const value_t *params) {
        value_t *symbols = json::array();
        const std::string uri = document_uri(params);
        const lsp_document *document = documents_.find(uri);
        if (!document)
            return symbols;

        for (const doc_symbol &symbol :
             scan_document_symbols(document->text)) {
            value_t *entry = json::object();
            json::set(entry, "name", json::string(symbol.name));
            json::set(entry, "kind", json::integer(symbol.kind));
            const lsp_position start{symbol.line, symbol.start_character};
            const lsp_position end{symbol.line,
                                   symbol.start_character +
                                       utf16_length(symbol.name)};
            json::set(entry, "location", location_value(uri, start, end));
            json::push(symbols, entry);
        }
        return symbols;
    }

    value_t *definition_result(const value_t *params) {
        value_t *locations = json::array();
        const std::string uri = document_uri(params);
        const lsp_document *document = documents_.find(uri);
        if (!document)
            return locations;

        const value_t *position_value = json::member(params, "position");
        lsp_position position;
        position.line = (int)json::int_value(json::member(position_value, "line"));
        position.character =
            (int)json::int_value(json::member(position_value, "character"));
        const std::size_t offset =
            byte_offset_from_position(document->text, position);
        const auto word_bounds = word_range(document->text, offset);
        const std::size_t word_start = word_bounds.first;
        const std::string word = document->text.substr(
            word_start, word_bounds.second - word_start);
        if (word.empty())
            return locations;

        bool found_local = false;
        for (const doc_symbol &symbol :
             scan_document_symbols(document->text)) {
            if (symbol.name != word)
                continue;
            const lsp_position start{symbol.line, symbol.start_character};
            const lsp_position end{symbol.line,
                                   symbol.start_character +
                                       utf16_length(symbol.name)};
            json::push(locations, location_value(uri, start, end));
            found_local = true;
        }

        /* Cross-file definitions from imported modules. */
        const std::string qualifier = qualifier_before(document->text, word_start);
        if (!qualifier.empty() &&
            !(qualifier == "std" || starts_with(qualifier, "std."))) {
            const std::string path = resolve_import_alias(document->text, qualifier);
            for (const index_entry &entry : index_.lookup(path + "." + word)) {
                const lsp_position start{entry.line, entry.character};
                const lsp_position end{entry.line,
                                       entry.character + utf16_length(word)};
                json::push(locations, location_value(entry.uri, start, end));
                found_local = true;
            }
        }
        (void)found_local;
        return locations;
    }

    void handle_notification(const std::string &method,
                             const value_t *params, bool &should_exit,
                             int &exit_code) {
        if (method == "initialized") {
            ensure_workspace_index();
            return;
        }
        if (method == "exit") {
            should_exit = true;
            exit_code = shutdown_received_ ? 0 : 1;
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = document_uri(params);
            if (uri.empty())
                return;
            const value_t *document = json::member(params, "textDocument");
            const int version =
                (int)json::int_value(json::member(document, "version"));
            lsp_document updated;
            const lsp_document *existing = documents_.find(uri);
            if (existing)
                updated = *existing;
            updated.uri = uri;
            updated.version = version;
            apply_content_changes(updated.text,
                                  json::member(params, "contentChanges"));
            documents_.put(updated);
            schedule_analysis(uri);
            return;
        }
        if (method == "textDocument/didOpen" ||
            method == "textDocument/didSave") {
            flush_pending_analysis();
            const std::string uri = document_uri(params);
            if (uri.empty())
                return;
            const value_t *document = json::member(params, "textDocument");
            std::string text = json::string_value(json::member(document, "text"));
            if (text.empty())
                text = json::string_value(json::member(params, "text"));
            int version = (int)json::int_value(json::member(document, "version"));
            if (method == "textDocument/didSave" && text.empty()) {
                const lsp_document *existing = documents_.find(uri);
                if (existing) {
                    text = existing->text;
                    version = existing->version;
                }
            }
            publish_diagnostics(uri, version, text);
            return;
        }
        if (method == "textDocument/didClose") {
            const std::string uri = document_uri(params);
            if (uri.empty())
                return;
            if (pending_analysis_ && pending_uri_ == uri)
                pending_analysis_ = false;
            documents_.erase(uri);
            value_t *diagnostics = json::array();
            value_t *notification = json::object();
            json::set(notification, "uri", json::string(uri));
            json::set(notification, "diagnostics", diagnostics);
            send_notification("textDocument/publishDiagnostics", notification);
        }
    }

    void handle_payload(const std::string &payload, bool &should_exit,
                        int &exit_code) {
        std::string error;
        value_t *root = json_parse_text(payload, error);
        if (!root) {
            std::fprintf(stderr, "skein lsp: invalid JSON: %s\n", error.c_str());
            send_error(json::null(), -32700, "Parse error");
            return;
        }

        const value_t *id = json::member(root, "id");
        const std::string method =
            json::string_value(json::member(root, "method"));
        const value_t *params = json::member(root, "params");

        if (method.empty()) {
            send_error(id, -32600, "Invalid Request");
            freeval(root);
            return;
        }

        if (method == "exit" || starts_with(method, "textDocument/did") ||
            method == "initialized" || starts_with(method, "$/")) {
            handle_notification(method, params, should_exit, exit_code);
            freeval(root);
            return;
        }

        flush_pending_analysis();
        value_t *result = nullptr;
        if (method == "initialize") {
            collect_workspace_folders(params);
            result = initialize_result();
        } else if (method == "shutdown") {
            shutdown_received_ = true;
            result = json::null();
        } else if (method == "textDocument/completion") {
            result = completion_result(params);
        } else if (method == "textDocument/hover") {
            result = hover_result(params);
        } else if (method == "textDocument/formatting" ||
                   method == "textDocument/rangeFormatting") {
            result = formatting_result(params);
        } else if (method == "textDocument/documentSymbol") {
            result = document_symbol_result(params);
        } else if (method == "textDocument/definition") {
            result = definition_result(params);
        } else {
            if (id)
                send_error(id, -32601, "Method not found");
            freeval(root);
            return;
        }

        if (id)
            send_response(id, result);
        else
            freeval(result);
        freeval(root);
    }

    lsp_options options_;
    FILE *output_;
    document_store documents_;
    std::map<std::string, std_entry> std_index_;
    workspace_index index_;
    std::vector<std::string> index_roots_;
    bool index_built_ = false;
    bool shutdown_received_ = false;
    int debounce_ms_ = 75;
    bool pending_analysis_ = false;
    std::string pending_uri_;
    std::chrono::steady_clock::time_point pending_deadline_;
};

int run_lsp_server(const lsp_options &options, FILE *input, FILE *output) {
    lsp_server_impl server(options, output);
    return server.run(input);
}

} // namespace lsp
} // namespace skein
