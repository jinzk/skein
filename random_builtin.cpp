#include "random_builtin.h"

#include "ast.h"
#include "valuetype.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

std::mt19937_64 &engine() {
    static std::mt19937_64 value = [] {
        std::random_device device;
        const uint64_t seed =
            ((uint64_t)device() << 32) ^ (uint64_t)device() ^
            (uint64_t)std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count();
        return std::mt19937_64(seed);
    }();
    return value;
}

}  // namespace

struct value_t *call_random_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_random_seed: {
            if (args.size() != 1) {
                yyerror("std.random.seed expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_int) {
                yyerror("std.random.seed expects an integer, got %s",
                        typename_of(args[0]));
                return NULL;
            }
            engine().seed(
                (uint64_t)((struct int_value_t *)args[0])->value);
            return newnull();
        }
        case B_random_int: {
            if (args.size() != 2) {
                yyerror("std.random.int expects 2 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_int || args[1]->type != T_int) {
                yyerror("std.random.int expects two integers");
                return NULL;
            }
            const long long minimum =
                ((struct int_value_t *)args[0])->value;
            const long long maximum =
                ((struct int_value_t *)args[1])->value;
            if (minimum > maximum) {
                yyerror("std.random.int minimum must not exceed maximum");
                return NULL;
            }
            std::uniform_int_distribution<long long> distribution(
                minimum, maximum);
            return newintval(distribution(engine()));
        }
        case B_random_float: {
            if (!args.empty()) {
                yyerror("std.random.float expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            std::uniform_real_distribution<double> distribution(0.0, 1.0);
            return newfloatval(distribution(engine()));
        }
        case B_random_bytes: {
            if (args.size() != 1) {
                yyerror("std.random.bytes expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_int ||
                ((struct int_value_t *)args[0])->value < 0) {
                yyerror("std.random.bytes expects a non-negative integer");
                return NULL;
            }
            const size_t count =
                (size_t)((struct int_value_t *)args[0])->value;
            std::string result;
            result.reserve(count);
            std::uniform_int_distribution<int> distribution(0, 255);
            for (size_t i = 0; i < count; ++i)
                result.push_back((char)distribution(engine()));
            return newbytesval(std::move(result));
        }
        case B_random_choice: {
            if (args.size() != 1 || args[0]->type != T_list) {
                yyerror("std.random.choice expects 1 list argument");
                return NULL;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            if (list->payload->items.empty()) {
                yyerror("std.random.choice expects a non-empty list");
                return NULL;
            }
            std::uniform_int_distribution<size_t> distribution(
                0, list->payload->items.size() - 1);
            return dupval(list->payload->items[distribution(engine())]);
        }
        case B_random_shuffle: {
            if (args.size() != 1 || args[0]->type != T_list) {
                yyerror("std.random.shuffle expects 1 list argument");
                return NULL;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            std::vector<value_t *> items;
            items.reserve(list->payload->items.size());
            for (value_t *item : list->payload->items)
                items.push_back(dupval(item));
            std::shuffle(items.begin(), items.end(), engine());
            struct list_value_t *result = newlistval();
            result->payload->items = std::move(items);
            return (value_t *)result;
        }
        case B_random_uuid: {
            if (!args.empty()) {
                yyerror("std.random.uuid expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            unsigned char bytes[16];
            std::uniform_int_distribution<int> distribution(0, 255);
            for (unsigned char &byte : bytes)
                byte = (unsigned char)distribution(engine());
            bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40);
            bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80);
            char buffer[37];
            std::snprintf(
                buffer, sizeof buffer,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
            return newstrval(buffer);
        }
        default:
            yyerror("unknown random function");
            return NULL;
    }
}
