#ifndef SKEIN_BINDGEN_SAMPLE_H
#define SKEIN_BINDGEN_SAMPLE_H

#define SAMPLE_MAX 16
#define SAMPLE_PI 3.5
#define SAMPLE_NAME "sample"
#define SAMPLE_SHIFT (1 << 4)
#define SAMPLE_MASK ((1 << 3) | 1)
#define SAMPLE_COMPARE (1 < 2)
#define SAMPLE_LOGICAL (1 && 0)
#define SAMPLE_TERNARY (1 ? 7 : 9)
#define SAMPLE_CAST ((unsigned char)0x1ff)
#define SAMPLE_BINARY 0b101
#define SAMPLE_FLOAT_SUFFIX 2.5f

enum SampleKind {
    SAMPLE_KIND_A = 1 << 2,
    SAMPLE_KIND_B = SAMPLE_KIND_A | 1,
    SAMPLE_KIND_C,
    SAMPLE_KIND_D = (1 != 2) ? 7 : 8
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
