#ifndef SKEIN_BINDGEN_TYPES_H
#define SKEIN_BINDGEN_TYPES_H

#include <stdint.h>

void type_bytes(const uint8_t *data, unsigned char *out, int length);
void type_text(const char *text);
void type_array(int values[4]);
void type_matrix(int values[][4]);

#endif
