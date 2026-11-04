#include "time_builtin.h"

#include "ast.h"
#include "valuetype.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char *kDefaultFormat = "%Y-%m-%d %H:%M:%S";

time_t tm_to_utc(std::tm *tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

bool get_tm(time_t value, bool utc, std::tm &tm) {
#ifdef _WIN32
    return utc ? gmtime_s(&tm, &value) == 0
               : localtime_s(&tm, &value) == 0;
#else
    return utc ? gmtime_r(&value, &tm) != nullptr
               : localtime_r(&value, &tm) != nullptr;
#endif
}

bool bool_argument(const struct value_t *value, bool &result) {
    if (value->type == T_bool) {
        result = ((struct bool_value_t *)value)->value;
        return true;
    }
    if (value->type == T_int) {
        result = ((struct int_value_t *)value)->value != 0;
        return true;
    }
    return false;
}

bool get_timestamp(const std::vector<struct value_t *> &args,
                   size_t index, long long &timestamp) {
    if (args[index]->type != T_int) {
        yyerror("std.time expects an integer timestamp, got %s",
                typename_of(args[index]));
        return false;
    }
    timestamp = ((struct int_value_t *)args[index])->value;
    return true;
}

}  // namespace

struct value_t *call_time_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_time_now: {
            if (!args.empty()) {
                yyerror("std.time.now expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            return newintval((long long)std::time(nullptr));
        }
        case B_time_now_ms: {
            if (!args.empty()) {
                yyerror("std.time.now_ms expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return newintval((long long)std::chrono::duration_cast<
                                 std::chrono::milliseconds>(now)
                                 .count());
        }
        case B_time_monotonic_ms: {
            if (!args.empty()) {
                yyerror("std.time.monotonic_ms expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            static const auto start = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::steady_clock::now() - start;
            return newintval((long long)std::chrono::duration_cast<
                                 std::chrono::milliseconds>(elapsed)
                                 .count());
        }
        case B_time_utc_offset: {
            if (!args.empty()) {
                yyerror("std.time.utc_offset expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            const time_t now = std::time(nullptr);
            std::tm local{};
            if (!get_tm(now, false, local)) {
                yyerror("std.time.utc_offset: cannot read local time");
                return NULL;
            }
            return newintval((long long)(tm_to_utc(&local) - now));
        }
        case B_time_format: {
            if (args.empty() || args.size() > 3) {
                yyerror("std.time.format expects 1 to 3 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            long long timestamp = 0;
            if (!get_timestamp(args, 0, timestamp))
                return NULL;
            std::string format = kDefaultFormat;
            if (args.size() >= 2) {
                if (args[1]->type != T_string) {
                    yyerror("std.time.format expects a format string, got %s",
                            typename_of(args[1]));
                    return NULL;
                }
                format = ((struct string_value_t *)args[1])->value;
            }
            bool utc = false;
            if (args.size() == 3 &&
                !bool_argument(args[2], utc)) {
                yyerror("std.time.format expects a bool utc flag, got %s",
                        typename_of(args[2]));
                return NULL;
            }
            std::tm tm{};
            if (!get_tm((time_t)timestamp, utc, tm)) {
                yyerror("std.time.format: invalid timestamp");
                return NULL;
            }
            char buffer[4096];
            const size_t length = std::strftime(
                buffer, sizeof buffer, format.c_str(), &tm);
            return newstrval(std::string(buffer, length).c_str());
        }
        case B_time_parse: {
            if (args.empty() || args.size() > 3) {
                yyerror("std.time.parse expects 1 to 3 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_string) {
                yyerror("std.time.parse expects a string, got %s",
                        typename_of(args[0]));
                return NULL;
            }
            std::string format = kDefaultFormat;
            if (args.size() >= 2) {
                if (args[1]->type != T_string) {
                    yyerror("std.time.parse expects a format string, got %s",
                            typename_of(args[1]));
                    return NULL;
                }
                format = ((struct string_value_t *)args[1])->value;
            }
            bool utc = false;
            if (args.size() == 3 && !bool_argument(args[2], utc)) {
                yyerror("std.time.parse expects a bool utc flag, got %s",
                        typename_of(args[2]));
                return NULL;
            }
            std::tm tm{};
            tm.tm_isdst = -1;
            std::istringstream stream(
                ((struct string_value_t *)args[0])->value);
            stream >> std::get_time(&tm, format.c_str());
            if (stream.fail()) {
                yyerror("std.time.parse: text does not match format");
                return NULL;
            }
            stream >> std::ws;
            if (!stream.eof()) {
                yyerror("std.time.parse: trailing text after timestamp");
                return NULL;
            }
            const time_t result =
                utc ? tm_to_utc(&tm) : std::mktime(&tm);
            if (result == (time_t)-1) {
                yyerror("std.time.parse: invalid timestamp");
                return NULL;
            }
            return newintval((long long)result);
        }
        case B_time_sleep: {
            if (args.size() != 1) {
                yyerror("std.time.sleep expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (args[0]->type != T_int ||
                ((struct int_value_t *)args[0])->value < 0) {
                yyerror("std.time.sleep expects a non-negative integer");
                return NULL;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(
                ((struct int_value_t *)args[0])->value));
            return newnull();
        }
        default:
            yyerror("unknown time function");
            return NULL;
    }
}
