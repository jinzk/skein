#ifndef SKEIN_LSP_WORKSPACE_INDEX_H
#define SKEIN_LSP_WORKSPACE_INDEX_H

#include <string>
#include <vector>

namespace skein {
namespace lsp {

struct index_entry {
    std::string qualified_name; // 对外可见的限定名，例如 "math.add"
    std::string uri;            // 定义所在文件的 file:// URI
    int line = 0;               // 0-based line
    int character = 0;          // 0-based UTF-16 code unit
    std::string type;
    bool callable = false;
};

/**
 * Index of symbols exported by modules under the workspace roots.
 *
 * The index is built by scanning module manifests (module.skein) and the
 * package files they export.  All file analysis goes through the check-only
 * front end so opening a workspace never executes target code.
 */
class workspace_index {
public:
    /** Rebuild the index from the given roots (package dirs / workspace folders). */
    void rebuild(const std::vector<std::string> &roots);

    /** All entries whose qualified name starts with the given prefix. */
    std::vector<index_entry> complete(const std::string &qualified_prefix) const;

    /** Entries with an exact qualified name. */
    std::vector<index_entry> lookup(const std::string &qualified_name) const;

    /** All indexed entries (used to derive module-name completions). */
    const std::vector<index_entry> &all() const { return entries_; }

private:
    std::vector<index_entry> entries_;
};

} // namespace lsp
} // namespace skein

#endif
