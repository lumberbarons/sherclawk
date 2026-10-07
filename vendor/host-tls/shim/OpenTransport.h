/*
 * Host-test shim for <OpenTransport.h>.
 *
 * Provides only the surface that Certainly's non-transport sources
 * reference, so the TLS 1.3 stack can run on macOS driven by the
 * BSD-socket transport in ../host_transport.c.
 */
#ifndef CERTAINLY_HOST_SHIM_OPENTRANSPORT_H
#define CERTAINLY_HOST_SHIM_OPENTRANSPORT_H

#include <stdint.h>

typedef int32_t OSStatus;
typedef int32_t OTResult;

typedef void *EndpointRef;
typedef void *InetSvcRef;

enum { noErr = 0 };

typedef struct {
    uint32_t hi;
    uint32_t lo;
} OTTimeStamp;

extern unsigned long TickCount(void);
extern void OTGetTimeStamp(OTTimeStamp *ts);

#endif