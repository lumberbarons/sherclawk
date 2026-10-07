#ifndef CERTAINLY_HOST_SHIM_OPENTPTINTERNET_H
#define CERTAINLY_HOST_SHIM_OPENTPTINTERNET_H

#include <stdint.h>

typedef uint32_t InetHost;

typedef struct {
    InetHost name;
    InetHost addrs[10];
} InetHostInfo;

#endif