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
#include <Events.h>
#include <Memory.h>
#include <AppleEvents.h>
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
    {
        uint32_t started=(uint32_t)TickCount(),maximum=0;int r;
        /* Start directly: the acceptance harness services and times each step. */
        r=!strcmp(call.name,"read_text") ? read_text_begin(&call,result,sizeof(result),journal,NULL,started) :
          !strcmp(call.name,"edit_text") ? edit_text_begin(&call,result,sizeof(result),journal,NULL,started) :
          tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
        while(r==2) {
            EventRecord event;uint32_t before,elapsed;const char *phase=tools_text_phase();int stop=0;
            if(WaitNextEvent(everyEvent,&event,0,NULL)) {
                if(event.what==kHighLevelEvent)AEProcessAppleEvent(&event);
                if(event.what==keyDown && (event.modifiers & cmdKey) && (event.message & charCodeMask)=='.')stop=1;
            }
            SystemTask();before=(uint32_t)TickCount();
            r=tools_text_step(result,sizeof(result),before,stop);elapsed=(uint32_t)(TickCount()-before);
            if(elapsed>maximum)maximum=elapsed;
            fprintf(logfile,"STEP phase=%s ticks=%lu free_heap=%ld\n",phase,(unsigned long)elapsed,(long)FreeMem());
        }
        stopped=r;
        fprintf(logfile,"TIMING tool=%s total_ticks=%lu maximum_step_ticks=%lu free_heap=%ld\n",call.name,
            (unsigned long)((uint32_t)TickCount()-started),(unsigned long)maximum,(long)FreeMem());
    }
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
/* Native fixtures bypass model string/argument caps, with bounded writes. */
static void large_fixture(long length)
{
    FSSpec spec;short ref;OSErr err,c;long at;
    static char page[TOOLS_WORK_CHUNK];
    snprintf(path,sizeof(path),"%s:large%ld.c",folder,length);
    err=spec_for(path,&spec);if(err==fnfErr)err=FSpCreate(&spec,'ttxt','TEXT',smSystemScript);
    if(!err)err=FSpOpenDF(&spec,fsWrPerm,&ref);
    if(err) { check(0,"large fixture open");return; }
    memset(page,'x',sizeof(page));
    for(at=0;!err && at<length;) {
        EventRecord event;long n=length-at,wanted;
        if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
        wanted=n;if(at==0)memcpy(page+1022,"un",2);
        if(at==1024) { memset(page,'x',sizeof(page));memcpy(page,"ique",4); }
        err=FSWrite(ref,&n,page);if(!err && n!=wanted)err=ioErr;
        if(at==0 || at==1024)memset(page,'x',sizeof(page));
        at+=n;WaitNextEvent(everyEvent,&event,0,NULL);SystemTask();
    }
    c=FSClose(ref);if(!err)err=c;if(!err)err=FlushVol(NULL,spec.vRefNum);
    check(!err,"native large fixture");
}
static void verify_large(const char *relative,long length,int replacement)
{
    FSSpec spec;short ref;OSErr err,c;long at;
    static char page[TOOLS_WORK_CHUNK];
    err=spec_for(relative,&spec);if(!err)err=FSpOpenDF(&spec,fsRdPerm,&ref);
    if(err) { check(0,"large verify open");return; }
    for(at=0;!err && at<length;) {
        long n=length-at,wanted,i;if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;wanted=n;
        err=FSRead(ref,&n,page);if(!err && n!=wanted)err=ioErr;
        for(i=0;!err && i<n;i++) {
            long offset=at+i;
            char expected=offset>=1022 && offset<1028 ? (replacement ? "UNIQUE" : "unique")[offset-1022] : 'x';
            if(page[i]!=expected)err=ioErr;
        }
        at+=n;
    }
    c=FSClose(ref);check(!err && !c,"large exact bytes verified");
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
    edit(revision,"y","yy");check(strstr(result,"EDITED")!=NULL,"growth beyond old 4096 cap");
    for(int fixture=0;fixture<2;fixture++) {
        long length=fixture ? 65536 : 16384;
        large_fixture(length);read_page(0);field("revision",revision,sizeof(revision));
        check(strstr(result,"\"editable\":true")!=NULL,"large whole-file editable revision");
        read_page(12000);check(strstr(result,revision)!=NULL,"large revision independent of page");
        edit(revision,"unique","UNIQUE");check(strstr(result,"EDITED")!=NULL,"large exact edit");
        if(field("backup_path",backup,sizeof(backup)))verify_large(backup,length,0);
        verify_large(path,length,1);field("revision",updated,sizeof(updated));
        read_page(0);check(strstr(result,updated)!=NULL,"large fresh revision readback");
        if(length==65536) { before=records;edit(updated,"UNIQUE","UNIQUEx");check(strstr(result,"LIMIT")!=NULL && records==before,"growth above 64 KiB refused"); }
    }
    fprintf(logfile,"RESULT failures=%d stopped=%d fixture=%s\n",failures,stopped,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
