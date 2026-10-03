/* Native exact-edit acceptance. AFP rename/open behavior cannot be inferred
 * from host mocks: preserve a unique fixture and every journal/backup, then
 * exercise guards, MacRoman/CR, paging, locks and replacement in OS 9. */
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
static int records, failures, stopped;
static AgentCall call;
static char result[AGENT_RESULT_CAP], folder[80], path[160];
static int journal(void *ctx, const char *event, const char *json)
{
    (void)ctx;
    if (fprintf(logfile,"{\"event\":\"%s\",\"message\":%s}\n",event,json)<0 || fflush(logfile) || FlushVol(NULL,0)) return -1;
    records++; return 0;
}
static void check(int condition, const char *name)
{
    fprintf(logfile,"%s %s\n",condition ? "PASS" : "FAIL",name);
    if (!condition) failures++;
    fflush(logfile);
}
static void execute(void)
{
    if (stopped) { failures++; return; }
    stopped=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    fprintf(logfile,"%s stop=%d %s\n",call.name,stopped,result);fflush(logfile);
}
static int field(const char *key, char *out, size_t cap)
{
    JsonToken tokens[128];
    return json_parse(result,strlen(result),tokens,128)>0 &&
        json_string(result,tokens,json_member(result,tokens,0,key),out,cap)>=0;
}
static OSErr spec_for(const char *relative, FSSpec *spec)
{
    char full[256];Str255 native;
    snprintf(full,sizeof(full),"%s%s",SHERCLAWK_WORKSPACE,relative);
    native[0]=(unsigned char)strlen(full);memcpy(native+1,full,native[0]);
    return FSMakeFSSpec(0,0,native,spec);
}
static void create_fixture(const char *name, const char *text)
{
    static char quoted[AGENT_ARGUMENT_CAP-400];
    snprintf(path,sizeof(path),"%s:%s",folder,name);strcpy(call.name,"write_text");
    if(json_quote(text,quoted,sizeof(quoted))<0) { failures++;return; }
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"text\":%s}",path,quoted);execute();
    check(strstr(result,"CREATED")!=NULL,"fixture created");
}
static void read_page(int offset)
{
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"start_byte\":%d}",path,offset);execute();
}
static void edit(const char *revision, const char *old, const char *text)
{
    static char qo[2048], qn[4096];
    if(json_quote(old,qo,sizeof(qo))<0 || json_quote(text,qn,sizeof(qn))<0) { failures++;return; }
    strcpy(call.name,"edit_text");
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"expected_revision\":\"%s\",\"old_text\":%s,\"new_text\":%s}",path,revision,qo,qn);execute();
}
int main(void)
{
    FSSpec spec;
    CInfoPBRec pb;
    char revision[80] = "", updated[80] = "", backup[768], raw[4098];
    short ref;
    long dir, size;
    OSErr err;
    int before;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkEditCheck.log","w");if(!logfile)return 1;
    snprintf(folder,sizeof(folder),"Sherclawk Edit %08lx",(unsigned long)TickCount());
    err=spec_for(folder,&spec);
    if(err!=fnfErr || FSpDirCreate(&spec,smSystemScript,&dir)) { fclose(logfile);return 1; }
    memset(&call,0,sizeof(call));strcpy(call.id,"native-edit");
    create_fixture("hello.c","/* caf\xc3\xa9 */\r\nint main(void) { return 0; }\n");
    read_page(0);check(field("revision",revision,sizeof(revision)) && strstr(result,"\"editable\":true"),"whole-file editable revision");
    read_page(8);check(strstr(result,revision)!=NULL,"revision independent of displayed page");
    before=records;
    edit(revision,"return 0;","return 1;\r\n/* caf\xc3\xa9 */\n");
    check(!stopped && strstr(result,"EDITED")!=NULL && records==before+4,"native edit with four durable boundaries");
    check(field("revision",updated,sizeof(updated)) && strcmp(updated,revision),"new revision returned");
    if(field("backup_path",backup,sizeof(backup))) {
        err=spec_for(backup,&spec);size=sizeof(raw)-1;
        if(!err)err=FSpOpenDF(&spec,fsRdPerm,&ref);
        if(!err) { err=FSRead(ref,&size,raw);FSClose(ref);raw[size]=0; }
        check((!err || err==eofErr) && !strcmp(raw,"/* caf\x8e */\rint main(void) { return 0; }\r"),"original backup exact bytes and unlocked");
    } else check(0,"backup path returned");
    err=spec_for(path,&spec);memset(&pb,0,sizeof(pb));
    pb.hFileInfo.ioNamePtr=spec.name;pb.hFileInfo.ioVRefNum=spec.vRefNum;pb.hFileInfo.ioDirID=spec.parID;
    check(!err && !PBGetCatInfoSync(&pb) && pb.hFileInfo.ioFlFndrInfo.fdType=='TEXT' && !pb.hFileInfo.ioFlRLgLen,"published Finder TEXT without resource fork");
    size=sizeof(raw)-1;if(!err)err=FSpOpenDF(&spec,fsRdPerm,&ref);
    if(!err) { err=FSRead(ref,&size,raw);FSClose(ref);raw[size]=0; }
    check((!err || err==eofErr) && !strcmp(raw,"/* caf\x8e */\rint main(void) { return 1;\r/* caf\x8e */\r }\r"),"replacement MacRoman/CR bytes");
    read_page(0);check(strstr(result,updated)!=NULL && strstr(result,"return 1;"),"edited readback and revision");
    before=records;edit(revision,"return 1;","return 2;");check(strstr(result,"REVISION_MISMATCH")!=NULL && records==before,"stale revision refused");
    edit(updated,"missing","x");check(strstr(result,"NO_MATCH")!=NULL && records==before,"missing match refused");
    edit(updated,"return 1;","\xf0\x9f\xa6\x80");check(strstr(result,"ENCODING_LIMIT")!=NULL && records==before,"unsupported Unicode refused");
    edit(updated,"return 1;","\177");check(strstr(result,"NOT_TEXT")!=NULL && records==before,"binary control refused");
    err=spec_for(path,&spec);if(!err)err=FSpOpenDF(&spec,fsRdWrPerm,&ref);
    check(!err,"exclusive open for busy-file check");
    if(!err) { edit(updated,"return 1;","return 2;");check(strstr(result,"BUSY")!=NULL && records==before,"busy source refused");FSClose(ref); }
    create_fixture("ambiguous.c","aaa\n");read_page(0);field("revision",revision,sizeof(revision));before=records;
    edit(revision,"aa","b");check(strstr(result,"AMBIGUOUS_MATCH")!=NULL && records==before,"overlapping matches refused");
    create_fixture("delete.c","remove\n");read_page(0);field("revision",revision,sizeof(revision));
    edit(revision,"remove\n","");check(strstr(result,"EDITED")!=NULL,"deletion to empty file");read_page(0);check(strstr(result,"\"text\":\"\"")!=NULL,"empty readback");
    memset(raw,'x',4096);raw[4095]='z';raw[4096]=0;create_fixture("large.c",raw);
    read_page(0);field("revision",revision,sizeof(revision));read_page(3500);check(strstr(result,revision)!=NULL,"4096-byte page guard");
    edit(revision,"z","y");check(strstr(result,"EDITED")!=NULL,"4096-byte edit boundary");
    read_page(0);field("revision",revision,sizeof(revision));before=records;
    edit(revision,"y","yy");check(strstr(result,"LIMIT")!=NULL && records==before,"oversized replacement refused");
    fprintf(logfile,"RESULT failures=%d stopped=%d fixture=%s\n",failures,stopped,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
