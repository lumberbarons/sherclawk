#ifndef CERTAINLY_HOST_SHIM_TIMER_H
#define CERTAINLY_HOST_SHIM_TIMER_H

#include <stdint.h>

typedef struct {
    uint32_t hi;
    uint32_t lo;
} UnsignedWide;

extern void Microseconds(UnsignedWide *usecs);

#endif