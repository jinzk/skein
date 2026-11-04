#include "generator.h"

#include "ast.h"
#include "fiber.h"
#include "valuetype.h"

#include <string>
#include <utility>
#include <vector>

namespace {

value_t *generator_next_builtin(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.generator.next expects 1 or 2 arguments");
        return NULL;
    }
    generator_ptr generator;
    if (!get_generator_value(args[0], generator))
        return NULL;

    value_t *send_value = args.size() == 2 ? dupval(args[1]) : NULL;
    value_t *value = NULL;
    bool done = false;
    if (!generator_next_value(generator, send_value, value, done))
        return NULL;
    if (!value)
        value = newnull();

    struct dict_value_t *result = newdictval();
    result->payload->items.push_back(std::make_pair(
        newstrval("done"), newboolval(done)));
    result->payload->items.push_back(std::make_pair(
        newstrval("value"), value));
    return (value_t *)result;
}

value_t *generator_done_builtin(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.generator.done expects 1 argument");
        return NULL;
    }
    generator_ptr generator;
    if (!get_generator_value(args[0], generator))
        return NULL;
    return newboolval(generator_done(generator));
}

value_t *generator_close_builtin(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.generator.close expects 1 argument");
        return NULL;
    }
    generator_ptr generator;
    if (!get_generator_value(args[0], generator))
        return NULL;
    return newboolval(generator_close(generator));
}

} // namespace

struct value_t *call_generator_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_generator_next:
            return generator_next_builtin(args);
        case B_generator_done:
            return generator_done_builtin(args);
        case B_generator_close:
            return generator_close_builtin(args);
        default:
            yyerror("unknown generator function");
            return NULL;
    }
}
