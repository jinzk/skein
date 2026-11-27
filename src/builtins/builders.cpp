#include "builders.h"

#include "ast.h"
#include "builtins_internal.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct list_builder_state
    : std::enable_shared_from_this<list_builder_state> {
    std::vector<value_t *> items;

    ~list_builder_state() {
        for (value_t *item : items)
            freeval(item);
    }
};

struct string_builder_state
    : std::enable_shared_from_this<string_builder_state> {
    std::string text;
};

std::string list_builder_describe(const void *data) {
    const list_builder_state *state =
        static_cast<const list_builder_state *>(data);
    return "list_builder(len=" +
           std::to_string(state ? state->items.size() : 0) + ")";
}

std::string string_builder_describe(const void *data) {
    const string_builder_state *state =
        static_cast<const string_builder_state *>(data);
    return "string_builder(len=" +
           std::to_string(state ? state->text.size() : 0) + ")";
}

value_t *list_builder_get_field(value_t *self, void *data, const char *name);
value_t *string_builder_get_field(void *data, const char *name);

/* Compatibility entry point for callers that only hold the payload. */
value_t *list_builder_get_field_plain(void *data, const char *name) {
    return list_builder_get_field(nullptr, data, name);
}

void list_builder_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *child)> &visit) {
    const list_builder_state *state =
        static_cast<const list_builder_state *>(data);
    if (!state)
        return;
    for (const value_t *item : state->items) {
        if (item)
            visit(item);
    }
}

const struct native_type kListBuilderType = {
    "list_builder",
    list_builder_describe,
    nullptr,
    list_builder_get_field_plain,
    nullptr,
    list_builder_for_each_child,
    list_builder_get_field,
};

const struct native_type kStringBuilderType = {
    "string_builder",
    string_builder_describe,
    nullptr,
    string_builder_get_field,
    nullptr,
    nullptr,
};

value_t *list_builder_get_field(value_t *self, void *data, const char *name) {
    list_builder_state *state = static_cast<list_builder_state *>(data);
    if (!state || !name) {
        yyerror("list builder has no field");
        return NULL;
    }
    std::shared_ptr<list_builder_state> shared = state->shared_from_this();
    auto method = [shared, self](
        std::function<value_t *(const std::vector<value_t *> &)> call) {
        value_t *fn = newnativeval(std::move(call), shared);
        native_add_child(fn, self);
        return fn;
    };
    if (std::strcmp(name, "push") == 0) {
        return method([shared](const std::vector<value_t *> &args) {
            if (args.size() != 1) {
                yyerror("builder.push expects 1 argument");
                return (value_t *)NULL;
            }
            shared->items.push_back(dupval(args[0]));
            return newnull();
        });
    }
    if (std::strcmp(name, "extend") == 0) {
        return method([shared](const std::vector<value_t *> &args) {
            if (args.size() != 1 || !args[0] || args[0]->type != T_list) {
                yyerror("builder.extend expects a list");
                return (value_t *)NULL;
            }
            const auto &items =
                ((const list_value_t *)args[0])->payload->items;
            shared->items.reserve(shared->items.size() + items.size());
            for (value_t *item : items)
                shared->items.push_back(dupval(item));
            return newnull();
        });
    }
    if (std::strcmp(name, "len") == 0) {
        return method([shared](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("builder.len expects 0 arguments");
                return (value_t *)NULL;
            }
            return newintval((long long)shared->items.size());
        });
    }
    if (std::strcmp(name, "build") == 0) {
        return method([shared](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("builder.build expects 0 arguments");
                return (value_t *)NULL;
            }
            list_value_t *list = newlistval();
            list->payload->items.reserve(shared->items.size());
            for (value_t *item : shared->items)
                list->payload->items.push_back(dupval(item));
            return (value_t *)list;
        });
    }
    yyerror("list builder has no field %s", name);
    return NULL;
}

value_t *string_builder_get_field(void *data, const char *name) {
    string_builder_state *state = static_cast<string_builder_state *>(data);
    if (!state || !name) {
        yyerror("string builder has no field");
        return NULL;
    }
    std::shared_ptr<string_builder_state> shared = state->shared_from_this();
    if (std::strcmp(name, "push") == 0) {
        return newnativeval([shared](const std::vector<value_t *> &args) {
            if (args.size() != 1 || !args[0] ||
                (args[0]->type != T_string && args[0]->type != T_char)) {
                yyerror("builder.push expects a string or char");
                return (value_t *)NULL;
            }
            if (args[0]->type == T_char) {
                std::string encoded;
                if (!utf8_encode(((const char_value_t *)args[0])->value,
                                 encoded)) {
                    yyerror("builder.push: invalid character");
                    return (value_t *)NULL;
                }
                shared->text += encoded;
            } else {
                shared->text += ((const string_value_t *)args[0])->value;
            }
            return newnull();
        }, shared);
    }
    if (std::strcmp(name, "len") == 0) {
        return newnativeval([shared](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("builder.len expects 0 arguments");
                return (value_t *)NULL;
            }
            return newintval((long long)shared->text.size());
        }, shared);
    }
    if (std::strcmp(name, "build") == 0) {
        return newnativeval([shared](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("builder.build expects 0 arguments");
                return (value_t *)NULL;
            }
            return newstrval(shared->text.c_str());
        }, shared);
    }
    yyerror("string builder has no field %s", name);
    return NULL;
}

value_t *new_list_builder() {
    auto state = std::make_shared<list_builder_state>();
    return newuserdataval(&kListBuilderType, state);
}

value_t *new_string_builder() {
    auto state = std::make_shared<string_builder_state>();
    return newuserdataval(&kStringBuilderType, state);
}

} // namespace

struct value_t *call_builder_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_list_builder:
            if (!args.empty()) {
                yyerror("std.list.builder expects 0 arguments");
                return NULL;
            }
            return new_list_builder();
        case B_string_builder:
            if (!args.empty()) {
                yyerror("std.string.builder expects 0 arguments");
                return NULL;
            }
            return new_string_builder();
        default:
            yyerror("unknown builder function");
            return NULL;
    }
}
