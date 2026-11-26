#include "repl_ui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace skein {
namespace repl_ui {
namespace {

bool ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool ident_continue(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

bool digit(unsigned char c) { return c >= '0' && c <= '9'; }

const std::vector<std::string> &keyword_list() {
    static const std::vector<std::string> words = {
        "if", "else", "while", "do", "for", "in", "switch", "case",
        "default", "match", "let", "const", "func", "struct", "enum",
        "proto", "impl", "macro", "extern", "package", "module", "import",
        "include", "export", "return", "break", "continue", "goto", "yield",
        "defer", "launch", "wait", "await", "try", "on", "error", "recover", "fail",
        "pass", "is", "as", "true", "false", "null",
    };
    return words;
}

const std::unordered_set<std::string> &keyword_set() {
    static const std::unordered_set<std::string> words(keyword_list().begin(),
                                                       keyword_list().end());
    return words;
}

std::size_t display_width(const std::string &text) {
    std::size_t width = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80)
            ++width;
    }
    return width;
}

const char *color_for(token_kind kind) {
    switch (kind) {
        case token_kind::keyword:
        case token_kind::command:
            return "\x1b[36m";
        case token_kind::string:
            return "\x1b[32m";
        case token_kind::comment:
            return "\x1b[90m";
        case token_kind::number:
            return "\x1b[33m";
        case token_kind::error:
            return "\x1b[31m";
        case token_kind::plain:
            break;
    }
    return "";
}

std::size_t scan_quoted(const std::string &line, std::size_t start,
                        char quote) {
    std::size_t pos = start + 1;
    while (pos < line.size()) {
        if (line[pos] == '\\' && pos + 1 < line.size()) {
            pos += 2;
            continue;
        }
        if (line[pos] == quote)
            return pos + 1;
        ++pos;
    }
    return line.size();
}

}  // namespace

const std::vector<std::string> &keywords() { return keyword_list(); }

std::vector<token_span> tokenize(const std::string &line) {
    std::vector<token_span> spans;
    if (line.empty())
        return spans;
    if (line[0] == ':') {
        spans.push_back({0, line.size(), token_kind::command});
        return spans;
    }

    std::size_t pos = 0;
    while (pos < line.size()) {
        const unsigned char c = (unsigned char)line[pos];
        if (std::isspace(c)) {
            ++pos;
            continue;
        }
        if (c == '/' && pos + 1 < line.size() && line[pos + 1] == '/') {
            spans.push_back({pos, line.size(), token_kind::comment});
            break;
        }
        if (c == '"' || c == '\'') {
            const std::size_t end = scan_quoted(line, pos, (char)c);
            const bool closed = end <= line.size() && end > pos + 1 &&
                                line[end - 1] == (char)c;
            spans.push_back(
                {pos, end, closed ? token_kind::string : token_kind::error});
            pos = end;
            continue;
        }
        if (digit(c) || (c == '.' && pos + 1 < line.size() &&
                         digit((unsigned char)line[pos + 1]))) {
            const std::size_t start = pos++;
            while (pos < line.size()) {
                const unsigned char n = (unsigned char)line[pos];
                if (!(std::isalnum(n) || n == '_' || n == '.'))
                    break;
                ++pos;
            }
            spans.push_back({start, pos, token_kind::number});
            continue;
        }
        if (ident_start(c)) {
            const std::size_t start = pos++;
            while (pos < line.size() &&
                   ident_continue((unsigned char)line[pos]))
                ++pos;
            const std::string word = line.substr(start, pos - start);
            spans.push_back(
                {start, pos, keyword_set().count(word) ? token_kind::keyword
                                                      : token_kind::plain});
            continue;
        }
        ++pos;
    }
    return spans;
}

std::string render(const std::string &line, bool color) {
    if (!color || line.empty())
        return line;
    const std::vector<token_span> spans = tokenize(line);
    std::string out;
    out.reserve(line.size() + spans.size() * 5);
    std::size_t pos = 0;
    for (const token_span &span : spans) {
        if (span.start > pos)
            out.append(line, pos, span.start - pos);
        const char *code = color_for(span.kind);
        if (*code)
            out += code;
        out.append(line, span.start, span.end - span.start);
        if (*code)
            out += "\x1b[0m";
        pos = span.end;
    }
    if (pos < line.size())
        out.append(line, pos, line.size() - pos);
    return out;
}

