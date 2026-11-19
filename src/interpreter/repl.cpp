#include "repl.h"

#include "platform.h"
#include "repl_ui.h"
#include "scopestack.h"
#include "source_encoding.h"
#include "structtype.h"
#include "symbol.h"
#include "unicode_ident.h"
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

extern int indent_count;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

namespace repl_ui = skein::repl_ui;

constexpr size_t MAX_HISTORY = 1000;

std::vector<std::string> history;
std::deque<std::string> pending_lines;
bool history_loaded = false;

std::string trim_copy(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace((unsigned char)text[begin]))
        ++begin;
    size_t end = text.size();
    while (end > begin && std::isspace((unsigned char)text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::string history_path() {
    std::string explicit_path;
    if (platform_get_env("SKEIN_HISTORY", explicit_path) &&
        !explicit_path.empty())
        return explicit_path;
#ifdef _WIN32
    const char *home_name = "USERPROFILE";
#else
    const char *home_name = "HOME";
#endif
    std::string home;
    if (!platform_get_env(home_name, home) || home.empty())
        return std::string();
    return home + "/.skein_history";
}

void history_add(const std::string &line) {
    if (trim_copy(line).empty())
        return;
    if (!history.empty() && history.back() == line)
        return;
    history.push_back(line);
    if (history.size() > MAX_HISTORY)
        history.erase(history.begin());
}

void history_load_once() {
    if (history_loaded)
        return;
    history_loaded = true;
    std::string path = history_path();
    if (path.empty() || !platform_file_exists(path))
        return;
    std::string data;
    std::string error;
    if (!platform_read_file(path, data, error))
        return;
    size_t start = 0;
    while (start <= data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string::npos)
            end = data.size();
        std::string line = data.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            history.push_back(line);
        if (end == data.size())
            break;
        start = end + 1;
    }
    if (history.size() > MAX_HISTORY)
        history.erase(history.begin(), history.end() - MAX_HISTORY);
}

bool starts_with(const std::string &text, const std::string &prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

std::string lowercase(std::string text) {
    for (char &c : text)
        c = (char)std::tolower((unsigned char)c);
    return text;
}

#ifdef _WIN32
bool enable_windows_vt();
#endif

bool terminal_supports_color() {
    if (!platform_stdin_is_tty() || !platform_stdout_is_tty())
        return false;
    std::string term;
    if (platform_get_env("TERM", term) && lowercase(term) == "dumb")
        return false;
#ifdef _WIN32
    return enable_windows_vt();
#else
    return true;
#endif
}

bool repl_color_enabled() {
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    std::string value;
    if (platform_get_env("NO_COLOR", value)) {
        cached = 0;
    } else if (platform_get_env("SKEIN_REPL_COLOR", value)) {
        value = lowercase(value);
        cached = (value == "always" || value == "1" || value == "true" ||
                  value == "on")
                     ? 1
                     : (value == "auto" && terminal_supports_color()) ? 1 : 0;
    } else {
        cached = terminal_supports_color() ? 1 : 0;
    }
    return cached != 0;
}

void write_stdout(const std::string &text) {
    std::fflush(stdout);
#ifdef _WIN32
    if (!text.empty())
        std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
#else
    const char *data = text.data();
    size_t remaining = text.size();
    while (remaining > 0) {
        ssize_t written = ::write(STDOUT_FILENO, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            break;
        }
        data += written;
        remaining -= (size_t)written;
    }
#endif
}

size_t previous_utf8(const std::string &text, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (((unsigned char)text[pos]) & 0xC0) == 0x80)
        --pos;
    return pos;
}

size_t next_utf8(const std::string &text, size_t pos) {
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           (((unsigned char)text[pos]) & 0xC0) == 0x80)
        ++pos;
    return pos;
}

size_t line_width(const std::string &text) {
    return unicode_ident::display_width(text.data(), text.size());
}

void redraw(const std::string &prompt, const std::string &line,
            size_t cursor) {
    const std::string rendered = repl_ui::render(line, repl_color_enabled());
    std::string out = "\r" + prompt + rendered + "\x1b[K";
    const size_t tail = line_width(line.substr(cursor));
    if (tail > 0)
        out += "\x1b[" + std::to_string(tail) + "D";
    write_stdout(out);
}

