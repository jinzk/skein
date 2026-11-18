#include "analysis_mode.h"

thread_local bool g_check_only = false;

check_only_guard::check_only_guard() : previous(g_check_only) {
    g_check_only = true;
}

check_only_guard::~check_only_guard() { g_check_only = previous; }
