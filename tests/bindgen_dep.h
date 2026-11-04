#ifndef SKEIN_BINDGEN_DEP_H
#define SKEIN_BINDGEN_DEP_H

typedef struct DepPoint {
    double x;
    double y;
} DepPoint;

typedef unsigned long DepSize;

enum DepMode {
    DEP_MODE_A = 1,
    DEP_MODE_B = 2
};

#endif
