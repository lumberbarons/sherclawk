/* Startup Items adapter. The host-tested controller advances one bounded phase
 * per event-loop turn. Claims survive crashes; uncertain work is never replayed. */
#include "files.h"
#include "run.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Processes.h>
#include <Timer.h>
#include <AppleEvents.h>
#include <AERegistry.h>
#include <stdio.h>
#include <string.h>
static HRun run;
static char buffer[H_REPORT_CAP], mailbox[80], config[256];
static ProcessSerialNumber child;
static char trace_path[100];
static unsigned int trace_records;
static AEEventHandlerUPP open_upp;
static pascal OSErr opened(const AppleEvent *event,AppleEvent *reply,long refcon)
{
    (void)event; (void)reply; (void)refcon; return noErr;
}
static void trace(const char *event,int state)
{
    FILE *f;
    if (trace_records >= 128 || !trace_path[0]) return;
    f=fopen(trace_path,"a");
    if (!f) return;
    fprintf(f,"%lu %s state=%d\n",h_now(),event,state);
    fclose(f); trace_records++;
}
static unsigned long now(void *ctx) { (void)ctx; return h_now(); }
static int request(void *ctx,HRequest *r)
{
    char path[100];
    int n;
    (void)ctx;
    snprintf(path,sizeof(path),"%srequest",mailbox);
    n=h_read(path,buffer,H_REQUEST_CAP);
    return n >= 0 && h_request_parse(buffer,(size_t)n,r);
}
static int claim(void *ctx,const HRequest *r)
{
    char path[100], claimed[100];
    FSSpec source,dest;
    HRequest claimed_request;
    int n;
    (void)ctx;
    snprintf(path,sizeof(path),"%srequest",mailbox);
    h_mailpath(claimed,sizeof(claimed),r,"claim");
    if (h_spec(path,&source) || h_spec(claimed,&dest) != fnfErr || FSpRename(&source,dest.name)) return 0;
    if (FlushVol(NULL,source.vRefNum)) return -1;
    n=h_read(claimed,buffer,H_REQUEST_CAP);
    return n >= 0 && h_request_parse(buffer,(size_t)n,&claimed_request) &&
        h_request_equal(r,&claimed_request) ? 1 : -1;
}
static int configure(void *ctx,const HRequest *r)
{
    int n=h_request_format(r,buffer,sizeof(buffer));
    (void)ctx;
    return n > 0 && h_publish(config,buffer,(size_t)n);
}
static int already_running(const FSSpec *app)
{
    ProcessSerialNumber psn={0,kNoProcess};
    ProcessInfoRec info;
    FSSpec found;
    OSErr err;
    int i;
    for (i=0; i<128; i++) {
        err=GetNextProcess(&psn);
        if (err == procNotFound) return 0;
        if (err) return -1;
        memset(&info,0,sizeof(info)); info.processInfoLength=sizeof(info); info.processAppSpec=&found;
        if (GetProcessInformation(&psn,&info)) return -1;
        if (found.vRefNum == app->vRefNum && found.parID == app->parID &&
            found.name[0] == app->name[0] && !memcmp(found.name+1,app->name+1,app->name[0])) return 1;
    }
    return -1;
}
static int launch(void *ctx,const HRequest *r)
{
    FSSpec app;
    FInfo finder;
    LaunchParamBlockRec pb;
    (void)ctx;
    if (h_spec(r->application,&app) || FSpGetFInfo(&app,&finder) ||
        finder.fdType != 'APPL' || (finder.fdFlags & 0x8000) || already_running(&app)) return 0;
    memset(&pb,0,sizeof(pb)); pb.launchBlockID=extendedBlock;
    pb.launchEPBLength=extendedBlockLen; pb.launchControlFlags=launchContinue|launchNoFileFlags|launchDontSwitch;
    pb.launchAppSpec=&app;
    if (LaunchApplication(&pb)) return 0;
    child=pb.launchProcessSN; return 1;
}
static int report(void *ctx,const HRequest *r,HReport *out)
{
    char path[100];
    int n;
    (void)ctx;
    h_mailpath(path,sizeof(path),r,"report"); n=h_read(path,buffer,sizeof(buffer));
    if (n == -2) return 0;
    return n >= 0 && h_report_parse(buffer,(size_t)n,r,out) ? 1 : -1;
}
static int present(void *ctx)
{
    ProcessInfoRec info;
    OSErr err;
    (void)ctx;
    memset(&info,0,sizeof(info)); info.processInfoLength=sizeof(info);
    err=GetProcessInformation(&child,&info);
    return !err ? 1 : err == procNotFound ? 0 : -1;
}
static int stopped(void *ctx,const HRequest *r)
{
    char path[100];
    HRequest stop;
    int n;
    (void)ctx;
    h_mailpath(path,sizeof(path),r,"stop"); n=h_read(path,buffer,H_REQUEST_CAP);
    return n >= 0 && h_request_parse(buffer,(size_t)n,&stop) && h_request_equal(r,&stop);
}
static int publish(void *ctx,const HReport *r)
{
    char path[100];
    int n=h_report_format(r,buffer,sizeof(buffer));
    (void)ctx;
    h_mailpath(path,sizeof(path),&r->request,"result");
    return n > 0 && h_publish(path,buffer,(size_t)n);
}
static int cleanup(void *ctx) { (void)ctx; return h_delete(config); }
int main(void)
{
    char identity[256], ready[128], path[100], boot[33], slot[6];
    UnsignedWide micros;
    unsigned long started, heartbeat=0;
    ProcessSerialNumber self;
    EventRecord event;
    int n, ready_published=0, last_state=-1;
    HRunHost host={NULL,now,request,claim,configure,launch,report,present,stopped,publish,cleanup};
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    open_upp=NewAEEventHandlerUPP(opened);
    if (!open_upp || AEInstallEventHandler(kCoreEventClass,kAEOpenApplication,open_upp,0,false)) return 1;
    if (!h_private_path("Harness slot",identity,sizeof(identity)) ||
        !h_private_path("Harness active",config,sizeof(config))) return 1;
    n=h_read(identity,buffer,sizeof(buffer));
    if (n != 6 || (strcmp(buffer,"slot1\r") && strcmp(buffer,"slot2\r"))) return 1;
    memcpy(slot,buffer,5); slot[5]=0;
    /* Leftover private config means an earlier controller/launch was uncertain. */
    if (h_read(config,buffer,sizeof(buffer)) != -2) return 1;
    Microseconds(&micros); started=h_now();
    if (GetCurrentProcess(&self)) return 1;
    snprintf(boot,sizeof(boot),"%08lx%08lx%08lx%08lx",started,(unsigned long)micros.hi,(unsigned long)micros.lo,(unsigned long)self.lowLongOfPSN);
    h_run_init(&run,slot,boot);
    snprintf(mailbox,sizeof(mailbox),"Retro68:Harness:%s:",slot);
    snprintf(path,sizeof(path),"%sready",mailbox);
    snprintf(trace_path,sizeof(trace_path),"Retro68:Harness:%s:log-%.20s",slot,boot);
    for (;;) {
        WaitNextEvent(everyEvent,&event,15,NULL);
        if (event.what == kHighLevelEvent) AEProcessAppleEvent(&event);
        if (run.state == H_QUARANTINED) {
            if (ready_published) { h_delete(path); ready_published=0; }
            continue;
        }
        if (!ready_published || h_now() != heartbeat) {
            heartbeat=h_now();
            if (!ready_published && heartbeat-started > 180) return 1;
            snprintf(ready,sizeof(ready),"READY/1\r%s\r%s\r%lu\rEND\r",slot,boot,heartbeat);
            if (!h_delete(path)) continue; /* readiness is replaceable; results are not */
            if (!ready_published) trace("ready_begin",run.state);
            ready_published=h_publish(path,ready,strlen(ready));
            if (last_state < 0) trace("ready_end",ready_published);
            if (!ready_published) continue;
        }
        if (last_state != run.state) { trace("phase",run.state); last_state=run.state; }
        h_run_step(&run,&host);
    }
}