const std::vector<std::string> &repl_commands() {
    static const std::vector<std::string> commands = {
        ":help", ":quit", ":q", ":exit", ":load", ":history",
    };
    return commands;
}

void append_global_symbols(std::vector<std::string> &out) {
    for (const struct symbol *sym : active_scope_context().global_slots) {
        if (!sym || !sym->name || !sym->name[0] || sym->name[0] == '$')
            continue;
        if (std::strcmp(sym->name, "std") == 0)
            continue;
        out.emplace_back(sym->name);
    }
}

const value_t *std_child(const value_t *value, const std::string &name) {
    if (!value || value->type != T_dict)
        return nullptr;
    const dict_value_t *dict = (const dict_value_t *)value;
    for (const auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string)
            continue;
        const string_value_t *key = (const string_value_t *)entry.first;
        if (key->value && name == key->value)
            return entry.second;
    }
    return nullptr;
}

void append_std_children(const std::string &qualifier,
                         std::vector<std::string> &out) {
    std::string path;
    if (qualifier == "std")
        path.clear();
    else if (starts_with(qualifier, "std."))
        path = qualifier.substr(4);
    else
        return;

    const value_t *current = lookup_global_symbol("std");
    std::size_t pos = 0;
    while (pos < path.size()) {
        const std::size_t dot = path.find('.', pos);
        const std::string part =
            path.substr(pos, dot == std::string::npos ? std::string::npos
                                                       : dot - pos);
        current = std_child(current, part);
        if (!current)
            return;
        if (dot == std::string::npos)
            break;
        pos = dot + 1;
    }
    if (!current || current->type != T_dict)
        return;

    const dict_value_t *dict = (const dict_value_t *)current;
    for (const auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string)
            continue;
        const string_value_t *key = (const string_value_t *)entry.first;
        if (!key->value || !key->value[0] || key->value[0] == '$')
            continue;
        std::string name = key->value;
        if (entry.second && entry.second->type == T_dict)
            name += ".";
        out.push_back(std::move(name));
    }
}

void append_value_members(const std::string &qualifier,
                          std::vector<std::string> &out) {
    const value_t *value = lookup_global_symbol(qualifier.c_str());
    if (!value)
        return;
    if (value->type == T_struct) {
        const struct_value_t *structure = (const struct_value_t *)value;
        if (!structure->def)
            return;
        for (const field_def &field : structure->def->fields)
            out.push_back(field.name);
        for (const auto &method : structure->def->methods)
            out.push_back(method.first);
        return;
    }
    if (value->type == T_enum) {
        const enum_value_t *enumeration = (const enum_value_t *)value;
        if (!enumeration->def || enumeration->variant < 0 ||
            (size_t)enumeration->variant >= enumeration->def->variants.size())
            return;
        const variant_def &variant =
            enumeration->def->variants[(size_t)enumeration->variant];
        for (const field_def &field : variant.fields)
            out.push_back(field.name);
    }
}

std::vector<std::string> completion_candidates(const std::string &line,
                                               size_t cursor) {
    const repl_ui::completion_context context =
        repl_ui::completion_context_at(line, cursor);
    std::vector<std::string> out;
    if (context.suppressed)
        return out;
    if (context.command) {
        if (context.start == 0) {
            out = repl_commands();
        } else if (starts_with(line, ":load") &&
                   (line.size() == 5 ||
                    std::isspace((unsigned char)line[5]))) {
            out = repl_ui::path_completions(context.prefix);
        }
        return repl_ui::filter_and_sort(out, context.prefix);
    }
    if (context.qualifier.empty()) {
        const std::vector<std::string> &words = repl_ui::keywords();
        out.insert(out.end(), words.begin(), words.end());
        append_global_symbols(out);
        out.emplace_back("std.");
    } else if (context.qualifier == "std" ||
               starts_with(context.qualifier, "std.")) {
        append_std_children(context.qualifier, out);
    } else {
        append_value_members(context.qualifier, out);
    }
    return repl_ui::filter_and_sort(out, context.prefix);
}

