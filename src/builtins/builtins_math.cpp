#include "builtins_internal.h"

const char *math_builtin_name(int functype) {
    switch (functype) {
        case B_sin: return "sin";
        case B_cos: return "cos";
        case B_tan: return "tan";
        case B_asin: return "asin";
        case B_acos: return "acos";
        case B_atan: return "atan";
        case B_atan2: return "atan2";
        case B_pow: return "pow";
        case B_floor: return "floor";
        case B_ceil: return "ceil";
        case B_round: return "round";
        case B_trunc: return "trunc";
        case B_log2: return "log2";
        case B_log10: return "log10";
        case B_hypot: return "hypot";
        case B_min: return "min";
        case B_max: return "max";
        case B_math_sign: return "std.math.sign";
        case B_math_clamp: return "std.math.clamp";
        case B_math_fmod: return "std.math.fmod";
        case B_math_mod: return "std.math.mod";
        case B_math_is_nan: return "std.math.is_nan";
        case B_math_is_inf: return "std.math.is_inf";
        case B_math_is_finite: return "std.math.is_finite";
        case B_math_cbrt: return "std.math.cbrt";
        case B_math_gcd: return "std.math.gcd";
        case B_math_lcm: return "std.math.lcm";
        case B_math_degrees: return "std.math.degrees";
        case B_math_radians: return "std.math.radians";
        default: return "math function";
    }
}

