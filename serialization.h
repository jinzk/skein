#ifndef SKEIN_SERIALIZATION_H
#define SKEIN_SERIALIZATION_H

struct value_t;

struct value_t *enum_decode_value(struct value_t *value);
struct value_t *struct_decode_value(struct value_t *value,
                                    struct value_t *type_value);

#endif
