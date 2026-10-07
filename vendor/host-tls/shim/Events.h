#ifndef CERTAINLY_HOST_SHIM_EVENTS_H
#define CERTAINLY_HOST_SHIM_EVENTS_H

#include <stdint.h>

typedef struct {
    int16_t v;
    int16_t h;
} Point;

extern unsigned long TickCount(void);
extern void GetMouse(Point *pt);

#endif