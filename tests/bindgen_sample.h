#ifndef SKEIN_BINDGEN_SAMPLE_H
#define SKEIN_BINDGEN_SAMPLE_H

#define SAMPLE_MAX 16
#define SAMPLE_PI 3.5
#define SAMPLE_NAME "sample"
#define SAMPLE_SHIFT (1 << 4)
#define SAMPLE_MASK ((1 << 3) | 1)

enum SampleKind {
    SAMPLE_KIND_A = 1 << 2,
    SAMPLE_KIND_B = SAMPLE_KIND_A | 1,
    SAMPLE_KIND_C
};

typedef struct SamplePoint {
    double x;
    double y;
} SamplePoint;

typedef struct {
    int32_t tag;
    SamplePoint point;
    unsigned char flags[4];
} SampleRecord;

typedef int (*SampleCompare)(const void *left, const void *right);

int sample_add(int left, int right);
double sample_len(SamplePoint point);
SamplePoint sample_make(double x, double y);
void sample_sort(void *base, size_t count, SampleCompare cmp);
const char *sample_name(void);
int32_t sample_flags(const SampleRecord *record);

#endif
