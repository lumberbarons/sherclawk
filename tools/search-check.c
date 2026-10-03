/* Native search acceptance: exercise real AFP catalog pagination and a
 * search/read/edit/read cycle. Preserve fixtures and recovery backups. */
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
static AgentCall call;
static char result[AGENT_RESULT_CAP];
static int failures;
static int journal(void *ctx,const char *event,const char *json)
{
    (void)ctx;
    return fprintf(logfile,"%s %s\n",event,json)<0 || fflush(logfile) || FlushVol(NULL,0);
}
static void execute(void)
{
    if(tools_execute_recorded(&call,result,sizeof(result),journal,NULL))failures++;
    fprintf(logfile,"%s %s\n",call.name,result);fflush(logfile);
}
static void check(int ok,const char *name)
{
    fprintf(logfile,"%s %s\n",ok ? "PASS" : "FAIL",name);if(!ok)failures++;fflush(logfile);
}
static int field(const char *key,char *out,size_t cap)
{
    JsonToken tokens[128];
    return json_parse(result,strlen(result),tokens,128)>0 && json_string(result,tokens,json_member(result,tokens,0,key),out,cap)>=0;
}
static OSErr folder_create(const char *relative)
{
    FSSpec spec;Str255 p;char full[256];long dir;OSErr err;
    snprintf(full,sizeof(full),"%s%s",SHERCLAWK_WORKSPACE,relative);p[0]=(unsigned char)strlen(full);memcpy(p+1,full,p[0]);
    err=FSMakeFSSpec(0,0,p,&spec);return err==fnfErr ? FSpDirCreate(&spec,smSystemScript,&dir) : dupFNErr;
}
int main(void)
{
    char folder[80], sub[100], path[140], cursor[256], revision[80], updated[80];
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkSearchCheck.log","w");if(!logfile)return 1;
    snprintf(folder,sizeof(folder),"Sherclawk Search %08lx",(unsigned long)TickCount());
    snprintf(sub,sizeof(sub),"%s:Sources",folder);snprintf(path,sizeof(path),"%s:hello.c",sub);
    check(!folder_create(folder) && !folder_create(sub),"fixture folders");
    strcpy(call.id,"native-search");strcpy(call.name,"write_text");
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"text\":\"/* caf\\u00e9 */\\nconst char *message = \\\"copper lobster\\\";\\n/* lobster */\\n\"}",path);execute();check(strstr(result,"CREATED")!=NULL,"fixture text");
    strcpy(call.name,"search_text");snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"%s\",\"query\":\"lobster\"}",folder);execute();check(strstr(result,"\"matches\":[]") && strstr(result,"\"truncated\":false"),"nonrecursive excludes child");
    snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"%s\",\"query\":\"lobster\",\"recursive\":true,\"limit\":1}",folder);execute();check(strstr(result,path) && strstr(result,"\"line\":2") && field("next_cursor",cursor,sizeof(cursor)),"recursive match and cursor");
    snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"%s\",\"query\":\"lobster\",\"recursive\":true,\"limit\":1,\"cursor\":\"%s\"}",folder,cursor);execute();check(strstr(result,"\"line\":3") && field("next_cursor",cursor,sizeof(cursor)),"continuation next match");
    snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"%s\",\"query\":\"lobster\",\"recursive\":true,\"cursor\":\"%s\"}",folder,cursor);execute();check(strstr(result,"\"truncated\":false")!=NULL,"completion");
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",path);execute();check(field("revision",revision,sizeof(revision)),"read match before edit");
    strcpy(call.name,"edit_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\",\"expected_revision\":\"%s\",\"old_text\":\"copper lobster\",\"new_text\":\"silver lobster\"}",path,revision);execute();check(strstr(result,"EDITED") && field("revision",updated,sizeof(updated)),"guarded edit of discovered source");
    strcpy(call.name,"read_text");snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",path);execute();check(strstr(result,"silver lobster") && strstr(result,updated),"verify edit");
    strcpy(call.name,"search_text");snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"%s\",\"query\":\"caf\\u00e9\",\"recursive\":true}",folder);execute();check(strstr(result,"\"line\":1")!=NULL,"MacRoman query");
    fprintf(logfile,"RESULT failures=%d\n",failures);fclose(logfile);return failures ? 1 : 0;
}
