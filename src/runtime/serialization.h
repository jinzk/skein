#ifndef SKEIN_SERIALIZATION_H
#define SKEIN_SERIALIZATION_H

struct value_t;
struct enum_def;

struct value_t *enum_decode_value(struct value_t *value);
struct value_t *enum_decode_value_ex(struct value_t *value,
                                     struct enum_def *expected,
                                     bool defaults);
struct value_t *struct_decode_value(struct value_t *value,
                                    struct value_t *type_value);
struct value_t *struct_decode_value_ex(struct value_t *value,
                                       struct value_t *type_value,
                                       bool defaults);

#endif
