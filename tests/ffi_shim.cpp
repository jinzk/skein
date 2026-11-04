#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define SHIM_EXPORT __declspec(dllexport)
#else
#define SHIM_EXPORT __attribute__((visibility("default")))
#endif

struct shim_point {
    double x;
    double y;
};

struct shim_pair {
    std::int8_t tag;
    std::int64_t value;
};

struct shim_nested {
    struct shim_point point;
    std::int32_t tag;
};

struct shim_words {
    std::int32_t values[3];
};

typedef std::int32_t (*shim_cmp_fn)(std::int32_t, std::int32_t);
typedef std::int32_t (*shim_ptr_cmp_fn)(const std::int32_t *,
                                        const std::int32_t *);
typedef double (*shim_point_fn)(struct shim_point);
typedef const char *(*shim_string_fn)(const char *);
typedef void (*shim_void_fn)(std::int32_t);
typedef void (*shim_notify_fn)(std::int32_t);

extern "C" {
    SHIM_EXPORT std::int32_t shim_global = 7;
    SHIM_EXPORT extern const std::int32_t shim_const_global = 42;

SHIM_EXPORT std::int8_t shim_i8(std::int8_t value) {
    return value;
}

SHIM_EXPORT std::uint8_t shim_u8(std::uint8_t value) {
    return value;
}

SHIM_EXPORT std::int16_t shim_i16(std::int16_t value) {
    return value;
}

SHIM_EXPORT std::uint16_t shim_u16(std::uint16_t value) {
    return value;
}

SHIM_EXPORT std::int32_t shim_i32(std::int32_t value) {
    return value;
}

SHIM_EXPORT std::int32_t shim_sum_varargs(std::int32_t count, ...) {
    va_list args;
    va_start(args, count);
    std::int32_t sum = 0;
    for (std::int32_t i = 0; i < count; ++i)
        sum += va_arg(args, int);
    va_end(args);
    return sum;
}

SHIM_EXPORT std::uint32_t shim_u32(std::uint32_t value) {
    return value;
}

SHIM_EXPORT std::int64_t shim_i64(std::int64_t value) {
    return value;
}

SHIM_EXPORT std::uint64_t shim_u64(std::uint64_t value) {
    return value;
}

SHIM_EXPORT float shim_f32(float value) {
    return value;
}

SHIM_EXPORT double shim_f64(double value) {
    return value;
}

SHIM_EXPORT const char *shim_cstring(const char *value) {
    return value;
}

SHIM_EXPORT std::size_t shim_usize(std::size_t value) {
    return value;
}

SHIM_EXPORT std::ptrdiff_t shim_isize(std::ptrdiff_t value) {
    return value;
}

SHIM_EXPORT std::uint64_t shim_u64_max(void) {
    return UINT64_MAX;
}

SHIM_EXPORT std::size_t shim_usize_max(void) {
    return SIZE_MAX;
}

SHIM_EXPORT std::int32_t shim_memcmp(const std::uint8_t *left,
                                     const std::uint8_t *right,
                                     std::size_t length) {
    return std::memcmp(left, right, length);
}

SHIM_EXPORT std::size_t shim_memcpy(std::uint8_t *dst,
                                    const std::uint8_t *src,
                                    std::size_t length) {
    std::memcpy(dst, src, length);
    return length;
}

SHIM_EXPORT std::int32_t shim_buffer_write(std::uint8_t *buffer,
                                           std::size_t capacity) {
    static const char text[] = "hello";
    std::size_t length = sizeof text - 1;
    if (capacity < length)
        return -1;
    std::memcpy(buffer, text, length);
    return (std::int32_t)length;
}

SHIM_EXPORT const char *shim_error(void) {
    return "shim error";
}

SHIM_EXPORT double shim_point_sum(struct shim_point point) {
    return point.x + point.y;
}

SHIM_EXPORT struct shim_point shim_point_make(double x, double y) {
    struct shim_point point = {x, y};
    return point;
}

SHIM_EXPORT double shim_point_dot(const struct shim_point *point) {
    return point->x * point->x + point->y * point->y;
}

SHIM_EXPORT void shim_point_scale(struct shim_point *point, double factor) {
    point->x *= factor;
    point->y *= factor;
}

SHIM_EXPORT std::int64_t shim_pair_sum(struct shim_pair pair) {
    return pair.value + pair.tag;
}

SHIM_EXPORT struct shim_pair shim_pair_make(std::int8_t tag,
                                            std::int64_t value) {
    struct shim_pair pair = {tag, value};
    return pair;
}

SHIM_EXPORT double shim_nested_score(struct shim_nested nested) {
    return nested.point.x + nested.point.y + nested.tag;
}

SHIM_EXPORT std::int32_t shim_words_sum(struct shim_words words) {
    return words.values[0] + words.values[1] + words.values[2];
}

SHIM_EXPORT const struct shim_point *shim_point_global(void) {
    static struct shim_point point = {3.0, 4.0};
    return &point;
}

SHIM_EXPORT std::int32_t shim_apply_cmp(shim_cmp_fn fn, std::int32_t a,
                                        std::int32_t b) {
    return fn(a, b);
}

SHIM_EXPORT std::int32_t shim_apply_cmp_twice(shim_cmp_fn fn, std::int32_t a,
                                              std::int32_t b) {
    return fn(a, b) - fn(b, a);
}

SHIM_EXPORT std::int32_t shim_apply_ptr_cmp(shim_ptr_cmp_fn fn,
                                            std::int32_t a,
                                            std::int32_t b) {
    return fn(&a, &b);
}

SHIM_EXPORT double shim_apply_point(shim_point_fn fn,
                                    struct shim_point point) {
    return fn(point);
}

SHIM_EXPORT const char *shim_apply_string(shim_string_fn fn,
                                          const char *text) {
    return fn(text);
}

SHIM_EXPORT std::int32_t shim_apply_void(shim_void_fn fn,
                                         std::int32_t value) {
    fn(value);
    return value;
}

static std::int32_t shim_free_counter = 0;

SHIM_EXPORT void *shim_alloc_point(void) {
    struct shim_point *point =
        (struct shim_point *)std::malloc(sizeof(struct shim_point));
    if (!point)
        return nullptr;
    point->x = 1.0;
    point->y = 2.0;
    return point;
}

SHIM_EXPORT void shim_free_point(void *pointer) {
    if (!pointer)
        return;
    ++shim_free_counter;
    std::free(pointer);
}

SHIM_EXPORT std::int32_t shim_free_count(void) { return shim_free_counter; }

SHIM_EXPORT void shim_consume_point(struct shim_point *point) {
    (void)point;
}

SHIM_EXPORT void shim_consume_free_point(struct shim_point *point) {
    if (!point)
        return;
    ++shim_free_counter;
    std::free(point);
}

SHIM_EXPORT void shim_spawn_notify(shim_notify_fn fn, std::int32_t value) {
    std::thread worker([fn, value]() { fn(value); });
    worker.join();
}

SHIM_EXPORT void shim_spawn_cmp(shim_cmp_fn fn, std::int32_t a,
                                std::int32_t b) {
    std::thread worker([fn, a, b]() { (void)fn(a, b); });
    worker.join();
}

static shim_cmp_fn shim_saved_cmp = nullptr;

SHIM_EXPORT void shim_save_cmp(shim_cmp_fn fn) { shim_saved_cmp = fn; }

SHIM_EXPORT std::int32_t shim_call_saved(std::int32_t a, std::int32_t b) {
    return shim_saved_cmp ? shim_saved_cmp(a, b) : 0;
}

static const char *shim_saved_text = nullptr;

SHIM_EXPORT void shim_save_text(const char *text) { shim_saved_text = text; }

SHIM_EXPORT const char *shim_saved_text_value(void) {
    return shim_saved_text;
}

static std::int32_t shim_subtract(std::int32_t a, std::int32_t b) {
    return a - b;
}

SHIM_EXPORT shim_cmp_fn shim_get_cmp(void) { return shim_subtract; }

SHIM_EXPORT void shim_churn_text(std::size_t size) {
    std::size_t begin = size > 16 ? size - 16 : 1;
    std::size_t end = size + 16;
    for (int pass = 0; pass < 4; ++pass) {
        for (std::size_t n = begin; n <= end; ++n) {
            void *block = std::malloc(n ? n : 1);
            if (!block)
                return;
            std::memset(block, 0x7f, n ? n : 1);
            std::free(block);
        }
    }
}

} // extern "C"
