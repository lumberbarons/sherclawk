/* Exercise the real build tool through native File Manager snapshots and the
 * native MPW executor. Keeps fresh independent
 * and starter projects, repaired snapshots and revision-bound rebuild IDs. */
#include "build_project.h"
#include "selfbuild.h"
#include "toolserver.h"
#include <AppleEvents.h>
#ifdef RUN_CHECK
#include "run_application.h"
#endif
#include "config.h"
#include "json.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Processes.h>
#include <Resources.h>
#include <stdio.h>
#include <string.h>
static FILE *logfile;
static char result[AGENT_RESULT_CAP],folder[64];
static AgentCall call;
static int failures;
static int journal(void *ctx,const char *event,const char *json)
{
    (void)ctx;
    return fprintf(logfile,"{\"event\":\"%s\",\"message\":%s}\n",event,json)<0 || fflush(logfile) || FlushVol(NULL,0) ? -1 : 0;
}
static void invoke(const char *name,const char *arguments)
{
    strcpy(call.id,"buildcheck"); strcpy(call.name,name); strcpy(call.arguments,arguments);
    if(tools_execute_recorded(&call,result,sizeof(result),journal,NULL) || !strstr(result,"\"status\":\"ok\""))failures++;
    fprintf(logfile,"TOOL %s %s\n",name,result); fflush(logfile);
}
static void write_file(const char *name,const char *text)
{
    char path[128],qp[300],qt[6000],args[AGENT_ARGUMENT_CAP];
    snprintf(path,sizeof(path),"%s:%s",folder,name);
    json_quote(path,qp,sizeof(qp)); json_quote(text,qt,sizeof(qt));
    snprintf(args,sizeof(args),"{\"path\":%s,\"text\":%s}",qp,qt); invoke("write_text",args);
}
static void replace(const char *name,const char *old,const char *replacement)
{
    char path[128],qp[300],qo[1200],qn[1200],revision[80],args[4096]; JsonToken t[128];
    snprintf(path,sizeof(path),"%s:%s",folder,name); json_quote(path,qp,sizeof(qp));
    snprintf(args,sizeof(args),"{\"path\":%s}",qp); invoke("read_text",args);
    if(json_parse(result,strlen(result),t,128)<1 || json_string(result,t,json_member(result,t,0,"revision"),revision,sizeof(revision))<0){failures++;return;}
    json_quote(old,qo,sizeof(qo)); json_quote(replacement,qn,sizeof(qn));
    snprintf(args,sizeof(args),"{\"path\":%s,\"expected_revision\":\"%s\",\"old_text\":%s,\"new_text\":%s}",qp,revision,qo,qn); invoke("edit_text",args);
}
#ifdef SELF_BUILD_CHECK
static int process_check(void)
{
    ProcessSerialNumber psn={0,kNoProcess}; ProcessInfoRec info; FSSpec spec; Str255 name; OSErr e;
    while((e=GetNextProcess(&psn))==noErr) {
        memset(&info,0,sizeof(info)); info.processInfoLength=sizeof(info);
        info.processName=name; info.processAppSpec=&spec;
        if(GetProcessInformation(&psn,&info))return -1;
        fprintf(logfile,"PROCESS name=%.*s app=%.*s\n",name[0],name+1,spec.name[0],spec.name+1);
    }
    fflush(logfile);
    return e==procNotFound ? 0 : -1;
}
#endif
#ifdef SELF_BUILD_CHECK
static int artifact_check(const char *path)
{
    FSSpec spec; short previous=CurResFile(),file; Handle h; int ok=1;
    if(tools_resolve(path,&spec))return -1;
    file=FSpOpenResFile(&spec,fsRdPerm);if(file==-1)return -1;
    UseResFile(file);h=Get1Resource('cfrg',0);
    if(!h || ResError() || GetHandleSize(h)<=0)ok=0;
    else {fprintf(logfile,"RESOURCE cfrg/0 bytes=%ld\n",(long)GetHandleSize(h));ReleaseResource(h);}
    h=Get1Resource('SIZE',-1);
    if(!h || ResError() || GetHandleSize(h)!=10)ok=0;
    else {fprintf(logfile,"RESOURCE SIZE/-1 bytes=%ld\n",(long)GetHandleSize(h));ReleaseResource(h);}
    CloseResFile(file);UseResFile(previous);return ok ? 0 : -1;
}
#endif
static int build(WindowPtr window,int success)
{
    EventRecord event; int r,stop=0;
#ifdef SELF_BUILD_CHECK
    if(process_check()) {failures++;return 1;}
#endif
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",folder);
    r=build_project_begin(&call,result,sizeof(result),journal,NULL,(uint32_t)TickCount());
    while(r==2) {
        if(WaitNextEvent(everyEvent,&event,1,NULL)) {
            if(event.what==kHighLevelEvent)AEProcessAppleEvent(&event);
            if(event.what==keyDown && (event.modifiers & cmdKey) && (event.message & charCodeMask)=='.')stop=1;
            if(event.what==updateEvt && window) { BeginUpdate(window); SetPort(window); MoveTo(15,30); DrawString((const unsigned char *)"\035Native build; Cmd-period stops"); EndUpdate(window); }
        }
#ifdef SELF_BUILD_STOP_CHECK
        if(toolserver_busy())stop=1;
#endif
        r=build_project_step(result,sizeof(result),(uint32_t)TickCount(),stop);
    }
    fprintf(logfile,"BUILD %s\n",result); fflush(logfile);
#ifdef SELF_BUILD_CHECK
    { JsonToken tokens[128];char build_id[25],path[200],artifact[256];FSSpec marker;
      if(json_parse(result,strlen(result),tokens,128)<1 ||
          json_string(result,tokens,json_member(result,tokens,0,"build_id"),build_id,sizeof(build_id))<0)failures++;
      else {
          snprintf(path,sizeof(path),SHERCLAWK_BUILD_QUEUE ":%s:native-executor",build_id);
          if(tools_resolve(path,&marker))failures++;
          else fprintf(logfile,"EXECUTOR build_id=%s native=1\n",build_id);
          if(success && (json_string(result,tokens,json_member(result,tokens,0,"artifact"),artifact,sizeof(artifact))<0 || artifact_check(artifact)))failures++;
      }
    }
#endif
#ifdef SELF_BUILD_STOP_CHECK
    if(r!=1 || !strstr(result,"\"status\":\"uncertain\""))failures++;
    { uint32_t deadline=(uint32_t)TickCount();
      while(toolserver_busy() && (uint32_t)(TickCount()-deadline)<180UL*60UL) {
          WaitNextEvent(everyEvent,&event,1,NULL);
          if(event.what==kHighLevelEvent)AEProcessAppleEvent(&event);
          selfbuild_drain((uint32_t)TickCount());
      }
      selfbuild_drain((uint32_t)TickCount());
      JsonToken tokens[128];char build_id[25],path[200];FSSpec spec;
      if(toolserver_busy() || json_parse(result,strlen(result),tokens,128)<1 ||
          json_string(result,tokens,json_member(result,tokens,0,"build_id"),build_id,sizeof(build_id))<0)failures++;
      else {
          const char *names[]={"result","launch.rec","build:native:success.txt"};
          for(int k=0;k<3;k++) {
              snprintf(path,sizeof(path),SHERCLAWK_BUILD_QUEUE ":%s:%s",build_id,names[k]);
              if(!tools_resolve(path,&spec))failures++;
          }
          snprintf(path,sizeof(path),SHERCLAWK_BUILD_QUEUE ":%s:native-drained",build_id);
          if(tools_resolve(path,&spec))failures++;
          if(!tools_resolve(SHERCLAWK_BUILD_QUEUE ":worker-lock",&spec))failures++;
          fprintf(logfile,"STOP build_id=%s uncertain=1 drained=%d no_result_no_authority_no_success=1\n",build_id,!toolserver_busy());
      }
    }
#else
    if(r || (success ? !strstr(result,"\"status\":\"ok\"") : !strstr(result,"\"exit\":1")))failures++;
#endif
#ifdef RUN_CHECK
    if(!r) {
        JsonToken t[128]; char id[25]; int launched;
        if(json_parse(result,strlen(result),t,128)<1 || json_string(result,t,json_member(result,t,0,"build_id"),id,sizeof(id))<0) {failures++;return 1;}
        snprintf(call.arguments,sizeof(call.arguments),"{\"build_id\":\"%s\"}",id);
        launched=run_application_begin(&call,result,sizeof(result),journal,NULL,(uint32_t)TickCount());
        while(launched==2) {
            WaitNextEvent(everyEvent,&event,1,NULL);
            launched=run_application_step(result,sizeof(result),(uint32_t)TickCount(),0);
        }
        fprintf(logfile,"RUN %s\n",result);fflush(logfile);
        if(success ? launched || !strstr(result,"LAUNCHED") : launched || !strstr(result,"BUILD_NOT_AUTHORIZED"))failures++;
    }
#endif
#ifdef SELF_BUILD_STOP_CHECK
    return 0;
#else
    return r;
#endif
}
int main(void)
{
    char args[512],revision[80]; JsonToken t[64]; WindowPtr window; Rect bounds;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    #ifdef SELF_BUILD_STOP_CHECK
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkSelfBuildStopCheck.log","w");
#elif defined(SELF_BUILD_CHECK)
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkSelfBuildCheck.log","w");
#elif defined(RUN_CHECK)
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkRunCheck.log","w");
#else
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkBuildCheck.log","w");
#endif
    if(!logfile)return 1;
    if(selfbuild_init()) {fclose(logfile);return 1;}
    snprintf(folder,sizeof(folder),"BuildCheck%08lx",(unsigned long)TickCount());
    snprintf(args,sizeof(args),"{\"path\":\"%s\"}",folder); invoke("create_folder",args);
    write_file("project.json","{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\",\"extra.c\"],\"resources\":[\"app.r\"],\"headers\":[\"shared.h\"],\"include_paths\":[\".\"],\"output\":\"independent\"}\n");
    write_file("main.c","#include <Quickdraw.h>\n#include <Fonts.h>\n#include <Windows.h>\n#include <Menus.h>\n#include <TextEdit.h>\n#include <Dialogs.h>\n#include <Events.h>\n#include \"shared.h\"\nQDGlobals qd;\nint main(void) { EventRecord event; WindowPtr w; Rect r; InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0); InitCursor(); SetRect(&r,60,70,460,180); w=NewWindow(0,&r,\"\\pIndependent build\",1,documentProc,(WindowPtr)-1,1,0); SetPort(w); while(1) { if(WaitNextEvent(everyEvent,&event,10,0)) { if(event.what==keyDown && (event.modifiers & cmdKey) && (event.message & charCodeMask)=='q')break; if(event.what==updateEvt) { BeginUpdate(w); MoveTo(15,30); DrawString(message()); EndUpdate(w); } } } DisposeWindow(w); return 0; }\n");
    write_file("shared.h","const unsigned char *message(void);\n");
    write_file("extra.c","#error deliberate build_project compiler error\n#include \"shared.h\"\nconst unsigned char *message(void) { return \"\\pTwo source files compiled natively\"; }\n");
    write_file("app.r","#include \"Types.r\"\nresource 'SIZE' (-1) { reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch, backgroundAndForeground, dontGetFrontClicks, ignoreAppDiedEvents, is32BitCompatible, isHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices, reserved, reserved, reserved, 524288, 524288 };\n");
    SetRect(&bounds,70,210,570,310); window=NewWindow(NULL,&bounds,(const unsigned char *)"\025Sherclawk build check",true,documentProc,(WindowPtr)-1,false,0);
    if(build(window,0))goto done;
#ifdef SELF_BUILD_STOP_CHECK
    goto done;
#endif
    snprintf(args,sizeof(args),"{\"path\":\"%s:extra.c\"}",folder); invoke("read_text",args);
    if(json_parse(result,strlen(result),t,64)<1 || json_string(result,t,json_member(result,t,0,"revision"),revision,sizeof(revision))<0){failures++;goto done;}
    snprintf(args,sizeof(args),"{\"path\":\"%s:extra.c\",\"expected_revision\":\"%s\",\"old_text\":\"#error deliberate build_project compiler error\\n\",\"new_text\":\"\"}",folder,revision); invoke("edit_text",args);
    if(build(window,1))goto done;
#ifdef SELF_BUILD_CHECK
    replace("extra.c","Two source files compiled natively","Fresh revision compiled natively");
    if(build(window,1))goto done;
#endif
    strcat(folder,"s");
    snprintf(args,sizeof(args),"{\"path\":\"%s\"}",folder); invoke("create_project",args);
    replace("project.json","\"sources\":[\"main.c\"]","\"sources\":[\"main.c\",\"extra.c\"]");
    replace("project.json","\"output\":\"template\"","\"output\":\"starter\"");
    write_file("extra.c","#error deliberate starter compiler error\nint added_source(void) { return 7; }\n");
    if(build(window,0))goto done;
    replace("extra.c","#error deliberate starter compiler error\n","");
    if(build(window,1))goto done;
#ifdef SELF_BUILD_CHECK
    replace("extra.c","return 7;","return 8;");
    build(window,1);
#endif
done:
    fprintf(logfile,"RESULT failures=%d project=%s\n",failures,folder); fflush(logfile); FlushVol(NULL,0); fclose(logfile);
    selfbuild_close();
    if(window)DisposeWindow(window);
    return failures ? 1 : 0;
}
