#include "completion_index.h"

#include "repl_ui.h"
#include "structtype.h"
#include "valuetype.h"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skein {
namespace completion {
namespace {

const char *keyword_detail(const std::string &word) {
    static const std::map<std::string, const char *> details = {
        {"if", "conditional"},        {"else", "conditional alternative"},
        {"while", "while loop"},      {"do", "do/while loop"},
        {"for", "for loop"},          {"in", "iteration"},
        {"switch", "switch"},         {"case", "switch case"},
        {"default", "switch default"},{"match", "pattern match"},
        {"let", "binding"},           {"const", "constant binding"},
        {"func", "function"},         {"struct", "struct type"},
        {"enum", "enum type"},        {"proto", "proto"},
        {"impl", "implementation"},   {"macro", "AST macro declaration"},
        {"extern", "external declaration"},
        {"package", "package"},       {"module", "module"},
        {"import", "import"},         {"include", "include"},
        {"export", "export"},         {"return", "return"},
        {"break", "break"},           {"continue", "continue"},
        {"goto", "jump to a label"},
        {"yield", "yield"},           {"defer", "defer"},
        {"launch", "launch"},         {"wait", "wait for a task"},
        {"await", "launch and wait"},
        {"try", "exception handling"},
        {"on", "exception handler"},  {"error", "exception handling"},
        {"recover", "exception handling"}, {"fail", "failure"},
        {"pass", "no-op"},            {"is", "type test"},
        {"as", "cast"},
        {"true", "boolean literal"},  {"false", "boolean literal"},
        {"null", "null literal"},
    };
    auto it = details.find(word);
    return it == details.end() ? "keyword" : it->second;
}

void add_std_node(index &out, const value_t *value, const std::string &path) {
    if (!value || value->type != T_dict)
        return;
    const dict_value_t *dict = (const dict_value_t *)value;
    for (const auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string)
            continue;
        const string_value_t *key = (const string_value_t *)entry.first;
        if (!key->value || !key->value[0] || key->value[0] == '$')
            continue;
        const std::string name = key->value;
        const std::string child_path = path + "." + name;
        const bool namespace_child =
            entry.second && entry.second->type == T_dict;
        item child;
        child.label = name;
        child.kind = namespace_child
                         ? 9
                         : (entry.second && entry.second->type == T_builtin ? 3
                                                                            : 6);
        child.detail = child_path;
        child.append_dot = namespace_child;
        out.add_member(path, std::move(child));
        if (namespace_child)
            add_std_node(out, entry.second, child_path);
    }
}

}  // namespace

void index::clear() {
    top_.clear();
    members_.clear();
}

void index::add_top(item value) { top_.push_back(std::move(value)); }

void index::add_member(const std::string &qualifier, item value) {
    members_[qualifier].push_back(std::move(value));
}

std::vector<item> index::complete(const std::string &prefix,
                                  const std::string &qualifier) const {
    static const std::vector<item> empty;
    const std::vector<item> &source =
        qualifier.empty() ? top_ : (members_.count(qualifier)
                                        ? members_.at(qualifier)
                                        : empty);
    std::vector<item> out;
    std::unordered_set<std::string> seen;
    for (const item &candidate : source) {
        if (candidate.label.rfind(prefix, 0) != 0)
            continue;
        if (!seen.insert(candidate.label).second)
            continue;
        out.push_back(candidate);
    }
    std::sort(out.begin(), out.end(),
              [](const item &a, const item &b) { return a.label < b.label; });
    return out;
}

void add_keywords(index &out) {
    for (const std::string &word : repl_ui::keywords())
        out.add_top({word, 14, keyword_detail(word), false});
}

void add_runtime_std(index &out, const value_t *root) {
    if (!root || root->type != T_dict)
        return;
    out.add_top({"std", 9, "standard library", true});
    add_std_node(out, root, "std");
}

std::vector<item> runtime_value_members(const value_t *value) {
    std::vector<item> out;
    if (!value)
        return out;
    if (value->type == T_struct) {
        const struct_value_t *structure = (const struct_value_t *)value;
        if (!structure->def)
            return out;
        for (const field_def &field : structure->def->fields)
            out.push_back({field.name, 6, "field", false});
        for (const auto &method : structure->def->methods)
            out.push_back({method.first, 3, "method", false});
    } else if (value->type == T_enum) {
        const enum_value_t *enumeration = (const enum_value_t *)value;
        if (!enumeration->def || enumeration->variant < 0 ||
            (size_t)enumeration->variant >= enumeration->def->variants.size())
            return out;
        const variant_def &variant =
            enumeration->def->variants[(size_t)enumeration->variant];
        for (const field_def &field : variant.fields)
            out.push_back({field.name, 6, "field", false});
    }
    return out;
}

}  // namespace completion
}  // namespace skein
