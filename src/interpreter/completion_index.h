#ifndef SKEIN_COMPLETION_INDEX_H
#define SKEIN_COMPLETION_INDEX_H

#include <map>
#include <string>
#include <vector>

struct value_t;

namespace skein {
namespace completion {

struct item {
    std::string label;
    int kind = 9;
    std::string detail;
    bool append_dot = false;
};

/* Data-only completion index shared by REPL and LSP. */
class index {
public:
    void clear();
    void add_top(item value);
    void add_member(const std::string &qualifier, item value);
    std::vector<item> complete(const std::string &prefix,
                               const std::string &qualifier) const;

private:
    std::vector<item> top_;
    std::map<std::string, std::vector<item>> members_;
};

/* Add language keywords and keyword details. */
void add_keywords(index &out);

/* Add `std` and all nested runtime std namespaces/members. */
void add_runtime_std(index &out, const value_t *root);

/* Fields/methods of a runtime struct value, or fields of the current enum
 * variant. */
std::vector<item> runtime_value_members(const value_t *value);

}  // namespace completion
}  // namespace skein

#endif
