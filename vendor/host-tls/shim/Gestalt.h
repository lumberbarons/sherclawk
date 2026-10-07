#ifndef CERTAINLY_HOST_SHIM_GESTALT_H
#define CERTAINLY_HOST_SHIM_GESTALT_H

#include <stdint.h>

typedef struct {
    uint8_t opaque[32];
} MachineLocation;

extern void ReadLocation(MachineLocation *loc);

#endif