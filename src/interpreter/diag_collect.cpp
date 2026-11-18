#include "diag_collect.h"

#include <cstddef>

namespace {

thread_local bool collect_active = false;
thread_local bool suppress_text = false;
thread_local std::vector<diagnostic_record> records;

} // namespace

void diag_collect_begin(bool suppress_text_output) {
    collect_active = true;
    suppress_text = suppress_text_output;
    records.clear();
}

std::vector<diagnostic_record> diag_collect_end() {
    collect_active = false;
    suppress_text = false;
    std::vector<diagnostic_record> result;
    result.swap(records);
    return result;
}

bool diag_collect_active() { return collect_active; }

bool diag_collect_suppress_text() { return suppress_text; }

void diag_collect_push(const diagnostic_record &record) {
    if (collect_active)
        records.push_back(record);
}

const char *diag_code_for_category(const char *category) {
    if (!category)
        return "GEN0001";
    const std::string text(category);
    if (text == "syntax error")
        return "PAR0001";
    if (text == "lexical error")
        return "LEX0001";
    if (text == "type error")
        return "TYP0001";
    if (text == "type warning")
        return "TYP0002";
    if (text == "runtime error")
        return "RNT0001";
    return "GEN0001";
}
