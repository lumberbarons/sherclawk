/* Native project acceptance preserves a unique project on the AFP volume.
 * Verify exact template bytes, Finder metadata and collision-safe publication;
 * this diagnostic does not compile or authorize launch of project artifacts. */
#include "tools.h"
#include "json.h"
#include "config.h"
#include "build/project-template.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <stdio.h>
#include <string.h>
static FILE *logfile;
static int records, failures;
static int journal(void *ctx, const char *event, const char *json)
{
    (void)ctx;
    if (fprintf(logfile,"{\"event\":\"%s\",\"message\":%s}\n",event,json)<0 || fflush(logfile) || FlushVol(NULL,0)) return -1;
    records++;return 0;
}
static void check(int ok, const char *name)
{
    fprintf(logfile,"%s %s\n",ok ? "PASS" : "FAIL",name);fflush(logfile);
    if(!ok)failures++;
}
int main(void)
{
    AgentCall call;
    char result[AGENT_RESULT_CAP], folder[80], full[256], raw[4096];
    FSSpec spec;
    CInfoPBRec pb;
    Str255 native;
    short ref;
    long count;
    OSErr err, closed;
    int i, stop;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkProjectCheck.log","w");if(!logfile)return 1;
    snprintf(folder,sizeof(folder),"Sherclawk Project %08lx",(unsigned long)TickCount());
    memset(&call,0,sizeof(call));strcpy(call.id,"native-project");strcpy(call.name,"create_project");
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",folder);
    stop=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    fprintf(logfile,"CREATE stop=%d %s\n",stop,result);
    check(!stop && strstr(result,"CREATED_PROJECT") && records==3,"complete journaled publication");
    for(i=0;i<3;i++) {
        snprintf(full,sizeof(full),"%s%s:%s",SHERCLAWK_WORKSPACE,folder,project_inputs[i].name);
        native[0]=(unsigned char)strlen(full);memcpy(native+1,full,native[0]);
        err=FSMakeFSSpec(0,0,native,&spec);
        memset(&pb,0,sizeof(pb));pb.hFileInfo.ioNamePtr=spec.name;pb.hFileInfo.ioVRefNum=spec.vRefNum;pb.hFileInfo.ioDirID=spec.parID;
        if(!err)err=PBGetCatInfoSync(&pb);
        check(!err && pb.hFileInfo.ioFlFndrInfo.fdType=='TEXT' && pb.hFileInfo.ioFlFndrInfo.fdCreator=='ttxt' && !pb.hFileInfo.ioFlRLgLen,"Finder TEXT/ttxt, no resource fork");
        count=(long)strlen(project_inputs[i].bytes);
        if(!err)err=FSpOpenDF(&spec,fsRdPerm,&ref);
        if(!err){err=FSRead(ref,&count,raw);closed=FSClose(ref);if(!err)err=closed;}
        check(!err && count==(long)strlen(project_inputs[i].bytes) && !memcmp(raw,project_inputs[i].bytes,(size_t)count),project_inputs[i].name);
    }
    stop=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    check(!stop && strstr(result,"EXISTS") && records==3,"existing project refused without mutation");
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s:main.c\"}",folder);
    tools_execute(&call,result,sizeof(result));fprintf(logfile,"READ %s\n",result);
    check(strstr(result,"whole_file") && strstr(result,"\"editable\":true"),"source readable with editable whole-file revision");
    strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\":escape\"}");
    stop=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    check(!stop && strstr(result,"PATH") && records==3,"path escape refused before mutation");
    fprintf(logfile,"RESULT failures=%d fixture=%s\n",failures,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
