/* Guest calibration for the host cost counters (tests/cost.h): ticks per AFP
 * open/read/close, per catalog lookup and per idle event turn on the real
 * Retro68: volume. Multiply a host count by the matching figure to estimate
 * guest time. Writes Retro68:SherclawkCalibrateCheck.log, deletes its scratch
 * file and quits. Run it with the guest otherwise idle. */
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
#define OPS 200
#define TURNS 300
#define PAGE 1024
#define SLOW_OPS 20
static FILE *logfile;
static int failures;
static FSSpec scratch;
static char page[PAGE];
static void report(const char *name, unsigned long ticks, int n)
{
    fprintf(logfile, "CALIBRATE %s ticks=%lu n=%d\n", name, ticks, n);
    fflush(logfile);
}
static void need(OSErr err, const char *what)
{
    if (err) { fprintf(logfile, "FAIL %s err=%d\n", what, (int)err); failures++; }
}
static unsigned long open_close(void)
{
    unsigned long start = TickCount();
    short ref;
    int i;
    for (i = 0; i < OPS; i++) {
        need(FSpOpenDF(&scratch, fsRdPerm, &ref), "open");
        need(FSClose(ref), "close");
    }
    return TickCount() - start;
}
static unsigned long open_read_close(void)
{
    unsigned long start = TickCount();
    short ref;
    long n;
    int i;
    for (i = 0; i < OPS; i++) {
        n = PAGE;
        need(FSpOpenDF(&scratch, fsRdPerm, &ref), "open");
        need(FSRead(ref, &n, page), "read");
        need(FSClose(ref), "close");
    }
    return TickCount() - start;
}
static unsigned long reads_on_open_file(void)
{
    unsigned long start;
    short ref;
    long n;
    int i;
    need(FSpOpenDF(&scratch, fsRdPerm, &ref), "open");
    start = TickCount();
    for (i = 0; i < OPS; i++) {
        n = PAGE;
        need(SetFPos(ref, fsFromStart, 0), "seek");
        need(FSRead(ref, &n, page), "read");
    }
    start = TickCount() - start;
    need(FSClose(ref), "close");
    return start;
}
static unsigned long catalog_lookups(void)
{
    unsigned long start = TickCount();
    CInfoPBRec pb;
    int i;
    for (i = 0; i < OPS; i++) {
        memset(&pb, 0, sizeof(pb));
        pb.hFileInfo.ioNamePtr = scratch.name;
        pb.hFileInfo.ioVRefNum = scratch.vRefNum;
        pb.hFileInfo.ioDirID = scratch.parID;
        need(PBGetCatInfoSync(&pb), "catalog");
    }
    return TickCount() - start;
}
static unsigned long writes_on_open_file(void)
{
    unsigned long start;
    short ref;
    long n;
    int i;
    need(FSpOpenDF(&scratch, fsRdWrPerm, &ref), "open for write");
    start = TickCount();
    for (i = 0; i < OPS; i++) {
        n = PAGE;
        need(SetFPos(ref, fsFromStart, 0), "seek");
        need(FSWrite(ref, &n, page), "write");
    }
    start = TickCount() - start;
    need(FSClose(ref), "close");
    return start;
}
static unsigned long flushes(void)
{
    unsigned long start = TickCount();
    int i;
    for (i = 0; i < SLOW_OPS; i++) need(FlushVol(NULL, scratch.vRefNum), "flush");
    return TickCount() - start;
}
/* Creating and deleting a file in the workspace folder, as staging does per input. */
static unsigned long create_delete(void)
{
    unsigned long start = TickCount();
    FSSpec spec = scratch;
    int i;
    spec.name[++spec.name[0]] = 'x';
    for (i = 0; i < SLOW_OPS; i++) {
        need(FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript), "create");
        need(FSpDelete(&spec), "delete");
    }
    return TickCount() - start;
}
/* The shipping loop sleeps 1 tick while a run is busy and 10 when idle. */
static unsigned long event_turns(long sleep, int turns)
{
    unsigned long start = TickCount();
    EventRecord event;
    int i;
    for (i = 0; i < turns; i++) {
        WaitNextEvent(everyEvent, &event, sleep, NULL);
        SystemTask();
    }
    return TickCount() - start;
}
int main(void)
{
    char full[256];
    Str255 native;
    short ref;
    long n;
    unsigned long write_ticks, flush_ticks, create_ticks, open_close_ticks, triple_ticks, read_ticks, catalog_ticks, busy_ticks, idle_ticks, zero_ticks;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile = fopen(SHERCLAWK_WORKSPACE "SherclawkCalibrateCheck.log", "w");
    if (!logfile) return 1;
    snprintf(full, sizeof(full), "%sSherclawk Calibrate %08lx", SHERCLAWK_WORKSPACE, (unsigned long)TickCount());
    native[0] = (unsigned char)strlen(full); memcpy(native + 1, full, native[0]);
    if (FSMakeFSSpec(0, 0, native, &scratch) != fnfErr || FSpCreate(&scratch, 'ttxt', 'TEXT', smSystemScript)) {
        fprintf(logfile, "FAIL scratch create\nRESULT failures=1\n"); fclose(logfile); return 1;
    }
    memset(page, 'x', sizeof(page)); n = PAGE;
    need(FSpOpenDF(&scratch, fsRdWrPerm, &ref), "open for write");
    need(FSWrite(ref, &n, page), "write");
    need(FSClose(ref), "close after write");
    FlushVol(NULL, scratch.vRefNum);
    open_close_ticks = open_close();
    read_ticks = reads_on_open_file();
    triple_ticks = open_read_close();
    catalog_ticks = catalog_lookups();
    write_ticks = writes_on_open_file();
    flush_ticks = flushes();
    create_ticks = create_delete();
    zero_ticks = event_turns(0, TURNS);
    busy_ticks = event_turns(1, TURNS);
    idle_ticks = event_turns(10, TURNS / 10);
    report("open_close", open_close_ticks, OPS);
    report("seek_read_1KiB_on_open_file", read_ticks, OPS);
    report("open_read_1KiB_close", triple_ticks, OPS);
    report("catalog_lookup", catalog_ticks, OPS);
    report("seek_write_1KiB_on_open_file", write_ticks, OPS);
    report("flush_volume", flush_ticks, SLOW_OPS);
    report("create_delete", create_ticks, SLOW_OPS);
    report("event_turn_sleep0", zero_ticks, TURNS);
    report("event_turn_sleep1_busy", busy_ticks, TURNS);
    report("event_turn_sleep10_idle", idle_ticks, TURNS / 10);
    fprintf(logfile, "scratch=%s deleted=%d\n", full, FSpDelete(&scratch) == 0);
    fprintf(logfile, "RESULT failures=%d\n", failures);
    fflush(logfile); FlushVol(NULL, 0); fclose(logfile);
    return failures ? 1 : 0;
}
