/*
 * host_shim.c — Toolbox/OT stubs for the host-test rig.
 *
 * Implements the handful of Mac OS calls Certainly's core makes, using
 * the host clock. Values only need to be plausible, not accurate: they
 * feed entropy gathering and timeout bookkeeping.
 */
#include <time.h>
#include <string.h>

#include <OpenTransport.h>
#include <Timer.h>
#include <Events.h>
#include <LowMem.h>
#include <Gestalt.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

unsigned long TickCount(void)
{
    return (unsigned long)(now_ns() / 16666667ull);   /* 60 ticks/sec */
}

void Microseconds(UnsignedWide *usecs)
{
    uint64_t t = now_ns() / 1000ull;
    usecs->hi = (uint32_t)(t >> 32);
    usecs->lo = (uint32_t)t;
}

void OTGetTimeStamp(OTTimeStamp *ts)
{
    uint64_t t = now_ns();
    ts->hi = (uint32_t)(t >> 32);
    ts->lo = (uint32_t)t;
}

void GetMouse(Point *pt)
{
    pt->v = 0;
    pt->h = 0;
}

uint32_t LMGetTicks(void)
{
    return (uint32_t)TickCount();
}

void ReadLocation(MachineLocation *loc)
{
    uint64_t t = now_ns();
    memset(loc, 0, sizeof(*loc));
    memcpy(loc->opaque, &t, sizeof(t));
}