struct value_t *call_math_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_sin:
        case B_cos:
        case B_tan:
        case B_asin:
        case B_acos:
        case B_atan: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            double result = 0.0;
            if (functype == B_sin) result = std::sin(value);
            else if (functype == B_cos) result = std::cos(value);
            else if (functype == B_tan) result = std::tan(value);
            else if (functype == B_asin) result = std::asin(value);
            else if (functype == B_acos) result = std::acos(value);
            else result = std::atan(value);
            ret = newfloatval(result);
            break;
        }
        case B_atan2:
        case B_pow:
        case B_hypot: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("%s expects two numbers, got %s and %s",
                        math_builtin_name(functype), typename_of(args[0]),
                        typename_of(args[1]));
                break;
            }
            double a = as_double(args[0]);
            double b = as_double(args[1]);
            if (functype == B_atan2) ret = newfloatval(std::atan2(a, b));
            else if (functype == B_pow) ret = newfloatval(std::pow(a, b));
            else ret = newfloatval(std::hypot(a, b));
            break;
        }
        case B_floor:
        case B_ceil:
        case B_round:
        case B_trunc: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (std::isnan(value) || std::isinf(value)) {
                yyerror("%s expects a finite number", math_builtin_name(functype));
                break;
            }
            double result = 0.0;
            if (functype == B_floor) result = std::floor(value);
            else if (functype == B_ceil) result = std::ceil(value);
            else if (functype == B_round) result = std::round(value);
            else result = std::trunc(value);
            if (result < -9223372036854775808.0 ||
                result >= 9223372036854775808.0)
                yyerror("%s result out of integer range",
                        math_builtin_name(functype));
            else
                ret = newintval((long long)result);
            break;
        }
        case B_log2:
        case B_log10: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (value <= 0.0)
                ret = newfloatval(std::nan(""));
            else
                ret = newfloatval(functype == B_log2
                                      ? std::log2(value)
                                      : std::log10(value));
            break;
        }
        case B_math_sign: {
            if (!is_numeric(arg)) {
                yyerror("std.math.sign expects a number, got %s",
                        typename_of(arg));
                break;
            }
            if (arg->type == T_float) {
                double value = ((struct float_value_t *)arg)->value;
                if (std::isnan(value))
                    ret = newfloatval(value);
                else
                    ret = newintval(value < 0.0 ? -1 : (value > 0.0 ? 1 : 0));
            } else {
                long long value = ((struct int_value_t *)arg)->value;
                ret = newintval(value < 0 ? -1 : (value > 0 ? 1 : 0));
            }
            break;
        }
        case B_math_clamp: {
            for (struct value_t *value : args) {
                if (!is_numeric(value)) {
                    yyerror("std.math.clamp expects numbers, got %s",
                            typename_of(value));
                    return NULL;
                }
            }
            if (args[0]->type == T_int && args[1]->type == T_int &&
                args[2]->type == T_int) {
                long long value = ((struct int_value_t *)args[0])->value;
                long long low = ((struct int_value_t *)args[1])->value;
                long long high = ((struct int_value_t *)args[2])->value;
                if (low > high) {
                    yyerror("std.math.clamp lower bound exceeds upper bound");
                    break;
                }
                if (value < low)
                    value = low;
                else if (value > high)
                    value = high;
                ret = newintval(value);
            } else {
                double value = as_double(args[0]);
                double low = as_double(args[1]);
                double high = as_double(args[2]);
                if (low > high) {
                    yyerror("std.math.clamp lower bound exceeds upper bound");
                    break;
                }
                if (value < low)
                    value = low;
                else if (value > high)
                    value = high;
                ret = newfloatval(value);
            }
            break;
        }
        case B_math_fmod: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("std.math.fmod expects numbers, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            ret = newfloatval(
                std::fmod(as_double(args[0]), as_double(args[1])));
            break;
        }
        case B_math_mod: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("std.math.mod expects numbers, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            if (args[0]->type == T_int && args[1]->type == T_int) {
                long long left = ((struct int_value_t *)args[0])->value;
                long long right = ((struct int_value_t *)args[1])->value;
                if (right == 0) {
                    yyerror("modulo by zero");
                    break;
                }
                long long value = left % right;
                if (value != 0 && ((value < 0) != (right < 0)))
                    value += right;
                ret = newintval(value);
            } else {
                double left = as_double(args[0]);
                double right = as_double(args[1]);
                double value = std::fmod(left, right);
                if (value != 0.0 && ((value < 0.0) != (right < 0.0)))
                    value += right;
                ret = newfloatval(value);
            }
            break;
        }
        case B_math_is_nan:
        case B_math_is_inf:
        case B_math_is_finite: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (functype == B_math_is_nan)
                ret = newboolval(std::isnan(value));
            else if (functype == B_math_is_inf)
                ret = newboolval(std::isinf(value));
            else
                ret = newboolval(std::isfinite(value));
            break;
        }
        case B_math_cbrt: {
            if (!is_numeric(arg)) {
                yyerror("std.math.cbrt expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(std::cbrt(as_double(arg)));
            break;
        }
        case B_math_gcd:
        case B_math_lcm: {
            if (args[0]->type != T_int || args[1]->type != T_int) {
                yyerror("%s expects integers, got %s and %s",
                        math_builtin_name(functype),
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            __int128 left = ((struct int_value_t *)args[0])->value;
            __int128 right = ((struct int_value_t *)args[1])->value;
            if (left < 0)
                left = -left;
            if (right < 0)
                right = -right;
            __int128 a = left;
            __int128 b = right;
            while (b != 0) {
                __int128 next = a % b;
                a = b;
                b = next;
            }
            if (functype == B_math_gcd) {
                ret = new_int_checked(a);
            } else if (left == 0 || right == 0) {
                ret = newintval(0);
            } else {
                ret = new_int_checked((left / a) * right);
            }
            break;
        }
        case B_math_degrees: {
            if (!is_numeric(arg)) {
                yyerror("std.math.degrees expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(
                as_double(arg) * (180.0 / 3.14159265358979323846));
            break;
        }
        case B_math_radians: {
            if (!is_numeric(arg)) {
                yyerror("std.math.radians expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(
                as_double(arg) * (3.14159265358979323846 / 180.0));
            break;
        }
        case B_min:
        case B_max: {
            bool all_int = true;
            for (const struct value_t *value : args) {
                if (!is_numeric(value)) {
                    yyerror("%s expects numbers, got %s",
                            math_builtin_name(functype), typename_of(value));
                    return NULL;
                }
                if (value->type != T_int)
                    all_int = false;
            }
            if (all_int) {
                long long best = ((struct int_value_t *)args[0])->value;
                for (size_t i = 1; i < args.size(); ++i) {
                    long long value =
                        ((struct int_value_t *)args[i])->value;
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newintval(best);
            } else {
                double best = as_double(args[0]);
                for (size_t i = 1; i < args.size(); ++i) {
                    double value = as_double(args[i]);
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newfloatval(best);
            }
            break;
        }

        case B_sqrt:
        case B_exp:
        case B_log:
            if (!is_numeric(arg)) {
                yyerror("math function expects a number, got %s", typename_of(arg));
                break;
            }
            if (functype == B_sqrt) {
                double value = as_double(arg);
                ret = newfloatval(value < 0.0 ? std::nan("")
                                             : std::sqrt(value));
            } else if (functype == B_exp) {
                ret = newfloatval(std::exp(as_double(arg)));
            } else {
                double value = as_double(arg);
                ret = newfloatval(value <= 0.0 ? std::nan("")
                                              : std::log(value));
            }
            break;
        case B_abs:
            if (arg->type == T_int) {
                long long n = ((struct int_value_t *)arg)->value;
                ret = new_int_checked(n < 0 ? -(__int128)n : (__int128)n);
            } else if (arg->type == T_float) {
                ret = newfloatval(std::fabs(((struct float_value_t *)arg)->value));
            } else {
                yyerror("abs expects a number, got %s", typename_of(arg));
            }
            break;

        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
