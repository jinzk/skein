#ifndef SKEIN_FFI_CHECK_H
#define SKEIN_FFI_CHECK_H

#include <string>

namespace ffi_check {

/* Validate a manifest and resolve every declared symbol without running a
 * script.  Returns a process exit code and fills `error` on failure. */
int run_check(const std::string &library_path,
              const std::string &manifest_path, std::string &error);

} // namespace ffi_check

#endif