void complete_line(std::string &line, size_t &cursor,
                   const std::string &prompt) {
    const repl_ui::completion_context context =
        repl_ui::completion_context_at(line, cursor);
    std::vector<std::string> candidates = completion_candidates(line, cursor);
    if (candidates.empty()) {
        if (!context.suppressed &&
            (!context.command || context.start == 0))
            write_stdout("\a");
        return;
    }
    if (candidates.size() == 1) {
        line.replace(context.start, cursor - context.start, candidates[0]);
        cursor = context.start + candidates[0].size();
        redraw(prompt, line, cursor);
        return;
    }
    const std::string prefix = repl_ui::common_prefix(candidates);
    if (prefix.size() > context.prefix.size()) {
        line.replace(context.start, cursor - context.start, prefix);
        cursor = context.start + prefix.size();
        redraw(prompt, line, cursor);
        return;
    }
    write_stdout("\r\n");
    for (const std::string &candidate : candidates)
        write_stdout("  " + candidate + "\n");
    redraw(prompt, line, cursor);
}

#ifndef _WIN32
int read_byte() {
    unsigned char c = 0;
    for (;;) {
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) {
            if (g_interrupted) return -2;
            continue;
        }
        return -1;
    }
}

struct raw_terminal {
    struct termios original;
    bool active = false;

    bool enable() {
        if (tcgetattr(STDIN_FILENO, &original) != 0)
            return false;
        struct termios raw = original;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
            return false;
        active = true;
        return true;
    }

