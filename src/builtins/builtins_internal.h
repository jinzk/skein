#ifndef SKEIN_BUILTINS_INTERNAL_H
#define SKEIN_BUILTINS_INTERNAL_H

#include "builtins.h"
#include "builtin_ids.h"
#include "dict.h"
#include "encoding.h"
#include "eval.h"
#include "eval_helpers.h"
#include "filetype.h"
#include "format.h"
#include "platform.h"
#include "runtime_context.h"
#include "runtime_state.h"
#include "structtype.h"
#include "symbol.h"
#include "unicode.h"
#include "valuetype.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct value_t;

const char *math_builtin_name(int functype);

std::shared_ptr<deque_payload> deque_push_back_payload(
    const std::shared_ptr<deque_payload> &source, struct value_t *value);

struct value_t *call_math_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_lru_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_deque_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_heap_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_set_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_list_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_dict_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_string_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_convert_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_file_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_text_builtin(
    int functype, const std::vector<struct value_t *> &args);
struct value_t *call_bytes_builtin(
    int functype, const std::vector<struct value_t *> &args);

bool get_file_resource(struct value_t *value,
                       std::shared_ptr<struct file_resource> &out,
                       const char *name);

#endif
