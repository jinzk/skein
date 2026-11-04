#ifndef SKEIN_BINDGEN_STRICT_H
#define SKEIN_BINDGEN_STRICT_H

int strict_unused(int value);
void __vectorcall strict_vectorcall(int value);
long double strict_long_double(void);

struct StrictUnknown {
    unknown_type value;
};

struct StrictAlignas {
    _Alignas(16) int value;
};

_Thread_local int strict_tls;

#endif
