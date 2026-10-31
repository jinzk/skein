#include "repl.h"

#include "platform.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

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
    const char *explicit_path = std::getenv("CALC_HISTORY");
    if (explicit_path && *explicit_path)
        return explicit_path;
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif
    if (!home || !*home)
        return std::string();
    return std::string(home) + "/.calc_history";
}

void history_add(const std::string &line) {
    if (line.empty())
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

#ifndef _WIN32
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

void write_stdout(const std::string &text) {
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
}

void redraw(const std::string &prompt, const std::string &line,
            size_t cursor) {
    std::string out = "\r" + prompt + line + "\x1b[K";
    size_t tail = line.size() - cursor;
    if (tail > 0)
        out += "\x1b[" + std::to_string(tail) + "D";
    write_stdout(out);
}

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
    (void)continuation;
    history_load_once();
    raw_terminal terminal;
    if (!terminal.enable()) {
        line.clear();
        int c;
        while ((c = std::fgetc(stdin)) != EOF) {
            if (c == '\n') return REPL_LINE;
            if (c != '\r') line.push_back((char)c);
        }
        return line.empty() ? REPL_EOF : REPL_LINE;
    }

    std::string current;
    size_t cursor = 0;
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
        if (c >= 32 || c == '\t') {
            current.insert(cursor, 1, (char)c);
            cursor += 1;
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
        if (!platform_read_file(path, data, error)) {
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
        if (g_show_prompt)
            std::printf("%s", prompt.c_str());
        std::fflush(stdout);
        status = read_plain_line(line);
#else
        if (g_show_prompt && platform_stdin_is_tty())
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
