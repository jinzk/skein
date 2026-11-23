#include "startup_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace skein {
namespace startup_profile {
namespace {

using clock = std::chrono::steady_clock;

struct span_record {
    std::string name;
    int depth;
    double duration;
};

bool read_enabled() {
    const char *value = std::getenv("SKEIN_STARTUP_STATS");
    if (!value || !*value)
        return false;
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "off") != 0 && std::strcmp(value, "no") != 0;
}

double elapsed_ms(clock::time_point end, clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

thread_local bool g_enabled = false;
thread_local bool g_started = false;
thread_local clock::time_point g_begin;
thread_local clock::time_point g_last;
thread_local int g_depth = 0;
thread_local std::vector<std::pair<std::string, double>> g_marks;
thread_local std::vector<span_record> g_spans;

} // namespace

bool enabled() { return g_enabled; }

void begin() {
    g_enabled = read_enabled();
    g_started = g_enabled;
    g_depth = 0;
    g_marks.clear();
    g_spans.clear();
    if (!g_enabled)
        return;
    g_begin = clock::now();
    g_last = g_begin;
}

void mark(const char *name) {
    if (!g_started)
        return;
    const clock::time_point now = clock::now();
    g_marks.push_back(std::make_pair(std::string(name), elapsed_ms(now, g_last)));
    g_last = now;
}

void finish() {
    if (!g_started)
        return;
    g_started = false;
    const double total = elapsed_ms(clock::now(), g_begin);
    std::fprintf(stderr, "[startup] total_ms=%.3f\n", total);
    for (const auto &entry : g_marks)
        std::fprintf(stderr, "[startup] %-18s %8.3f ms\n",
                     entry.first.c_str(), entry.second);
    if (!g_spans.empty()) {
        std::fprintf(stderr, "[startup] spans:\n");
        for (const auto &entry : g_spans) {
            const std::string indent(2 + entry.depth * 2, ' ');
            std::fprintf(stderr, "[startup] %s%s  %8.3f ms\n",
                         indent.c_str(), entry.name.c_str(), entry.duration);
        }
    }
    std::fflush(stderr);
}

span_scope::span_scope(const char *name) : active_(false), name_(name) {
    if (!g_started)
        return;
    active_ = true;
    started_ = clock::now();
    g_depth += 1;
}

span_scope::span_scope(const std::string &name) : span_scope(name.c_str()) {}

span_scope::~span_scope() {
    if (!active_)
        return;
    const clock::time_point now = clock::now();
    g_depth -= 1;
    g_spans.push_back(
        {name_, g_depth, elapsed_ms(now, started_)});
}

} // namespace startup_profile
} // namespace skein
