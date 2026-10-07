/* Standalone ToolServer wire-protocol spike. No MacRelix code is linked/copied.
 * CR/MacRoman scripts travel in misc/dosc events; replies arrive via aevt/ansr.
 * Stop/timeouts never resend: retain and drain the outstanding return ID.
 * Fixed sources are native TEXT files in a fresh retained AFP fixture.
 */
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Processes.h>
#include <AppleEvents.h>
#include <AERegistry.h>
#include <Files.h>
#include <Script.h>
#include <Resources.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define TEXT_LIMIT 8192
static FILE *logfile;
static WindowPtr window;
static ProcessSerialNumber server;
static char fixture[160], status_line[240];
static char output[TEXT_LIMIT+1], diagnostic[TEXT_LIMIT+1];
static AEEventHandlerUPP answer_upp, open_upp;
static short next_id=100, pending_id;
static int waiting, abandoned, received, malformed, failures, phase, quit;
static SInt32 tool_status;
static uint32_t sent_at, deadline, last_tick, loops, updates, max_gap;

static void record(const char *fmt,...)
{
    va_list args; va_start(args,fmt); vfprintf(logfile,fmt,args); va_end(args);
    fputc('\n',logfile); fflush(logfile);
}
static void message(const char *fmt,...)
{
    va_list args; va_start(args,fmt);
    vsnprintf(status_line,sizeof(status_line),fmt,args); va_end(args);
    if(window) { Rect r=window->portRect; InvalRect(&r); }
}
static void draw(void)
{
    const char *lines[]={status_line,"R: success/error suite   L: long Rez command",
        "S / Cmd-period: stop observing   T: expire deadline",
        "K: request ToolServer quit   Cmd-Q: leave diagnostic"};
    int i; Str255 text;
    SetPort(window); EraseRect(&window->portRect);
    for(i=0;i<4;i++) { size_t n=strlen(lines[i]); text[0]=(unsigned char)n;
        memcpy(text+1,lines[i],n); MoveTo(12,25+i*23); DrawString(text); }
}
static int find_server(ProcessSerialNumber *psn)
{
    ProcessInfoRec p; FSSpec spec; Str255 name;
    psn->highLongOfPSN=0; psn->lowLongOfPSN=kNoProcess;
    while(GetNextProcess(psn)==noErr) {
        memset(&p,0,sizeof(p)); p.processInfoLength=sizeof(p);
        p.processName=name; p.processAppSpec=&spec;
        if(!GetProcessInformation(psn,&p) && p.processSignature=='MPSX') {
            record("ToolServer process=%lu:%lu app=%.*s",(unsigned long)psn->highLongOfPSN,
                (unsigned long)psn->lowLongOfPSN,spec.name[0],spec.name+1); return 0;
        }
    }
    return -1;
}
static OSErr find_or_launch(void)
{
    HParamBlockRec v; DTPBRec dt; Str255 volume,name; FSSpec spec;
    LaunchParamBlockRec launch; short index; OSErr e=fnfErr;
    if(!find_server(&server)) return noErr;
    record("ToolServer absent; searching mounted desktop databases");
    for(index=1;index<64;index++) {
        memset(&v,0,sizeof(v)); v.volumeParam.ioNamePtr=volume;
        v.volumeParam.ioVolIndex=index;
        if(PBHGetVInfoSync(&v)) break;
        memset(&dt,0,sizeof(dt)); dt.ioVRefNum=v.volumeParam.ioVRefNum;
        if(PBDTGetPath(&dt)) continue;
        dt.ioNamePtr=name; dt.ioIndex=0; dt.ioFileCreator='MPSX';
        e=PBDTGetAPPLSync(&dt); if(e) continue;
        memset(&spec,0,sizeof(spec)); spec.vRefNum=v.volumeParam.ioVRefNum;
        spec.parID=dt.ioAPPLParID; memcpy(spec.name,name,(size_t)name[0]+1);
        record("launch ToolServer volume=%d parent=%ld app=%.*s",spec.vRefNum,
            spec.parID,name[0],name+1);
        memset(&launch,0,sizeof(launch)); launch.launchBlockID=extendedBlock;
        launch.launchEPBLength=extendedBlockLen;
        launch.launchControlFlags=launchContinue | launchNoFileFlags | launchDontSwitch;
        launch.launchAppSpec=&spec; e=LaunchApplication(&launch);
        if(!e) { server=launch.launchProcessSN; return noErr; }
    }
    return e ? e : fnfErr;
}
static int fixture_file(const char *name,const char *bytes)
{
    char path[256]; Str255 p; FSSpec s; short ref; long n=(long)strlen(bytes);
    OSErr e,closed;
    snprintf(path,sizeof(path),"%s%s",fixture,name);
    p[0]=(unsigned char)strlen(path); memcpy(p+1,path,p[0]);
    e=FSMakeFSSpec(0,0,p,&s); if(e!=fnfErr) return -1;
    e=FSpCreate(&s,'ttxt','TEXT',smSystemScript); if(e) return -1;
    e=FSpOpenDF(&s,fsWrPerm,&ref); if(e) return -1;
    e=FSWrite(ref,&n,bytes); closed=FSClose(ref);
    return e || closed || n!=(long)strlen(bytes) || FlushVol(NULL,s.vRefNum) ? -1 : 0;
}
/* Read only bounded text. A missing optional text parameter means empty text;
 * oversize/incorrect-type replies fail the diagnostic, without allocating copies. */
