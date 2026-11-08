#include "iter.h"

#include "eval.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "runtime_context.h"
#include "structtype.h"
#include "unicode.h"
#include "valuetype.h"

#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

struct iter_source {
    explicit iter_source(struct value_t *source)
        : owned(dupval(source), freeval) {
        if (fiber_is_generator(owned.get())) {
            if (!get_generator_value(owned.get(), generator))
                failed = true;
            return;
        }
        if (!materialize(owned.get(), items))
            failed = true;
    }

    bool next(struct value_t *&out) {
        out = NULL;
        if (failed)
            return false;
        if (generator) {
            struct value_t *value = NULL;
            bool done = false;
            if (!generator_next_value(generator, NULL, value, done)) {
                failed = true;
                return false;
            }
            if (done) {
                freeval(value);
                return false;
            }
            out = value;
            return true;
        }
        if (index >= items.size())
            return false;
        out = items[index];
        items[index] = NULL;
        ++index;
        return true;
    }

    void close() {
        if (generator && !generator_done(generator))
            generator_close(generator);
    }

    ~iter_source() {
        for (size_t i = index; i < items.size(); ++i)
            freeval(items[i]);
    }

    std::shared_ptr<struct value_t> owned;
    generator_ptr generator;
    std::vector<struct value_t *> items;
    size_t index = 0;
    bool failed = false;

    static bool materialize(struct value_t *value,
                            std::vector<struct value_t *> &out) {
        switch (value->type) {
            case T_list: {
                auto *list = (struct list_value_t *)value;
                out.reserve(list->payload->items.size());
                for (struct value_t *item : list->payload->items)
                    out.push_back(dupval(item));
                return true;
            }
            case T_dict: {
                auto *dict = (struct dict_value_t *)value;
                out.reserve(dict->payload->items.size());
                for (auto &entry : dict->payload->items)
                    out.push_back(dupval(entry.first));
                return true;
            }
            case T_string: {
                auto *text = (struct string_value_t *)value;
                size_t text_size = std::strlen(text->value);
                size_t offset = 0;
                while (offset < text_size) {
                    size_t consumed = 0;
                    unsigned int codepoint = 0;
                    if (!utf8_decode_one(text->value + offset,
                                         text_size - offset,
                                         consumed, codepoint) ||
                        consumed == 0) {
                        yyerror("invalid UTF-8 string iteration");
                        return false;
                    }
                    out.push_back(newcharval(codepoint));
                    offset += consumed;
                }
                return true;
            }
            case T_bytes: {
                auto *bytes = (struct bytes_value_t *)value;
                out.reserve(bytes->value.size());
                for (unsigned char byte : bytes->value.read())
                    out.push_back(newintval(byte));
                return true;
            }
            case T_deque:
            case T_heap:
            case T_set: {
                std::vector<struct value_t *> borrowed;
                if (value->type == T_deque)
                    deque_items_in_order(
                        ((struct deque_value_t *)value)->payload.get(),
                        borrowed);
                else if (value->type == T_heap)
                    heap_items_level_order(
                        ((struct heap_value_t *)value)->payload.get(),
                        borrowed);
                else
                    set_values_in_order(
                        ((struct set_value_t *)value)->payload.get(),
                        borrowed);
                out.reserve(borrowed.size());
                for (struct value_t *item : borrowed)
                    out.push_back(dupval(item));
                return true;
            }
            default:
                yyerror("cannot iterate %s", typename_of(value));
                return false;
        }
    }
};

struct iter_capture {
    std::vector<struct value_t *> values;

    ~iter_capture() {
        for (struct value_t *value : values)
            freeval(value);
    }
};

using iter_body =
    std::function<struct value_t *(const std::vector<struct value_t *> &)>;

struct value_t *lazy_iter(const std::vector<struct value_t *> &args,
                          iter_body body) {
    auto capture = std::make_shared<iter_capture>();
    capture->values.reserve(args.size());
    for (struct value_t *value : args)
        capture->values.push_back(dupval(value));

    generator_ptr generator = generator_create([capture, body]() {
        return body(capture->values);
    });
    if (!generator) {
        yyerror("generators are not available");
        return NULL;
    }
    return new_generator_value(generator);
}

bool callable_value(const struct value_t *value) {
    if (!value)
        return false;
    return value->type == T_func || value->type == T_builtin ||
           value->type == T_method || value->type == T_native;
}

bool yield_owned(struct value_t *value) {
    struct value_t *sent = fiber_yield_value(value);
    if (!sent)
        return false;
    freeval(sent);
    return true;
}

struct value_t *iter_map_body(const std::vector<struct value_t *> &values) {
    iter_source source(values[0]);
    struct value_t *item = NULL;
    while (source.next(item)) {
        std::vector<struct value_t *> call_args;
        call_args.push_back(item);
        struct value_t *mapped =
            skein_call_function(values[1], std::move(call_args));
        if (!mapped)
            return NULL;
        if (!yield_owned(mapped))
            return NULL;
    }
    return newnull();
}

struct value_t *iter_filter_body(const std::vector<struct value_t *> &values) {
    iter_source source(values[0]);
    struct value_t *item = NULL;
    while (source.next(item)) {
        std::vector<struct value_t *> call_args;
        call_args.push_back(dupval(item));
        struct value_t *keep =
            skein_call_function(values[1], std::move(call_args));
        if (!keep) {
            freeval(item);
            return NULL;
        }
        bool wanted = truthy(keep);
        freeval(keep);
        if (wanted) {
            if (!yield_owned(item))
                return NULL;
        } else {
            freeval(item);
        }
    }
    return newnull();
}

