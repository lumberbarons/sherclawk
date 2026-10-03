/* Native create-only acceptance: exercise actual AFP/File Manager behavior,
 * MacRoman/CR/TEXT, readback, collisions, limits and flushed recovery records.
 * Unique fixture folders are preserved so diagnostics never overwrite sources. */
#include "tools.h"
#include "json.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
static FILE *logfile;
static int records, failures;
static AgentCall call;
static char result[AGENT_RESULT_CAP], folder[80], path[160];
static int journal(void *ctx, const char *event, const char *json)
{
    (void)ctx;
    if (fprintf(logfile,"{\"event\":\"%s\",\"message\":%s}\n",event,json)<0 || fflush(logfile)) return -1;
    if (FlushVol(NULL,0)) return -1;
    records++; return 0;
}
static void check(int condition, const char *name)
{
    fprintf(logfile,"%s %s\n",condition ? "PASS" : "FAIL",name);
    if (!condition) failures++;
    fflush(logfile);
}
static void write_call(const char *name, const char *text)
{
    char quoted[AGENT_ARGUMENT_CAP-400];
    snprintf(path,sizeof(path),"%s:%s",folder,name);
    strcpy(call.name,"write_text");
    if (json_quote(text,quoted,sizeof(quoted))<0) { failures++; return; }
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"text\":%s}",path,quoted);
    int stop=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    fprintf(logfile,"WRITE stop=%d %s\n",stop,result);fflush(logfile);
}
int main(void)
{
    FSSpec spec;
    CInfoPBRec pb;
    Str255 native;
    char full[256], raw[4097], revision[80];
    JsonToken tokens[128];
    short ref;
    long dir, size;
    OSErr err;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkWriteCheck.log","w");if(!logfile)return 1;
    snprintf(folder,sizeof(folder),"Sherclawk Write %08lx",(unsigned long)TickCount());
    snprintf(full,sizeof(full),"%s%s",SHERCLAWK_WORKSPACE,folder);
    native[0]=(unsigned char)strlen(full);memcpy(native+1,full,native[0]);
    err=FSMakeFSSpec(0,0,native,&spec);
    if(err!=fnfErr || FSpDirCreate(&spec,smSystemScript,&dir)) { fclose(logfile);return 1; }
    memset(&call,0,sizeof(call));strcpy(call.id,"native-write");
    write_call("hello.c","/* caf\xc3\xa9 */\r\nint main(void) { return 0; }\n");
    check(strstr(result,"CREATED")!=NULL && records==3,"created with intent/staged/committed records");
    json_parse(result,strlen(result),tokens,128);
    json_string(result,tokens,json_member(result,tokens,0,"revision"),revision,sizeof(revision));
    snprintf(full,sizeof(full),"%s%s",SHERCLAWK_WORKSPACE,path);
    native[0]=(unsigned char)strlen(full);memcpy(native+1,full,native[0]);
    err=FSMakeFSSpec(0,0,native,&spec);
    memset(&pb,0,sizeof(pb));pb.hFileInfo.ioNamePtr=spec.name;pb.hFileInfo.ioVRefNum=spec.vRefNum;pb.hFileInfo.ioDirID=spec.parID;
    check(!err && !PBGetCatInfoSync(&pb) && pb.hFileInfo.ioFlFndrInfo.fdType=='TEXT' && !pb.hFileInfo.ioFlRLgLen,"native Finder TEXT, no resource fork");
    size=sizeof(raw)-1;err=FSpOpenDF(&spec,fsRdPerm,&ref);
    if(!err) { err=FSRead(ref,&size,raw);FSClose(ref);raw[size]=0; }
    check((!err || err==eofErr) && !strcmp(raw,"/* caf\x8e */\rint main(void) { return 0; }\r"),"exact MacRoman and CR bytes");
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",path);
    tools_execute(&call,result,sizeof(result));fprintf(logfile,"READ %s\n",result);
    check(strstr(result,"caf\xc3\xa9")!=NULL && strstr(result,revision)!=NULL,"readback and matching revision");
    write_call("hello.c","overwrite");check(strstr(result,"EXISTS")!=NULL && records==3,"collision refused without another mutation");
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",path);tools_execute(&call,result,sizeof(result));
    check(strstr(result,"int main")!=NULL && !strstr(result,"overwrite"),"original survives collision");
    write_call("emoji.txt","\xf0\x9f\xa6\x80");check(strstr(result,"ENCODING_LIMIT")!=NULL && records==3,"unsupported Unicode refused before mutation");
    write_call("control.txt","\001");check(strstr(result,"NOT_TEXT")!=NULL && records==3,"binary control refused");
    write_call("empty.txt","");check(strstr(result,"CREATED")!=NULL,"empty file created");
    memset(raw,'x',4096);raw[4096]=0;write_call("large.c",raw);check(strstr(result,"CREATED")!=NULL,"4096 byte boundary created");
    strcpy(call.name,"write_text");strcpy(call.arguments,"{\"path\":\":escape.c\",\"text\":\"x\"}");tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    check(strstr(result,"PATH")!=NULL,"path escape refused");
    fprintf(logfile,"RESULT failures=%d fixture=%s\n",failures,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
