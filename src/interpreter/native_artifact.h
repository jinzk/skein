#ifndef SKEIN_NATIVE_ARTIFACT_H
#define SKEIN_NATIVE_ARTIFACT_H

#include <string>

/* Compiler command selected by SKEIN_CXX / CXX, defaulting to "c++". */
std::string native_compiler_from_env();

/* Query the compiler version.  `available` is false when the compiler cannot
 * be started; this lets deployed applications load a verified artifact without
 * requiring a compiler to be installed. */
bool native_compiler_version(const std::string &compiler, std::string &version,
                             bool &available, std::string &error);

/* Write metadata beside a freshly built native artifact. */
bool write_native_artifact_metadata(const std::string &module_path,
                                    const std::string &version,
                                    const std::string &compiler,
                                    const std::string &compiler_version,
                                    const std::string &build_flags,
                                    const std::string &build_command_hash,
                                    const std::string &artifact,
                                    std::string &error);

/* Validate source hash, artifact hash and (when possible) compiler version.
 * Returns the artifact path on success. */
bool validate_native_artifact(const std::string &module_path,
                              const std::string &version,
                              const std::string &output,
                              std::string &artifact, std::string &error);

#endif
