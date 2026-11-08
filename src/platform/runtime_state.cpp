#include "runtime_state.h"

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

thread_local bool g_interactive = true;
thread_local bool g_show_prompt = false;
thread_local int g_package_loading = 0;
thread_local bool g_keep_going = false;
thread_local bool g_script_stop = false;
thread_local bool g_statement_invalid = false;
