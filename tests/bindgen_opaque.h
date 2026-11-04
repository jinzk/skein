#ifndef SKEIN_BINDGEN_OPAQUE_H
#define SKEIN_BINDGEN_OPAQUE_H

typedef struct OpaqueRecord {
    int value;
    union {
        int i;
        float f;
    } data;
} OpaqueRecord;

OpaqueRecord *opaque_record_new(void);
int opaque_record_take(OpaqueRecord value);
int opaque_record_read(OpaqueRecord *value);

#endif
