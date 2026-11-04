#include "runtime_state.h"

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

bool g_interactive = true;
bool g_show_prompt = false;
int g_package_loading = 0;
bool g_keep_going = false;
bool g_script_stop = false;
bool g_statement_invalid = false;
