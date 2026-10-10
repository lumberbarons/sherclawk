/* Native move_to_trash acceptance on the real volume: log what
 * FSFindFolder(kTrashFolderType) resolves to, move a pinned fixture with one
 * same-volume rename, verify the destination catalog identity and that the
 * source path is gone, then move the fixture back to show recovery. The tool
 * never deletes and never empties a Trash; the fixture folder is preserved. */
#include "tools.h"
#include "json.h"
#include "text.h"
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
static char result[AGENT_RESULT_CAP], folder[80], path[200], dest[400], arguments[1024];
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
static int moved_as(char *out, size_t cap)
{
    JsonToken t[128]; int files, first;
    if(json_parse(result,strlen(result),t,128)<1)return 0;
    files=json_member(result,t,0,"files");
    if(files<0||t[files].type!=JSON_ARRAY)return 0;
    first=files+1; if(first>=t[files].next)return 0;
    return json_string(result,t,json_member(result,t,first,"moved_as"),out,cap)>=0;
}
static int catalog_at(const char *relative, CInfoPBRec *pb)
{
    FSSpec spec;
    memset(pb,0,sizeof(*pb));
    if(tools_resolve(relative,&spec))return -1;
    pb->hFileInfo.ioNamePtr=spec.name;pb->hFileInfo.ioVRefNum=spec.vRefNum;pb->hFileInfo.ioDirID=spec.parID;
    return PBGetCatInfoSync(pb);
}
int main(void)
{
    FSSpec ws, moved;
    CInfoPBRec before, after, parent;
    short trash_vref=0;
    long trash_id=0, parent_id=0;
    char revision[48], leaf[96], trash_text[300], leaf_native[64];
    long bytes_before=0;
    unsigned long type_before=0, creator_before=0;
    OSErr err;
    int moved_native=0, nlen;
    revision[0]=0;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkTrashCheck.log","w");if(!logfile)return 1;
    memset(&call,0,sizeof(call));strcpy(call.id,"native-trash");
    snprintf(folder,sizeof(folder),"Sherclawk Trash Check %08lx",(unsigned long)TickCount());
    snprintf(path,sizeof(path),"%s:fixture.c",folder);
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\"}",folder);
    tool_call("create_folder",arguments);
    check(strstr(result,"CREATED_FOLDER")!=NULL,"fixture folder created");
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\",\"text\":\"copper lobster\\r\"}",path);
    tool_call("write_text",arguments);
    check(strstr(result,"CREATED")!=NULL,"fixture text written");

    /* The pin comes from the model-facing listing, not a private shortcut. */
    snprintf(arguments,sizeof(arguments),"{\"path\":\"%s\"}",path);
    tool_call("get_file_info",arguments);
    check(field("revision",revision,sizeof(revision)) && !strncmp(revision,"cat-",4),"catalog revision emitted");
    check(!catalog_at(path,&before),"fixture catalog readable");
    bytes_before=before.hFileInfo.ioFlLgLen;
    type_before=before.hFileInfo.ioFlFndrInfo.fdType;
    creator_before=before.hFileInfo.ioFlFndrInfo.fdCreator;
    check(!catalog_at(folder,&parent),"fixture folder catalog readable");
    parent_id=parent.dirInfo.ioDrDirID;

    /* Evidence: resolve the volume Trash independently of the tool. */
    err=tools_workspace_root(&ws);
    check(!err,"workspace root spec");
    err=FindFolder(ws.vRefNum,kTrashFolderType,0,&trash_vref,&trash_id);
    if(err) fprintf(logfile,"NOTE FindFolder error=%d; the tool must use the workspace fallback\n",(int)err);
    else {
        fprintf(logfile,"NOTE volume Trash vRefNum=%d dirID=%ld\n",(int)trash_vref,trash_id);
        check(trash_vref==ws.vRefNum && trash_id!=0,"volume Trash resolves on the same volume");
    }

    /* The move itself: one pinned same-volume rename, journaled. */
    snprintf(arguments,sizeof(arguments),"{\"files\":[{\"path\":\"%s\",\"revision\":\"%s\"}]}",path,revision);
    tool_call("move_to_trash",arguments);
    check(strstr(result,"TRASHED")!=NULL && records>=2,"pinned file moved with journal records");
    check(catalog_at(path,&after)!=0,"source path is gone after the move");

    /* Verify the destination catalog identity through the reported name. */
    nlen=moved_as(leaf,sizeof(leaf)) ? text_to_macroman_strict(leaf,leaf_native,sizeof(leaf_native)) : -1;
    if(nlen>0) {
        memmove(leaf_native+1,leaf_native,(size_t)nlen);
        leaf_native[0]=(char)nlen;
        if(trash_id) {
            err=FSMakeFSSpec(trash_vref,trash_id,(const unsigned char *)leaf_native,&moved);
            moved_native=!err;
        }
        if(!moved_native && field("trash",trash_text,sizeof(trash_text)) && trash_text[0]) {
            snprintf(dest,sizeof(dest),"%s%s",trash_text,leaf);
            moved_native=!tools_resolve(dest,&moved);
        }
        check(moved_native,"moved file resolves at its reported Trash name");
        if(moved_native) {
            memset(&after,0,sizeof(after));
            after.hFileInfo.ioNamePtr=moved.name;after.hFileInfo.ioVRefNum=moved.vRefNum;after.hFileInfo.ioDirID=moved.parID;
            err=PBGetCatInfoSync(&after);
            check(!err && after.hFileInfo.ioFlLgLen==bytes_before && !after.hFileInfo.ioFlRLgLen &&
                after.hFileInfo.ioFlFndrInfo.fdType==type_before &&
                after.hFileInfo.ioFlFndrInfo.fdCreator==creator_before,
                "data size, type/creator and empty resource fork preserved");
            if(!err) {
                FSSpec restored;
                err=FSMakeFSSpec(ws.vRefNum,parent_id,(const unsigned char *)"\012restored.c",&restored);
                if(!err)err=FSpCatMove(&moved,&restored);
                check(!err,"Trash item moved back to the fixture folder");
                check(!catalog_at(folder,&after),"fixture folder still readable");
                snprintf(dest,sizeof(dest),"%s:restored.c",folder);
                check(!catalog_at(dest,&after) && after.hFileInfo.ioFlLgLen==bytes_before,
                    "restored file reads back with the original bytes");
            }
        }
    } else check(0,"moved_as reported");

    fprintf(logfile,"RESULT failures=%d fixture=%s\n",failures,folder);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
