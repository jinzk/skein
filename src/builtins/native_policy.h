#ifndef SKEIN_NATIVE_POLICY_H
#define SKEIN_NATIVE_POLICY_H

#include "diagnostics.h"
#include "runtime_state.h"
#include "analysis_mode.h"
#include "symbol.h"

#include <algorithm>
#include <cctype>
#include <string>

/* Native code is always allowed in the main script so existing FFI scripts
 * and statically registered modules keep working.  An imported third-party
 * module must opt in explicitly with SKEIN_ALLOW_NATIVE=1; setting the env
 * var to 0 disables native loading everywhere. */
inline bool skein_native_code_allowed() {
    std::string value;
    if (thread_env_get("SKEIN_ALLOW_NATIVE", value)) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return value != "0" && value != "false" && value != "off" &&
               value != "no";
    }
    return diag_current_module_name().empty();
}

inline const char *skein_native_policy_error() {
    return "native code loading is disabled; set SKEIN_ALLOW_NATIVE=1 to "
           "allow it";
}

/*
 * Shared gate for APIs that load native code at runtime (`std.native.load`,
 * `std.ffi.open`).  Keeps the check-only and policy diagnostics in one place.
 * `api` names the public entry point for the message.
 */
inline bool skein_native_load_allowed(const char *api) {
    if (g_check_only) {
        yyerror("%s is disabled in check-only mode", api);
        return false;
    }
    if (!skein_native_code_allowed()) {
        yyerror("%s", skein_native_policy_error());
        return false;
    }
    return true;
}

#endif
