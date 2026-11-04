#ifndef SKEIN_FFI_BINDGEN_H
#define SKEIN_FFI_BINDGEN_H

#include <string>
#include <vector>

namespace skein {

/* Run `skein --ffi-gen ...`.  Returns a process exit code and fills `error`
 * on failure.  Kept separate from main so it can become a standalone tool. */
int run_ffi_gen(const std::vector<std::string> &args, std::string &error);

} // namespace skein

#endif
