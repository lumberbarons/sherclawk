/* Native copy_file acceptance on the real volume: create a fixture with
 * both forks and Finder identity, pin it through get_file_info, publish a
 * copy under a new name, verify the destination catalog identity and both
 * fork bytes against the fixture, confirm the source is untouched, and
 * check that a second copy onto the same name is refused. The fixture
 * folder is preserved for inspection. */
#include "tools/tools.h"
#include "agent/json.h"
#include "agent/text.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include "diagnostic-tools.h"
static FILE *logfile;
static int records, failures;
static AgentCall call;
static char result[AGENT_RESULT_CAP], folder[80], source[200], destination[200], arguments[1024];
static char data_bytes[3000], resource_bytes[1500], observed[3000];
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
static int tool_call(const char *name, const char *args)
{
    int stop;
    strcpy(call.name,name); strcpy(call.arguments,args);
    stop=tools_execute_recorded(&call,result,sizeof(result),journal,NULL);
    fprintf(logfile,"%s stop=%d %s\n",name,stop,result);fflush(logfile);
    return stop;
}
static int field(const char *name, char *out, size_t cap)
{
    JsonToken t[128];
    if(json_parse(result,strlen(result),t,128)<1)return 0;
    return json_string(result,t,json_member(result,t,0,name),out,cap)>=0;
}
static int catalog_at(const char *relative, CInfoPBRec *pb)
{
    FSSpec spec;
    memset(pb,0,sizeof(*pb));
    if(tools_resolve(relative,&spec))return -1;
    pb->hFileInfo.ioNamePtr=spec.name;pb->hFileInfo.ioVRefNum=spec.vRefNum;pb->hFileInfo.ioDirID=spec.parID;
    return PBGetCatInfoSync(pb);
}
/* One whole fork read back into a bounded buffer for byte comparison. */
static OSErr read_fork(const char *relative, int resource, char *buf, long size)
{
    FSSpec spec;
    short ref;
    long count=size;
    OSErr err, closed;
    if(tools_resolve(relative,&spec))return -1;
    err=resource ? FSpOpenRF(&spec,fsRdPerm,&ref) : FSpOpenDF(&spec,fsRdPerm,&ref);
    if(err)return err;
    err=FSRead(ref,&count,buf);
    closed=FSClose(ref);
    if(!err && count!=size)err=ioErr;
    return err ? err : closed;
}
int main(void)
{
    CInfoPBRec before, after;
    char revision[48];
    unsigned long i, type_before=0, creator_before=0;
    unsigned short flags_masked=0;
    long data_size, resource_size;
    OSErr err;
    revision[0]=0;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkCopyCheck.log","w");if(!logfile)return 1;
    memset(&call,0,sizeof(call));strcpy(call.id,"native-copy");
    snprintf(folder,sizeof(folder),"Sherclawk Copy Check %08lx",(unsigned long)TickCount());
    snprintf(source,sizeof(source),"%s:fixture%08lx",folder,(unsigned long)TickCount());
    snprintf(destination,sizeof(destination),"%s:published%08lx",folder,(unsigned long)TickCount());
    /* The fixture folder, then a native fixture with both forks: write_text
     * cannot produce resource forks or Finder flags, so the diagnostic
     * builds one with the File Manager directly. */
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\"}",folder);
    tool_call("create_folder",arguments);
    check(strstr(result,"CREATED_FOLDER")!=NULL,"fixture folder created");
    data_size=(long)sizeof(data_bytes); resource_size=(long)sizeof(resource_bytes);
    for(i=0;i<sizeof(data_bytes);i++) data_bytes[i]=(char)(i*7+3);
    for(i=0;i<sizeof(resource_bytes);i++) resource_bytes[i]=(char)(i*11+5);
    {
        FSSpec spec;
        short ref;
        long count;
        FInfo info;
        err=tools_resolve(source,&spec);
        check(err==fnfErr,"fixture name free before creation");
        /* FSpCreate reserves the name with the Finder identity the copy must preserve. */
        err=FSpCreate(&spec,'ShCk','APPL',smSystemScript);
        check(!err,"fixture created with type and creator");
        err=FSpOpenDF(&spec,fsWrPerm,&ref);
        if(!err) {
            count=data_size; err=FSWrite(ref,&count,data_bytes);
            if(!err && count!=data_size) err=ioErr;
            { OSErr closed=FSClose(ref); if(!err) err=closed; }
        }
        check(!err,"fixture data fork written");
        err=FSpOpenRF(&spec,fsWrPerm,&ref);
        if(!err) {
            count=resource_size; err=FSWrite(ref,&count,resource_bytes);
            if(!err && count!=resource_size) err=ioErr;
            { OSErr closed=FSClose(ref); if(!err) err=closed; }
        }
        check(!err,"fixture resource fork written");
        if(!FSpGetFInfo(&spec,&info)) {
            info.fdFlags=(unsigned short)(0x0400|info.fdFlags); /* a custom-icon flag survives a copy */
            flags_masked=(unsigned short)(info.fdFlags & ~0x8001u);
            check(!FSpSetFInfo(&spec,&info),"fixture Finder flags set");
        }
    }
    check(!catalog_at(source,&before),"fixture catalog readable");
    type_before=before.hFileInfo.ioFlFndrInfo.fdType;
    creator_before=before.hFileInfo.ioFlFndrInfo.fdCreator;

    /* The pin comes from the model-facing listing, not a private shortcut. */
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\"}",source);
    tool_call("get_file_info",arguments);
    check(field("revision",revision,sizeof(revision)) && !strncmp(revision,"cat-",4),"catalog revision emitted");

    /* The copy itself: pinned, journaled, both forks verified. */
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\",\"revision\":\"%s\",\"destination\":\"%s\"}",
        source,revision,destination);
    tool_call("copy_file",arguments);
    check(strstr(result,"COPIED")!=NULL && records>=3,"pinned copy published with journal records");
    check(!catalog_at(destination,&after),"destination catalog readable");
    check(after.hFileInfo.ioFlLgLen==data_size && after.hFileInfo.ioFlRLgLen==resource_size,
        "both fork sizes preserved");
    check(after.hFileInfo.ioFlFndrInfo.fdType==type_before && after.hFileInfo.ioFlFndrInfo.fdCreator==creator_before,
        "Finder type and creator preserved");
    check((after.hFileInfo.ioFlFndrInfo.fdFlags & ~0x8001u)==flags_masked,
        "Finder flags preserved without lock and alias bits");
    err=read_fork(destination,0,observed,data_size);
    check(!err && !memcmp(observed,data_bytes,(size_t)data_size),"data fork bytes identical");
    err=read_fork(destination,1,observed,resource_size);
    check(!err && !memcmp(observed,resource_bytes,(size_t)resource_size),"resource fork bytes identical");
    err=catalog_at(source,&after);
    check(!err && after.hFileInfo.ioFlLgLen==data_size && after.hFileInfo.ioFlRLgLen==resource_size &&
        after.hFileInfo.ioFlFndrInfo.fdType==type_before,"source untouched");

    /* A copy onto the published name is refused before any I/O. */
    records=0;
    tool_call("copy_file",arguments);
    check(strstr(result,"EXISTS")!=NULL && records==0,"existing destination refused without journal records");

    fprintf(logfile,"RESULT failures=%d fixture=%s\n",failures,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
