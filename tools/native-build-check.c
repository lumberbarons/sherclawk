/* Increment 2: fixed structured native build, independent of queue/chat state.
 * Sources come from the real starter, never a supplied shell script. Each
 * attempt keeps a fresh folder; unknown outcomes never advance or replay.
 * This acceptance fixture requires an idle ToolServer and logs every running
 * process as evidence of the executor environment.
 */
#include "config.h"
#include "toolserver.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Processes.h>
#include <Files.h>
#include <Resources.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "build/project-template.h"
#ifdef NATIVE_BUILD_ERROR
#define LOG_NAME "SherclawkNativeBuildErrorCheck.log"
#else
#define LOG_NAME "SherclawkNativeBuildCheck.log"
#endif

static FILE *logfile;
static WindowPtr window;
static char fixture[160], status_line[240];
static int stage, failures, finished, quit, stop;
static uint32_t last_tick, max_gap, loops;
#define COMPILE(file, object) {"compile", "MrC " file " -o " object " -i \"{CIncludes}\" -i \":\" -w off"}
static const struct { const char *stage, *command; } steps[] = {
    COMPILE("main.c", "main.o"), COMPILE("scene.c", "scene.o"), COMPILE("io.c", "io.o"),
    COMPILE("png.c", "png.o"), COMPILE("selfrender.c", "selfrender.o"),
    {"link", "PPCLink -o Template main.o scene.o io.o png.o selfrender.o \"{SharedLibraries}\"InterfaceLib \"{SharedLibraries}\"StdCLib \"{PPCLibraries}\"StdCRuntime.o \"{PPCLibraries}\"PPCCRuntime.o -t APPL"},
    {"resources", "Rez app.r -o Template -append -i \"{RIncludes}\""}
};
#define STEPS ((int)(sizeof(steps)/sizeof(steps[0])))
static void record(const char *fmt,...)
{
    va_list args; va_start(args,fmt); vfprintf(logfile,fmt,args); va_end(args);
    fputc('\n',logfile); fflush(logfile);
}
static void message(const char *text)
{
    snprintf(status_line,sizeof(status_line),"%s",text);
    if(window) { Rect r=window->portRect; SetPort(window); InvalRect(&r); }
}
static void draw(void)
{
    const char *lines[]={status_line,"S / Cmd-period: stop (in-flight outcome unknown)",
        "Cmd-Q: leave diagnostic; retained fixture is never reused."};
    Str255 text; int i;
    SetPort(window); EraseRect(&window->portRect);
    for(i=0;i<3;i++) { size_t n=strlen(lines[i]); text[0]=(unsigned char)n;
        memcpy(text+1,lines[i],n); MoveTo(12,25+23*i); DrawString(text); }
}
static OSErr resolve(const char *leaf,FSSpec *spec)
{
    char path[256]; Str255 p;
    if(snprintf(path,sizeof(path),"%s%s",fixture,leaf)>255)return paramErr;
    p[0]=(unsigned char)strlen(path); memcpy(p+1,path,p[0]);
    return FSMakeFSSpec(0,0,p,spec);
}
static OSErr catalog(FSSpec *s,CInfoPBRec *p)
{
    memset(p,0,sizeof(*p)); p->hFileInfo.ioNamePtr=s->name;
    p->hFileInfo.ioVRefNum=s->vRefNum; p->hFileInfo.ioDirID=s->parID;
    return PBGetCatInfoSync(p);
}
/* Create-only, close/flush/read back; preserve even partially written files. */
static int write_file(const char *name,const char *bytes)
{
    FSSpec spec; CInfoPBRec info; short ref; OSErr e,c;
    long n=(long)strlen(bytes),got=n; char verify[4096];
    if(n>(long)sizeof(verify) || resolve(name,&spec)!=fnfErr ||
        FSpCreate(&spec,'ttxt','TEXT',smSystemScript) || FSpOpenDF(&spec,fsWrPerm,&ref))return -1;
    e=FSWrite(ref,&got,bytes); c=FSClose(ref);
    if(e || c || got!=n || FlushVol(NULL,spec.vRefNum) || FSpOpenDF(&spec,fsRdPerm,&ref))return -1;
    got=n; e=FSRead(ref,&got,verify); c=FSClose(ref);
    if(e || c || got!=n || memcmp(verify,bytes,(size_t)n) || catalog(&spec,&info) ||
        info.hFileInfo.ioFlFndrInfo.fdType!='TEXT' ||
        info.hFileInfo.ioFlFndrInfo.fdCreator!='ttxt' || info.hFileInfo.ioFlRLgLen)return -1;
    record("input=%s bytes=%ld exact=1 TEXT/ttxt resource=0",name,n); return 0;
}
/* Process listing is evidence for this fixture installation, not a queue
 * ownership lock. Returns 1 when the whole list was read. */