completion_context completion_context_at(const std::string &line,
                                         std::size_t cursor) {
    completion_context context;
    if (cursor > line.size())
        cursor = line.size();
    if (!line.empty() && line[0] == ':') {
        context.command = true;
        const std::size_t space = line.find_first_of(" \t");
        if (space == std::string::npos || cursor <= space) {
            context.start = 0;
            context.prefix = line.substr(0, cursor);
        } else {
            std::size_t start = cursor;
            while (start > space &&
                   !std::isspace((unsigned char)line[start - 1]))
                --start;
            context.start = start;
            context.prefix = line.substr(start, cursor - start);
        }
        return context;
    }

    std::size_t start = cursor;
    while (start > 0 && ident_continue((unsigned char)line[start - 1]))
        --start;
    context.start = start;
    context.prefix = line.substr(start, cursor - start);

    if (start > 0 && line[start - 1] == '.') {
        std::size_t qualifier_start = start - 1;
        while (qualifier_start > 0) {
            const unsigned char c =
                (unsigned char)line[qualifier_start - 1];
            if (!(ident_continue(c) || c == '.'))
                break;
            --qualifier_start;
        }
        context.qualifier =
            line.substr(qualifier_start, start - 1 - qualifier_start);
    }

    for (const token_span &span : tokenize(line)) {
        if (span.kind == token_kind::comment || span.kind == token_kind::error)
            context.suppressed =
                context.suppressed || (cursor >= span.start && cursor <= span.end);
        else if (span.kind == token_kind::string)
            context.suppressed =
                context.suppressed || (cursor > span.start && cursor < span.end);
    }
    return context;
}