static OSErr text_param(const AppleEvent *event,AEKeyword key,char *buf)
{
    DescType type=typeNull; Size size=0,got=0; OSErr e=AESizeOfParam(event,key,&type,&size);
    buf[0]=0;
    if(e==errAEDescNotFound) return noErr;
    if(e || type!=typeChar || size<0 || size>TEXT_LIMIT) {
        record("text parameter %08lx error=%d type=%08lx bytes=%ld limit=%d",
            (unsigned long)key,e,(unsigned long)type,(long)size,TEXT_LIMIT);
        return e ? e : paramErr;
    }
    e=AEGetParamPtr(event,key,typeChar,&type,buf,TEXT_LIMIT,&got);
    if(e || got!=size) return e ? e : paramErr;
    buf[got]=0; return noErr;
}
static pascal OSErr answer(const AppleEvent *event,AppleEvent *reply,long refcon)
{
    DescType type; Size got; short id=0; ProcessSerialNumber sender;
    OSErr e; (void)reply; (void)refcon;
    e=AEGetAttributePtr(event,keyReturnIDAttr,typeSInt16,&type,&id,sizeof(id),&got);
    if(e || got!=sizeof(id) || !waiting || id!=pending_id) {
        record("ignored reply return_id=%d error=%d waiting=%d expected=%d",id,e,waiting,pending_id);
        return noErr;
    }
    e=AEGetAttributePtr(event,keyAddressAttr,typeProcessSerialNumber,&type,&sender,sizeof(sender),&got);
    if(e || got!=sizeof(sender) || sender.highLongOfPSN!=server.highLongOfPSN ||
        sender.lowLongOfPSN!=server.lowLongOfPSN) {
        record("ignored reply sender error=%d bytes=%ld",e,(long)got); return noErr;
    }
    malformed=0; tool_status=-999;
    e=AEGetParamPtr(event,'stat',typeSInt32,&type,&tool_status,sizeof(tool_status),&got);
    if(e || got!=sizeof(tool_status)) malformed=1;
    if(text_param(event,keyDirectObject,output) || text_param(event,'diag',diagnostic)) malformed=1;
    record("reply id=%d stat=%ld malformed=%d abandoned=%d elapsed_ticks=%lu loops=%lu updates=%lu max_gap=%lu",
        id,(long)tool_status,malformed,abandoned,(unsigned long)(TickCount()-sent_at),
        (unsigned long)loops,(unsigned long)updates,(unsigned long)max_gap);
    record("stdout=[%s]",output); record("diag=[%s]",diagnostic);
    waiting=0; received=1; return noErr;
}
static pascal OSErr opened(const AppleEvent *event,AppleEvent *reply,long refcon)
{ (void)event; (void)reply; (void)refcon; return noErr; }
/* Construct malformed replies locally in the guest to exercise the same
 * handler used for ToolServer replies. These are never sent to another app. */