static int scan_processes(void)
{
    ProcessSerialNumber psn={0,kNoProcess}; ProcessInfoRec info;
    FSSpec spec; Str255 name; OSErr e;
    while((e=GetNextProcess(&psn))==noErr) {
        memset(&info,0,sizeof(info)); info.processInfoLength=sizeof(info);
        info.processName=name; info.processAppSpec=&spec;
        if(GetProcessInformation(&psn,&info))return 0;
        record("process=%lu:%lu signature=%08lx name=%.*s app=%.*s",
            (unsigned long)psn.highLongOfPSN,(unsigned long)psn.lowLongOfPSN,
            (unsigned long)info.processSignature,name[0],name+1,spec.name[0],spec.name+1);
    }
    return e==procNotFound;
}
static int fail(const char *reason,int unknown)
{
    finished=1; failures++;
    record("RESULT failures=%d status=%s stage=%d reason=%s loops=%lu max_gap=%lu fixture=%s",
        failures,unknown ? "unknown" : "error",stage,reason,(unsigned long)loops,
        (unsigned long)max_gap,fixture);
    message(unknown ? "Unknown outcome; retained fixture; no subsequent commands." :
        "Diagnostic failed; inspect retained evidence. No automatic retry.");
    return -1;
}
static int send_step(uint32_t now)
{
    OSErr e;
    if(stop)return fail("Stop between commands",0);
    if(!scan_processes())return fail("process scan failed",0);
    record("stage=%s started",steps[stage].stage);
    message(steps[stage].stage);
    e=toolserver_send(fixture,steps[stage].command,now);
    if(e)return fail("ToolServer send/discovery error (no retry)",1);
    return 0;
}
#ifndef NATIVE_BUILD_ERROR
static int read_resource(short file,ResType type,short id,Handle *h)
{
    UseResFile(file); *h=Get1Resource(type,id);
    return !*h || ResError() || GetHandleSize(*h)<=0 ? -1 : 0;
}
static int verify_and_launch(void)
{
    FSSpec spec,actual; CInfoPBRec info; FInfo finder;
    short ref,resources,previous; OSErr e,c; long got;
    unsigned char header[12]; Handle h; int valid;
    LaunchParamBlockRec launch; ProcessInfoRec process; Str255 name;
    record("stage=metadata started");
    if(resolve("Template",&spec) || FSpGetFInfo(&spec,&finder))return fail("artifact missing",0);
    finder.fdType='APPL'; finder.fdCreator='SHTP';
    if(FSpSetFInfo(&spec,&finder) || FlushVol(NULL,spec.vRefNum) || catalog(&spec,&info) ||
        (info.hFileInfo.ioFlAttrib & 16) || (info.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) ||
        info.hFileInfo.ioFlFndrInfo.fdType!='APPL' || info.hFileInfo.ioFlFndrInfo.fdCreator!='SHTP' ||
        info.hFileInfo.ioFlLgLen<40 || info.hFileInfo.ioFlRLgLen<=0)return fail("forks or Finder metadata",0);
    if(FSpOpenDF(&spec,fsRdPerm,&ref))return fail("data fork open",0);
    got=sizeof(header); e=FSRead(ref,&got,header); c=FSClose(ref);
    if(e || c || got!=sizeof(header) || memcmp(header,"Joy!peffpwpc",12))return fail("PowerPC PEF header",0);
    previous=CurResFile(); resources=FSpOpenResFile(&spec,fsRdPerm);
    if(resources==-1)return fail("resource fork open",0);
    valid=!read_resource(resources,'cfrg',0,&h);
    if(valid) { record("resource=cfrg/0 bytes=%ld",(long)GetHandleSize(h)); ReleaseResource(h); }
    if(valid) {
        valid=!read_resource(resources,'SIZE',-1,&h);
        if(valid) {
            HLock(h);
            /* SIZE flags + big-endian preferred/minimum allocations. */
            valid=GetHandleSize(h)==10 && !memcmp(*h+2,"\000\040\000\000\000\030\000\000",8);
            HUnlock(h); record("resource=SIZE/-1 bytes=%ld allocation_exact=%d",(long)GetHandleSize(h),valid);
            ReleaseResource(h);
        }
    }
    CloseResFile(resources); UseResFile(previous);
    if(!valid)return fail("missing cfrg or incorrect SIZE",0);
    record("artifact=Template data=%ld resource=%ld Finder=APPL/SHTP PEF=PowerPC",
        info.hFileInfo.ioFlLgLen,info.hFileInfo.ioFlRLgLen);
    if(stop)return fail("Stop before success/launch",0);
    if(write_file("success.txt","artifact=Template\n"))return fail("success record persistence",1);
    record("stage=complete status=0");
    if(!scan_processes())return fail("process scan failed before launch",0);
    record("launch intent artifact=%sTemplate",fixture);
    memset(&launch,0,sizeof(launch)); launch.launchBlockID=extendedBlock;
    launch.launchEPBLength=extendedBlockLen;
    launch.launchControlFlags=launchContinue | launchNoFileFlags;
    launch.launchAppSpec=&spec; e=LaunchApplication(&launch);
    if(e)return fail("LaunchApplication error (no retry)",1);
    memset(&process,0,sizeof(process)); process.processInfoLength=sizeof(process);
    process.processName=name; process.processAppSpec=&actual;
    e=GetProcessInformation(&launch.launchProcessSN,&process);
    if(e || actual.vRefNum!=spec.vRefNum || actual.parID!=spec.parID ||
        memcmp(actual.name,spec.name,(size_t)spec.name[0]+1))return fail("launch observation uncertain",1);
    record("launch process=%lu:%lu exact_artifact=1 name=%.*s",
        (unsigned long)launch.launchProcessSN.highLongOfPSN,
        (unsigned long)launch.launchProcessSN.lowLongOfPSN,name[0],name+1);
    record("RESULT failures=0 fixed_native_build=complete process_present=1 visual_smoke=pending loops=%lu max_gap=%lu fixture=%s",
        (unsigned long)loops,(unsigned long)max_gap,fixture);
    finished=1; message("Native compile/link/Rez/verify/launch passed. Fixture retained."); return 0;
}
#endif
int main(void)
{
    EventRecord event; Rect bounds; Str255 path; FSSpec folder;
    char full[160]; long dir; OSErr e; uint32_t now; int i; static ToolServerReply reply;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE LOG_NAME,"w"); if(!logfile)return 1;
    snprintf(fixture,sizeof(fixture),SHERCLAWK_WORKSPACE "NativeBuildCheck%08lx:",(unsigned long)TickCount());
    record("fixture=%s executor=native fixed_template=ppc-toolbox-v1",fixture);
    strcpy(full,fixture); full[strlen(full)-1]=0;
    path[0]=(unsigned char)strlen(full); memcpy(path+1,full,path[0]);
    e=FSMakeFSSpec(0,0,path,&folder);
    if(e!=fnfErr || FSpDirCreate(&folder,smSystemScript,&dir)) {
        fail("fixture setup",0); fclose(logfile); return 1;
    }
    for(i=0;i<(int)(sizeof(project_inputs)/sizeof(project_inputs[0]));i++) {
        const char *bytes=project_inputs[i].bytes;
        if(!strcmp(project_inputs[i].name,"project.json"))continue;
#ifdef NATIVE_BUILD_ERROR
        if(!strcmp(project_inputs[i].name,"main.c"))bytes="#error Sherclawk deliberate native compiler failure\r";
#endif
        if(write_file(project_inputs[i].name,bytes)) break;
    }
    if(i<(int)(sizeof(project_inputs)/sizeof(project_inputs[0])) || FlushVol(NULL,folder.vRefNum)) {
        fail("fixture setup",0); fclose(logfile); return 1;
    }
    if(toolserver_init(record)) { fail("Apple-event handler installation",0); fclose(logfile); return 1; }
    SetRect(&bounds,40,260,620,385);
    window=NewWindow(NULL,&bounds,(const unsigned char *)"\031Sherclawk NativeBuildCheck",true,documentProc,(WindowPtr)-1,false,0);
    last_tick=(uint32_t)TickCount(); send_step(last_tick);
    while(!quit) {
        WaitNextEvent(everyEvent,&event,1,NULL); now=(uint32_t)TickCount();
        if(!finished) { uint32_t gap=now-last_tick; if(gap>max_gap)max_gap=gap; loops++; }
        last_tick=now;
        if(event.what==kHighLevelEvent) { e=AEProcessAppleEvent(&event); if(e)record("AEProcess error=%d",e); }
        else if(event.what==updateEvt && window && (WindowPtr)event.message==window) { BeginUpdate(window); draw(); EndUpdate(window); }
        else if(event.what==mouseDown) {
            WindowPtr hit; short part=FindWindow(event.where,&hit);
            if(part==inDrag && hit==window) { Rect limit; SetRect(&limit,0,20,640,480); DragWindow(window,event.where,&limit); }
            else if(part==inContent && hit==window)SelectWindow(window);
        } else if(event.what==keyDown || event.what==autoKey) {
            char key=(char)(event.message & charCodeMask);
            if((event.modifiers & cmdKey) && key=='q') { stop=1; quit=1; }
            else if(key=='s' || key=='S' || ((event.modifiers & cmdKey) && key=='.'))stop=1;
        }
        int outcome=toolserver_poll(now,stop,&reply);
        if(outcome<0 && !finished)fail("Stop/deadline/ToolServer disappearance",1);
        if(outcome==1) {
            record("stage=%s raw_status=%ld malformed=%d abandoned=%d stdout=[%s] diag=[%s]",
                steps[stage].stage,(long)reply.status,reply.malformed,reply.abandoned,reply.output,reply.diagnostic);
            if(!finished) {
                if(reply.malformed || reply.abandoned)fail("unusable or abandoned reply",1);
                else if(reply.status) {
#ifdef NATIVE_BUILD_ERROR
                    FSSpec s;
                    if(stage==0 && strstr(reply.diagnostic,"Sherclawk deliberate native compiler failure") &&
                        resolve("Template",&s)==fnfErr && resolve("success.txt",&s)==fnfErr) {
                        finished=1;
                        record("RESULT failures=0 expected_compiler_failure=1 no_artifact=1 no_success=1 no_launch=1 fixture=%s",fixture);
                        message("Expected compiler failure; no artifact, success record or launch.");
                    } else fail("unexpected failure or output after compiler error",0);
#else
                    fail("tool command failed",0);
#endif
                }
#ifdef NATIVE_BUILD_ERROR
                else fail("deliberate compiler error unexpectedly succeeded",0);
#else
                else if(stop)fail("Stop before next command",0);
                else if(++stage<STEPS)send_step(now);
                else verify_and_launch();
#endif
            }
        }
    }
    record("diagnostic exit busy=%d stop=%d",toolserver_busy(),stop);
    toolserver_close(); if(window)DisposeWindow(window);
    FlushVol(NULL,folder.vRefNum); fclose(logfile); return failures ? 1 : 0;
}
