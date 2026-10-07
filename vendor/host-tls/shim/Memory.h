#ifndef CERTAINLY_HOST_SHIM_MEMORY_H
#define CERTAINLY_HOST_SHIM_MEMORY_H

#include <stdlib.h>

typedef char *Ptr;

static inline Ptr NewPtrClear(long size)
{
    return (Ptr)calloc(1, (size_t)size);
}

static inline void DisposePtr(Ptr p)
{
    free(p);
}

#endif