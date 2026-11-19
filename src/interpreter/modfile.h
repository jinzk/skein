#ifndef SKEIN_MODFILE_H
#define SKEIN_MODFILE_H

#include <string>
#include <vector>

namespace skein {
namespace modfile {

struct require_spec {
    std::string path;
    std::string version;
};

struct mod_file {
    std::string module;
    std::vector<require_spec> requirements;
};

/* Nearest ancestor containing skein.mod (preferred) or legacy .skeinroot.
 * Returns an empty string when neither marker exists. */
std::string find_root(const std::string &start);
bool exists(const std::string &root);

/* A missing skein.mod is an empty file, not an error. */
bool load(const std::string &root, mod_file &out, std::string &error);
bool save(const std::string &root, const mod_file &mod, std::string &error);
bool upsert_require(const std::string &root, const std::string &path,
                    const std::string &version, std::string &error);

bool add_sum(const std::string &root, const std::string &path,
             const std::string &version, const std::string &hash,
             std::string &error);
bool find_sum(const std::string &root, const std::string &path,
              const std::string &version, std::string &hash,
              std::string &error);
bool verify_cached_module(const std::string &root, const std::string &path,
                          const std::string &version, std::string &error);

bool is_valid_module_path(const std::string &path);
bool is_valid_version(const std::string &version);
bool is_valid_package_subpath(const std::string &subpath);

std::string cache_dir();
std::string cache_path(const std::string &path, const std::string &version);
bool dirhash(const std::string &directory, std::string &hash,
             std::string &error);

}  // namespace modfile
}  // namespace skein

#endif