std::vector<std::string> filter_and_sort(
    const std::vector<std::string> &candidates, const std::string &prefix) {
    std::vector<std::string> out;
    for (const std::string &candidate : candidates) {
        if (candidate.rfind(prefix, 0) == 0)
            out.push_back(candidate);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string common_prefix(const std::vector<std::string> &candidates) {
    if (candidates.empty())
        return std::string();
    std::string prefix = candidates.front();
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const std::string &candidate = candidates[i];
        std::size_t length = 0;
        while (length < prefix.size() && length < candidate.size() &&
               prefix[length] == candidate[length])
            ++length;
        prefix.resize(length);
        if (prefix.empty())
            break;
    }
    /* Avoid returning an incomplete UTF-8 sequence. */
    if (!prefix.empty()) {
        std::size_t length = prefix.size();
        const unsigned char last = (unsigned char)prefix[length - 1];
        if (last >= 0x80) {
            std::size_t start = length - 1;
            while (start > 0 &&
                   (((unsigned char)prefix[start]) & 0xC0) == 0x80)
                --start;
            const unsigned char lead = (unsigned char)prefix[start];
            std::size_t needed = 1;
            if ((lead & 0xE0) == 0xC0)
                needed = 2;
            else if ((lead & 0xF0) == 0xE0)
                needed = 3;
            else if ((lead & 0xF8) == 0xF0)
                needed = 4;
            if (length - start < needed)
                length = start;
            prefix.resize(length);
        }
    }
    return prefix;
}

completion_popup_view format_completion_popup(
    const std::vector<std::string> &candidates, std::size_t width,
    std::size_t max_rows, std::size_t selected) {
    completion_popup_view view;
    if (candidates.empty() || max_rows == 0)
        return view;

    std::size_t content_width = width >= 8 ? width - 4 : 24;
    if (content_width < 12)
        content_width = 12;

    std::size_t cell_width = 0;
    for (const std::string &candidate : candidates)
        cell_width = std::max(cell_width, display_width(candidate) + 2);

    auto choose_columns = [&](std::size_t label_width) {
        const std::size_t per_cell = label_width + 2;
        return std::max<std::size_t>(1, content_width / per_cell);
    };

    std::size_t columns = choose_columns(cell_width);
    std::size_t capacity = columns * max_rows;
    std::size_t visible = std::min(candidates.size(), capacity);
    bool truncated = visible < candidates.size();
    if (truncated) {
        const std::string more =
            "... +" + std::to_string(candidates.size() - visible + 1) + " more";
        cell_width = std::max(cell_width, display_width(more) + 2);
        columns = choose_columns(cell_width);
        capacity = columns * max_rows;
        visible = std::min(candidates.size(), capacity > 1 ? capacity - 1 : 1);
        truncated = visible < candidates.size();
    }
    if (visible == 0)
        return view;

    view.visible_count = visible;
    if (selected >= visible)
        selected = visible - 1;

    const std::string border =
        "+" + std::string(content_width + 2, '-') + "+";
    std::string heading =
        "candidates (" + std::to_string(candidates.size()) + ")";
    if (display_width(heading) > content_width)
        heading = "candidates";
    std::string header = heading;
    if (display_width(header) < content_width)
        header += std::string(content_width - display_width(header), ' ');

    view.rows.push_back(border);
    view.rows.push_back("| " + header + " |");
    view.rows.push_back(border);

    auto append_cell = [&](std::string &line, const std::string &text,
                           bool last) {
        line += text;
        const std::size_t used = display_width(text);
        if (!last && used < cell_width)
            line += std::string(cell_width - used, ' ');
        if (!last)
            line += "  ";
    };

    for (std::size_t row = 0; row * columns < visible; ++row) {
        std::string line;
        for (std::size_t column = 0; column < columns; ++column) {
            const std::size_t index = row * columns + column;
            if (index >= visible)
                break;
            append_cell(line,
                        std::string(index == selected ? "> " : "  ") +
                            candidates[index],
                        column + 1 == columns || index + 1 == visible);
        }
        if (truncated && (visible % columns == 0
                              ? row + 1 == visible / columns
                              : row == visible / columns)) {
            const std::string more =
                "... +" + std::to_string(candidates.size() - visible) +
                " more";
            append_cell(line, more, true);
        }
        const std::size_t used = display_width(line);
        if (used < content_width)
            line += std::string(content_width - used, ' ');
        view.rows.push_back("| " + line + " |");
    }
    view.rows.push_back(border);
    return view;
}

std::string indentation(int level, int width) {
    if (level <= 0)
        return std::string();
    if (width <= 0)
        width = 2;
    if (level > 40)
        level = 40;
    return std::string((std::size_t)level * (std::size_t)width, ' ');
}

std::size_t dedent_for_closing(std::string &line, std::size_t cursor,
                               char closing, int width) {
    if (closing != '}' && closing != ')' && closing != ']')
        return cursor;
    if (cursor > line.size())
        cursor = line.size();
    for (std::size_t i = 0; i < cursor; ++i) {
        if (line[i] != ' ' && line[i] != '\t')
            return cursor;
    }
    if (width <= 0)
        width = 2;
    std::size_t remove = 0;
    while (remove < (std::size_t)width && remove < cursor &&
           line[remove] == ' ')
        ++remove;
    if (remove == 0)
        return cursor;
    line.erase(0, remove);
    return cursor - remove;
}

bool continues_line(const std::string &line) {
    std::size_t end = line.size();
    while (end > 0 && std::isspace((unsigned char)line[end - 1]))
        --end;
    if (end == 0)
        return false;
    const char last = line[end - 1];
    return last == '+' || last == '-' || last == '*' || last == '/' ||
           last == '%' || last == '=' || last == ',' || last == '&' ||
           last == '|' || last == '^' || last == '?' || last == ':' ||
           last == '.' || last == '<' || last == '>';
}

std::vector<std::string> path_completions(const std::string &prefix) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const std::size_t separator = prefix.find_last_of("/\\");
    const std::string directory_text =
        separator == std::string::npos ? std::string()
                                       : prefix.substr(0, separator + 1);
    const std::string base_text =
        separator == std::string::npos
            ? std::string(".")
            : (separator == 0 ? prefix.substr(0, 1)
                              : prefix.substr(0, separator));
    const std::string leaf =
        separator == std::string::npos ? prefix : prefix.substr(separator + 1);

    std::error_code ec;
    fs::directory_iterator it(fs::u8path(base_text), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.rfind(leaf, 0) != 0)
            continue;
        std::string candidate = directory_text + name;
        std::error_code kind_ec;
        if (it->is_directory(kind_ec))
            candidate += "/";
        out.push_back(std::move(candidate));
    }
    return filter_and_sort(out, prefix);
}

}  // namespace repl_ui
}  // namespace skein
