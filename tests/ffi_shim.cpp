#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#define SHIM_EXPORT __declspec(dllexport)
#else
#define SHIM_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

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

} // extern "C"