static int reply_checks(void)
{
    AEAddressDesc address={typeNull,NULL}; AppleEvent event={typeNull,NULL},reply={typeNull,NULL};
    static char oversized[TEXT_LIMIT+1];
    SInt32 zero=0; ProcessSerialNumber wrong={0,0}; int bad=0; OSErr e;
    record("reply_checks begin (synthetic local events)");
    e=AECreateDesc(typeProcessSerialNumber,&server,sizeof(server),&address);
    if(!e)e=AECreateAppleEvent(kCoreEventClass,kAEAnswer,&address,300,kAnyTransactionID,&event);
    if(e) { bad=1; goto done; }
    pending_id=301; waiting=1; received=0;
    answer(&event,&reply,0); if(!waiting || received)bad++;
    pending_id=300;
    e=AEPutAttributePtr(&event,keyAddressAttr,typeProcessSerialNumber,&wrong,sizeof(wrong));
    if(e) { bad++; goto done; }
    answer(&event,&reply,0); if(!waiting || received)bad++;
    e=AEPutAttributePtr(&event,keyAddressAttr,typeProcessSerialNumber,&server,sizeof(server));
    if(e) { bad++; goto done; }
    answer(&event,&reply,0); if(waiting || !received || !malformed)bad++;
    e=AEPutParamPtr(&event,'stat',typeSInt32,&zero,sizeof(zero));
    if(!e)e=AEPutParamPtr(&event,keyDirectObject,typeSInt32,&zero,sizeof(zero));
    if(e) { bad++; goto done; }
    waiting=1; received=0; answer(&event,&reply,0);
    if(waiting || !received || !malformed)bad++;
    memset(oversized,'x',sizeof(oversized));
    e=AEPutParamPtr(&event,keyDirectObject,typeChar,oversized,sizeof(oversized));
    if(e) { bad++; goto done; }
    waiting=1; received=0; answer(&event,&reply,0);
    if(waiting || !received || !malformed)bad++;
done:
    waiting=received=malformed=0; pending_id=0;
    AEDisposeDesc(&reply); AEDisposeDesc(&event); AEDisposeDesc(&address);
    record("reply_checks failures=%d wrong_id wrong_sender missing_stat wrong_text_type oversize",bad);
    return bad;
}
static OSErr send_event(AEEventClass cls,AEEventID event_id,const char *script,short id,AESendMode mode)
{
    AEAddressDesc address={typeNull,NULL}; AppleEvent event={typeNull,NULL},reply={typeNull,NULL};
    OSErr e=AECreateDesc(typeProcessSerialNumber,&server,sizeof(server),&address);
    if(!e) e=AECreateAppleEvent(cls,event_id,&address,id,kAnyTransactionID,&event);
    if(!e && script) e=AEPutParamPtr(&event,keyDirectObject,typeChar,script,(Size)strlen(script));
    if(!e) e=AESend(&event,&reply,mode | kAENeverInteract,kAENormalPriority,60,NULL,NULL);
    AEDisposeDesc(&reply); AEDisposeDesc(&event); AEDisposeDesc(&address); return e;
}
static int send_command(int which)
{
    char script[4096],command[3072]; ProcessSerialNumber front; OSErr e;
    if(waiting) return -1;
    e=find_or_launch(); if(e) { record("FAIL ToolServer discovery/launch=%d",e); return -1; }
    if(which==2) {
        int i; command[0]=0;
        /* Explicit repetition keeps this spike independent of MPW Loop syntax. */
        for(i=0;i<40;i++) strcat(command,"Rez good.r -o slow.rsrc\r");
        strcat(command,"Rez good.r -o slow.rsrc");
    }
    else snprintf(command,sizeof(command),"Rez %s.r -o %s.rsrc",which ? "bad" : "good",which ? "bad" : "good");
    snprintf(script,sizeof(script),"Set Exit 0\rDirectory \"%s\"\r%s < Dev:Null\rSet CommandStatus {Status}\rDirectory \"{MPW}\"\rExit {CommandStatus}\r",fixture,command);
    pending_id=++next_id; abandoned=received=malformed=0; tool_status=-999;
    output[0]=diagnostic[0]=0; sent_at=(uint32_t)TickCount(); deadline=sent_at+120UL*60UL;
    last_tick=sent_at; loops=updates=max_gap=0;
    e=send_event(kAEMiscStandards,kAEDoScript,script,pending_id,kAEQueueReply);
    GetFrontProcess(&front);
    record("send kind=%d id=%d error=%d duration_ticks=%lu front=%lu:%lu script=[%s]",which,
        pending_id,e,(unsigned long)(TickCount()-sent_at),(unsigned long)front.highLongOfPSN,
        (unsigned long)front.lowLongOfPSN,script);
    if(e) { record("RESULT failures=1 send_error=%d",e); return -1; }
    waiting=1; message("Waiting for ToolServer reply %d (kind %d).",pending_id,which); return 0;
}
static void abandon(const char *why)
{
    if(!waiting || abandoned) return;
    abandoned=1; phase=3;
    record("UNKNOWN id=%d reason=%s; no resend; retain reply correlation",pending_id,why);
    message("Unknown outcome (%s); waiting to drain reply %d.",why,pending_id);
}
static int verify_resource(void)
{
    char full[256]; Str255 p; FSSpec spec; short file,previous=CurResFile();
    Handle resource; int ok=0; const char expected[]="native async lobster";
    snprintf(full,sizeof(full),"%sgood.rsrc",fixture);
    p[0]=(unsigned char)strlen(full); memcpy(p+1,full,p[0]);
    if(FSMakeFSSpec(0,0,p,&spec))return -1;
    file=FSpOpenResFile(&spec,fsRdPerm); if(file==-1)return -1;
    UseResFile(file); resource=Get1Resource('STR ',128);
    if(resource) {
        HLock(resource);
        ok=GetHandleSize(resource)==sizeof(expected) &&
            (unsigned char)(*resource)[0]==sizeof(expected)-1 &&
            !memcmp(*resource+1,expected,sizeof(expected)-1);
        HUnlock(resource); ReleaseResource(resource);
    }
    CloseResFile(file); UseResFile(previous);
    record("resource STR/128 exact=%d",ok); return ok ? 0 : -1;
}
int main(void)
{
    EventRecord event; Rect bounds; Str255 path; FSSpec folder;
    long dir; char full[256]; OSErr e; uint32_t now;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkToolServerCheck.log","w"); if(!logfile) return 1;
    if(snprintf(fixture,sizeof(fixture),SHERCLAWK_WORKSPACE "ToolServerCheck%08lx:",
        (unsigned long)TickCount())>=(int)sizeof(fixture)) {
        record("RESULT failures=1 fixture_path_limit"); fclose(logfile); return 1;
    }
    strcpy(full,fixture); full[strlen(full)-1]=0;
    path[0]=(unsigned char)strlen(full); memcpy(path+1,full,path[0]);
    e=FSMakeFSSpec(0,0,path,&folder);
    if(e!=fnfErr || FSpDirCreate(&folder,smSystemScript,&dir) ||
        fixture_file("good.r","type 'STR ' { pstring; };\rresource 'STR ' (128) { \"native async lobster\" };\r") ||
        fixture_file("bad.r","type 'STR ' { pstring; };\rresource 'STR ' (128) { deliberate_invalid_token };\r")) {
        record("RESULT failures=1 fixture_setup error=%d",e); fclose(logfile); return 1;
    }
    record("fixture=%s text_limit=%d",fixture,TEXT_LIMIT);
    answer_upp=NewAEEventHandlerUPP(answer); open_upp=NewAEEventHandlerUPP(opened);
    e=AEInstallEventHandler(kCoreEventClass,kAEAnswer,answer_upp,0,false);
    if(!e) e=AEInstallEventHandler(kCoreEventClass,kAEOpenApplication,open_upp,0,false);
    if(e) { record("RESULT failures=1 handler=%d",e); fclose(logfile); return 1; }
    SetRect(&bounds,40,80,620,225);
    window=NewWindow(NULL,&bounds,(const unsigned char *)"\031Sherclawk ToolServerCheck",true,documentProc,(WindowPtr)-1,false,0);
    if(send_command(0)) { failures++; phase=3; record("RESULT failures=1 discovery_or_send"); message("ToolServer unavailable; R retries discovery explicitly."); }
    while(!quit) {
        WaitNextEvent(everyEvent,&event,1,NULL); now=(uint32_t)TickCount();
        if(waiting) { uint32_t gap=now-last_tick; if(gap>max_gap)max_gap=gap; last_tick=now; loops++; }
        if(event.what==kHighLevelEvent) { e=AEProcessAppleEvent(&event); if(e)record("AEProcess error=%d",e); }
        else if(event.what==updateEvt && window) { BeginUpdate(window); draw(); EndUpdate(window); updates++; }
        else if(event.what==mouseDown) {
            WindowPtr hit; short part=FindWindow(event.where,&hit);
            if(part==inDrag && hit==window) { Rect limit; SetRect(&limit,0,20,640,480); DragWindow(window,event.where,&limit); record("drag finished waiting=%d",waiting); }
            else if(part==inContent && hit==window) SelectWindow(window);
        } else if(event.what==keyDown || event.what==autoKey) {
            char key=(char)(event.message & charCodeMask);
            if((event.modifiers & cmdKey) && key=='q') { abandon("diagnostic quit"); quit=1; }
            else if(key=='s' || key=='S' || ((event.modifiers & cmdKey) && key=='.')) abandon("Stop");
            else if(key=='t' || key=='T') { if(waiting)deadline=now; }
            else if(key=='k' || key=='K') {
                abandon("ToolServer quit requested");
                e=send_event(kCoreEventClass,kAEQuitApplication,NULL,++next_id,kAENoReply);
                record("quit request error=%d waiting=%d (server may defer)",e,waiting);
                if(!waiting)message("ToolServer quit requested. R explicitly tests discovery/launch.");
            } else if(!waiting && (key=='r' || key=='R')) { phase=0; failures=0; if(send_command(0))failures++; }
            else if(!waiting && (key=='l' || key=='L')) { phase=3; if(send_command(2))failures++; }
            record("key=%d waiting=%d abandoned=%d",key,waiting,abandoned);
        }
        if(waiting) {
            ProcessInfoRec p; memset(&p,0,sizeof(p)); p.processInfoLength=sizeof(p);
            if(GetProcessInformation(&server,&p)) {
                abandon("ToolServer disappeared");
                record("retired id=%d server disappeared; outcome unknown; no resend",pending_id);
                waiting=0;
                message("ToolServer disappeared; unknown outcome. R explicitly starts a fresh test.");
            } else if(!abandoned && (int32_t)(now-deadline)>=0) abandon("deadline");
        }
        if(received) {
            received=0;
            if(abandoned) message("Late reply drained; outcome remains unknown. R or L starts a fresh test.");
            else if(phase<2) {
                if(malformed || (phase==0 ? tool_status!=0 : tool_status==0) || (phase==1 && !diagnostic[0]))failures++;
                if(phase==0 && !malformed && tool_status==0 && verify_resource())failures++;
                if(phase==0) { failures+=reply_checks(); phase=1; if(send_command(1)) { failures++; phase=3; } }
                else { phase=2; record("RESULT failures=%d success_and_error_suite=complete",failures);
                    message("Rez suite failures=%d. L runs long command for interaction tests.",failures); }
            } else message("Long command stat=%ld; R or L starts a fresh test.",(long)tool_status);
        }
    }
    record("diagnostic exit waiting=%d abandoned=%d",waiting,abandoned);
    FlushVol(NULL,folder.vRefNum); fclose(logfile);
    if(window)DisposeWindow(window);
    AERemoveEventHandler(kCoreEventClass,kAEAnswer,answer_upp,false);
    AERemoveEventHandler(kCoreEventClass,kAEOpenApplication,open_upp,false);
    DisposeAEEventHandlerUPP(answer_upp); DisposeAEEventHandlerUPP(open_upp);
    return failures ? 1 : 0;
}
