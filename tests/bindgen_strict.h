#ifndef SKEIN_BINDGEN_STRICT_H
#define SKEIN_BINDGEN_STRICT_H

int strict_variadic(int value, ...);
void __stdcall strict_stdcall(int value);
long double strict_long_double(void);

struct StrictUnknown {
    unknown_type value;
};

struct StrictAlignas {
    _Alignas(16) int value;
};

#endif