struct value_t *iter_take_body(const std::vector<struct value_t *> &values) {
    long long limit = ((struct int_value_t *)values[1])->value;
    if (limit <= 0)
        return newnull();
    iter_source source(values[0]);
    struct value_t *item = NULL;
    long long taken = 0;
    while (taken < limit && source.next(item)) {
        if (!yield_owned(item))
            return NULL;
        ++taken;
    }
    if (taken >= limit)
        source.close();
    return newnull();
}

struct value_t *iter_drop_body(const std::vector<struct value_t *> &values) {
    long long skip = ((struct int_value_t *)values[1])->value;
    iter_source source(values[0]);
    struct value_t *item = NULL;
    while (skip > 0 && source.next(item)) {
        freeval(item);
        --skip;
    }
    while (source.next(item)) {
        if (!yield_owned(item))
            return NULL;
    }
    return newnull();
}

struct value_t *iter_enumerate_body(
    const std::vector<struct value_t *> &values) {
    iter_source source(values[0]);
    struct value_t *item = NULL;
    long long index = 0;
    while (source.next(item)) {
        struct list_value_t *pair = newlistval();
        pair->payload->items.push_back(newintval(index));
        pair->payload->items.push_back(item);
        if (!yield_owned((struct value_t *)pair))
            return NULL;
        ++index;
    }
    return newnull();
}

struct value_t *iter_collect(const std::vector<struct value_t *> &args) {
    iter_source source(args[0]);
    struct list_value_t *out = newlistval();
    struct value_t *item = NULL;
    while (source.next(item))
        out->payload->items.push_back(item);
    if (source.failed) {
        freeval((struct value_t *)out);
        return NULL;
    }
    return (struct value_t *)out;
}

bool for_each_wants_two_args(struct value_t *fn, bool &two_args) {
    two_args = false;
    int declared = -1;
    int required = -1;
    if (fn && fn->type == T_func) {
        struct func_value_t *func = (struct func_value_t *)fn;
        if (func->def && func->def->code) {
            declared = func->def->code->nparams;
            required = func->def->code->required_params;
        }
    } else if (fn && fn->type == T_method) {
        struct method_value_t *method = (struct method_value_t *)fn;
        if (method->method && method->method->code) {
            /* Bound method arity excludes the implicit receiver. */
            declared = method->method->code->nparams - 1;
            required = method->method->code->required_params - 1;
        }
    }
    if (declared < 0)
        return true;
    if (required > 2) {
        yyerror("std.iter.for_each callback must accept 1 or 2 parameters");
        return false;
    }
    two_args = declared >= 2;
    return true;
}

struct value_t *iter_for_each(const std::vector<struct value_t *> &args) {
    bool two_args = false;
    if (!for_each_wants_two_args(args[1], two_args))
        return NULL;

    if (two_args && args[0] && args[0]->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)args[0];
        for (auto &entry : dict->payload->items) {
            std::vector<struct value_t *> call_args;
            call_args.push_back(dupval(entry.first));
            call_args.push_back(dupval(entry.second));
            struct value_t *result =
                skein_call_function(args[1], std::move(call_args));
            if (!result)
                return NULL;
            freeval(result);
        }
        return newnull();
    }

    iter_source source(args[0]);
    struct value_t *item = NULL;
    long long index = 0;
    while (source.next(item)) {
        std::vector<struct value_t *> call_args;
        if (two_args)
            call_args.push_back(newintval(index));
        call_args.push_back(item);
        struct value_t *result =
            skein_call_function(args[1], std::move(call_args));
        if (!result)
            return NULL;
        freeval(result);
        ++index;
    }
    if (source.failed)
        return NULL;
    return newnull();
}

} // namespace

struct value_t *call_iter_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_iter_map:
        case B_iter_filter:
            if (args.size() != 2) {
                yyerror("%s expects 2 arguments", functype == B_iter_map
                                                     ? "std.iter.map"
                                                     : "std.iter.filter");
                return NULL;
            }
            if (!callable_value(args[1])) {
                yyerror("%s expects a callable, got %s",
                        functype == B_iter_map ? "std.iter.map"
                                               : "std.iter.filter",
                        typename_of(args[1]));
                return NULL;
            }
            return lazy_iter(args, functype == B_iter_map ? iter_map_body
                                                          : iter_filter_body);
        case B_iter_take:
        case B_iter_drop:
            if (args.size() != 2) {
                yyerror("%s expects 2 arguments", functype == B_iter_take
                                                     ? "std.iter.take"
                                                     : "std.iter.drop");
                return NULL;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("%s expects a non-negative int",
                        functype == B_iter_take ? "std.iter.take"
                                                : "std.iter.drop");
                return NULL;
            }
            return lazy_iter(args, functype == B_iter_take ? iter_take_body
                                                           : iter_drop_body);
        case B_iter_enumerate:
            if (args.size() != 1) {
                yyerror("std.iter.enumerate expects 1 argument");
                return NULL;
            }
            return lazy_iter(args, iter_enumerate_body);
        case B_iter_collect:
            if (args.size() != 1) {
                yyerror("std.iter.collect expects 1 argument");
                return NULL;
            }
            return iter_collect(args);
        case B_iter_for_each:
            if (args.size() != 2) {
                yyerror("std.iter.for_each expects 2 arguments");
                return NULL;
            }
            if (!callable_value(args[1])) {
                yyerror("std.iter.for_each expects a callable, got %s",
                        typename_of(args[1]));
                return NULL;
            }
            return iter_for_each(args);
        default:
            yyerror("unknown std.iter function");
            return NULL;
    }
}