    ~raw_terminal() {
        if (active)
            tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
};

repl_line_status read_edited_line(const std::string &prompt,
                                  bool continuation, std::string &line) {
    history_load_once();
    raw_terminal terminal;
    if (!terminal.enable()) {
        write_stdout(prompt);
        line.clear();
        int c;
        while ((c = std::fgetc(stdin)) != EOF) {
            if (c == '\n') return REPL_LINE;
            if (c != '\r') line.push_back((char)c);
        }
        return line.empty() ? REPL_EOF : REPL_LINE;
    }

    std::string current =
        continuation ? repl_ui::indentation(indent_count) : std::string();
    size_t cursor = current.size();
    size_t history_pos = history.size();
    std::string draft;
    write_stdout(prompt);

    for (;;) {
        int c = read_byte();
        if (c == -2 || g_interrupted) {
            g_interrupted = 1;
            write_stdout("^C\r\n");
            return REPL_INTERRUPTED;
        }
        if (c < 0) {
            write_stdout("\r\n");
            return current.empty() ? REPL_EOF : REPL_LINE;
        }
        if (c == '\r' || c == '\n') {
            write_stdout("\r\n");
            line = current;
            history_add(line);
            return REPL_LINE;
        }
        if (c == 3) {
            g_interrupted = 1;
            write_stdout("^C\r\n");
            return REPL_INTERRUPTED;
        }
        if (c == 4) {
            if (current.empty()) {
                write_stdout("\r\n");
                return REPL_EOF;
            }
            if (cursor < current.size()) {
                size_t end = next_utf8(current, cursor);
                current.erase(cursor, end - cursor);
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (c == 7) { /* Ctrl-G: cancel current line */
            current.clear();
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 1) { /* Ctrl-A */
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 5) { /* Ctrl-E */
            cursor = current.size();
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 11) { /* Ctrl-K */
            current.erase(cursor);
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 21) { /* Ctrl-U */
            current.erase(0, cursor);
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 23) { /* Ctrl-W */
            size_t begin = previous_utf8(current, cursor);
            while (begin > 0 &&
                   std::isspace((unsigned char)current[begin - 1]))
                begin = previous_utf8(current, begin);
            size_t word = begin;
            while (word > 0 &&
                   !std::isspace((unsigned char)current[word - 1]))
                word = previous_utf8(current, word);
            current.erase(word, cursor - word);
            cursor = word;
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 12) { /* Ctrl-L */
            write_stdout("\x1b[2J\x1b[H");
            redraw(prompt, current, cursor);
            continue;
        }
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                size_t begin = previous_utf8(current, cursor);
                current.erase(begin, cursor - begin);
                cursor = begin;
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (c == 27) {
            int a = read_byte();
            if (a != '[' && a != 'O')
                continue;
            int b = read_byte();
            if (b == 'A') { /* up */
                if (history.empty()) continue;
                if (history_pos == history.size()) draft = current;
                if (history_pos > 0) --history_pos;
                current = history[history_pos];
                cursor = current.size();
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == 'B') { /* down */
                if (history_pos >= history.size()) continue;
                ++history_pos;
                current = (history_pos == history.size()) ? draft
                                                          : history[history_pos];
                cursor = current.size();
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == 'C') {
                cursor = next_utf8(current, cursor);
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == 'D') {
                cursor = previous_utf8(current, cursor);
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == 'H' || b == '1') {
                if (b == '1') read_byte(); /* tilde */
                cursor = 0;
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == 'F' || b == '4') {
                if (b == '4') read_byte(); /* tilde */
                cursor = current.size();
                redraw(prompt, current, cursor);
                continue;
            }
            if (b == '3') { /* delete */
                read_byte(); /* tilde */
                if (cursor < current.size()) {
                    size_t end = next_utf8(current, cursor);
                    current.erase(cursor, end - cursor);
                    redraw(prompt, current, cursor);
                }
                continue;
            }
            continue;
        }
        if (c == '\t') {
            complete_line(current, cursor, prompt);
            continue;
        }
        if (c >= 32) {
            if (c == '}' || c == ')' || c == ']')
                cursor = repl_ui::dedent_for_closing(current, cursor,
                                                     (char)c);
            current.insert(cursor, 1, (char)c);
            cursor += 1;
            redraw(prompt, current, cursor);
        }
    }
}
#endif

#ifdef _WIN32
bool enable_windows_vt() {
    static int state = -1;
    if (state >= 0)
        return state != 0;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) {
        state = 0;
        return false;
    }
    state = SetConsoleMode(handle,
                           mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
                ? 1
                : 0;
    return state != 0;
}

struct console_input_guard {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD original = 0;
    bool active = false;

    bool enable() {
        handle = GetStdHandle(STD_INPUT_HANDLE);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetConsoleMode(handle, &original))
            return false;
        const DWORD mode =
            original &
            ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        if (!SetConsoleMode(handle, mode))
            return false;
        active = true;
        return true;
    }

    ~console_input_guard() {
        if (active)
            SetConsoleMode(handle, original);
    }
};

std::string utf8_from_wchars(const wchar_t *data, int length) {
    if (length <= 0)
        return std::string();
    const int count = WideCharToMultiByte(CP_UTF8, 0, data, length, nullptr,
                                          0, nullptr, nullptr);
    if (count <= 0)
        return std::string();
    std::string out((size_t)count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, length, out.data(), count, nullptr,
                        nullptr);
    return out;
}

repl_line_status read_edited_line(const std::string &prompt,
                                  bool continuation, std::string &line) {
    history_load_once();
    console_input_guard terminal;
    if (!enable_windows_vt() || !terminal.enable()) {
        write_stdout(prompt);
        line.clear();
        int c;
        while ((c = std::fgetc(stdin)) != EOF) {
            if (c == '\n') return REPL_LINE;
            if (c != '\r') line.push_back((char)c);
        }
        return line.empty() ? REPL_EOF : REPL_LINE;
    }

    std::string current =
        continuation ? repl_ui::indentation(indent_count) : std::string();
    size_t cursor = current.size();
    size_t history_pos = history.size();
    std::string draft;
    wchar_t high_surrogate = 0;
    write_stdout(prompt);

    auto append_char = [&](wchar_t ch) {
        wchar_t buffer[2] = {ch, 0};
        int length = 1;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            high_surrogate = ch;
            return;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF && high_surrogate) {
            buffer[0] = high_surrogate;
            buffer[1] = ch;
            length = 2;
            high_surrogate = 0;
        } else {
            high_surrogate = 0;
        }
        const std::string bytes = utf8_from_wchars(buffer, length);
        if (!bytes.empty()) {
            current.insert(cursor, bytes);
            cursor += bytes.size();
        }
    };

    for (;;) {
        if (g_interrupted) {
            g_interrupted = 1;
            write_stdout("^C\r\n");
            return REPL_INTERRUPTED;
        }
        INPUT_RECORD record;
        DWORD count = 0;
        if (!ReadConsoleInputW(terminal.handle, &record, 1, &count) ||
            count == 0) {
            if (g_interrupted) {
                g_interrupted = 1;
                write_stdout("^C\r\n");
                return REPL_INTERRUPTED;
            }
            write_stdout("\r\n");
            return current.empty() ? REPL_EOF : REPL_LINE;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
            continue;
        const KEY_EVENT_RECORD &key = record.Event.KeyEvent;
        const bool ctrl =
            (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) !=
            0;

        if (ctrl && key.wVirtualKeyCode == 'C') {
            g_interrupted = 1;
            write_stdout("^C\r\n");
            return REPL_INTERRUPTED;
        }
        if (key.wVirtualKeyCode == VK_LEFT) {
            cursor = previous_utf8(current, cursor);
            redraw(prompt, current, cursor);
            continue;
        }
        if (key.wVirtualKeyCode == VK_RIGHT) {
            cursor = next_utf8(current, cursor);
            redraw(prompt, current, cursor);
            continue;
        }
        if (key.wVirtualKeyCode == VK_HOME) {
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (key.wVirtualKeyCode == VK_END) {
            cursor = current.size();
            redraw(prompt, current, cursor);
            continue;
        }
        if (key.wVirtualKeyCode == VK_UP) {
            if (!history.empty()) {
                if (history_pos == history.size())
                    draft = current;
                if (history_pos > 0)
                    --history_pos;
                current = history[history_pos];
                cursor = current.size();
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (key.wVirtualKeyCode == VK_DOWN) {
            if (history_pos < history.size()) {
                ++history_pos;
                current = history_pos == history.size() ? draft
                                                        : history[history_pos];
                cursor = current.size();
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (key.wVirtualKeyCode == VK_DELETE) {
            if (cursor < current.size()) {
                current.erase(cursor, next_utf8(current, cursor) - cursor);
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (key.wVirtualKeyCode == VK_RETURN) {
            write_stdout("\r\n");
            line = current;
            history_add(line);
            return REPL_LINE;
        }
        if (key.wVirtualKeyCode == VK_TAB) {
            complete_line(current, cursor, prompt);
            continue;
        }
        if (key.wVirtualKeyCode == VK_BACK) {
            if (cursor > 0) {
                const size_t begin = previous_utf8(current, cursor);
                current.erase(begin, cursor - begin);
                cursor = begin;
                redraw(prompt, current, cursor);
            }
            continue;
        }

        const wchar_t ch = key.uChar.UnicodeChar;
        if (ch == 0)
            continue;
        if (ch == 3) {
            g_interrupted = 1;
            write_stdout("^C\r\n");
            return REPL_INTERRUPTED;
        }
        if (ch == 4) {
            if (current.empty()) {
                write_stdout("\r\n");
                return REPL_EOF;
            }
            if (cursor < current.size()) {
                current.erase(cursor, next_utf8(current, cursor) - cursor);
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (ch == 7) {
            current.clear();
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 1) {
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 5) {
            cursor = current.size();
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 11) {
            current.erase(cursor);
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 21) {
            current.erase(0, cursor);
            cursor = 0;
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 23) {
            size_t begin = previous_utf8(current, cursor);
            while (begin > 0 &&
                   std::isspace((unsigned char)current[begin - 1]))
                begin = previous_utf8(current, begin);
            size_t word = begin;
            while (word > 0 &&
                   !std::isspace((unsigned char)current[word - 1]))
                word = previous_utf8(current, word);
            current.erase(word, cursor - word);
            cursor = word;
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 12) {
            write_stdout("\x1b[2J\x1b[H");
            redraw(prompt, current, cursor);
            continue;
        }
        if (ch == 8 || ch == 127) {
            if (cursor > 0) {
                const size_t begin = previous_utf8(current, cursor);
                current.erase(begin, cursor - begin);
                cursor = begin;
                redraw(prompt, current, cursor);
            }
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            write_stdout("\r\n");
            line = current;
            history_add(line);
            return REPL_LINE;
        }
        if (ch == '\t') {
            complete_line(current, cursor, prompt);
            continue;
        }
        if (ch >= 32) {
            if (ch == '}' || ch == ')' || ch == ']')
                cursor = repl_ui::dedent_for_closing(current, cursor,
                                                     (char)ch);
            append_char(ch);
            redraw(prompt, current, cursor);
        }
    }
}
#endif

repl_line_status read_plain_line(std::string &line) {
    line.clear();
    for (;;) {
        int c = std::fgetc(stdin);
        if (c == EOF)
            return line.empty() ? REPL_EOF : REPL_LINE;
        if (c == '\n')
            return REPL_LINE;
        if (c == '\r')
            continue;
        if (c == 8 || c == 127) {
            if (!line.empty())
                line.pop_back();
            continue;
        }
        line.push_back((char)c);
    }
}

bool handle_command(const std::string &line) {
    std::string command = trim_copy(line);
    if (command == ":help" || command == ":h") {
        std::printf(
            ":help              show this help\n"
            ":quit / :q         exit\n"
            ":load FILE         execute a file in this session\n"
            ":history           show command history\n");
        return true;
    }
    if (command == ":quit" || command == ":q" || command == ":exit") {
        pending_lines.clear();
        return false;
    }
    if (command == ":history") {
        for (size_t i = 0; i < history.size(); ++i)
            std::printf("%4zu  %s\n", i + 1, history[i].c_str());
        return true;
    }
    if (command.rfind(":load", 0) == 0 &&
        (command.size() == 5 ||
         std::isspace((unsigned char)command[5]))) {
        std::string path = trim_copy(command.substr(5));
        if (path.empty()) {
            std::fprintf(stderr, ":load requires a file path\n");
            return true;
        }
        std::string data;
        std::string error;
        if (!load_source_file(path, source_encoding_get(), data, error)) {
            std::fprintf(stderr, ":load %s: %s\n", path.c_str(),
                         error.c_str());
            return true;
        }
        size_t start = 0;
        while (start <= data.size()) {
            size_t end = data.find('\n', start);
            if (end == std::string::npos)
                end = data.size();
            std::string loaded = data.substr(start, end - start);
            if (!loaded.empty() && loaded.back() == '\r')
                loaded.pop_back();
            pending_lines.push_back(loaded);
            if (end == data.size())
                break;
            start = end + 1;
        }
        return true;
    }
    std::fprintf(stderr, "unknown command: %s (try :help)\n",
                 command.c_str());
    return true;
}

} // namespace

repl_line_status repl_read_line(std::string &line, bool continuation) {
    for (;;) {
        if (!pending_lines.empty()) {
            line = pending_lines.front();
            pending_lines.pop_front();
            return REPL_LINE;
        }

        std::string prompt;
        if (g_show_prompt)
            prompt = continuation ? "...> " : "> ";

        repl_line_status status;
#ifdef _WIN32
        if (g_show_prompt && platform_stdin_is_tty() &&
            platform_stdout_is_tty()) {
            status = read_edited_line(prompt, continuation, line);
        } else {
            if (g_show_prompt) {
                std::printf("%s", prompt.c_str());
                std::fflush(stdout);
            }
            status = read_plain_line(line);
        }
#else
        if (g_show_prompt && platform_stdin_is_tty() &&
            platform_stdout_is_tty())
            status = read_edited_line(prompt, continuation, line);
        else {
            if (g_show_prompt) {
                std::printf("%s", prompt.c_str());
                std::fflush(stdout);
            }
            status = read_plain_line(line);
        }
#endif
        if (status != REPL_LINE)
            return status;
        if (!line.empty() && line[0] == ':') {
            if (!handle_command(line)) {
                line.clear();
                return REPL_EOF;
            }
            continue;
        }
        history_add(line);
        return REPL_LINE;
    }
}

void repl_history_load() {
    history_load_once();
}

void repl_history_save() {
    std::string path = history_path();
    if (path.empty() || history.empty())
        return;
    std::string data;
    for (size_t i = 0; i < history.size(); ++i) {
        data += history[i];
        data += '\n';
    }
    std::string error;
    platform_write_file(path, data, false, error);
}

void repl_set_last_result(struct value_t *value) {
    if (!value)
        return;
    const char *names[] = {"ans", "_"};
    for (const char *name : names) {
        struct symbol *sym = lookupsym(name);
        if (!sym) {
            sym = newsym(name);
            savesym(sym);
        }
        if (sym->is_const)
            continue;
        freeval(sym->val);
        sym->val = dupval(value);
    }
}
