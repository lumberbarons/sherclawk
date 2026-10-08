/* Reuse the File Manager fault model to check descriptor rejection and source
 * revision binding before publication. Native compiler behavior is guest-tested. */
#define TEST_EXTERNAL_PROCESS_INFO 1
#define FSRead model_FSRead
#define FSClose model_FSClose
#define SetFPos model_SetFPos
#define main text_tools_main
#include "test_tools.c"
#undef main
#undef FSRead
#undef FSClose
#undef SetFPos
#include "build_project.h"
#include "run_application.h"
#include "toolserver.h"
#include "selfbuild.h"
#include <Processes.h>
/* Queued transport model: replies arrive only when explicitly released. */
static int ts_busy,ts_ready,ts_sends,ts_abandoned,ts_failure,ts_malformed,ts_send_error;
static char ts_directory[256],ts_command[2048];
OSErr toolserver_init(ToolServerLog log) { (void)log;return 0; }
void toolserver_close(void) { ts_busy=ts_ready=0; }
int toolserver_busy(void) { return ts_busy; }
OSErr toolserver_send(const char *directory,const char *command,uint32_t now)
{ (void)now;assert(!ts_busy);ts_sends++;strcpy(ts_directory,directory);strcpy(ts_command,command);ts_busy=1;ts_abandoned=0;return ts_send_error ? ioErr : 0; }
int toolserver_poll(uint32_t now,int stop,ToolServerReply *reply)
{
    (void)now;
    if(ts_busy && stop && !ts_abandoned) {ts_abandoned=1;return -1;}
    if(ts_busy && ts_ready) {
        memset(reply,0,sizeof(*reply));reply->abandoned=ts_abandoned;reply->malformed=ts_malformed;
        reply->status=ts_failure;strcpy(reply->diagnostic,ts_failure ? "compiler failure\r" : "");
        ts_busy=ts_ready=0;return 1;
    }
    return 0;
}
OSErr FSpDelete(const FSSpec *s) {int i=find(s->parID,s->name);if(i<0)return fnfErr;files[i].used=0;return 0;}
OSErr FSpGetFInfo(const FSSpec *s,FInfo *p) {int i=find(s->parID,s->name);if(i<0)return fnfErr;*p=files[i].info;return 0;}
OSErr FSpSetFInfo(const FSSpec *s,const FInfo *p) {int i=find(s->parID,s->name);if(i<0)return fnfErr;files[i].info=*p;return 0;}
static int launches,launch_error,process_error,run_journal_error;
static FSSpec launched;
static unsigned char resource_bytes[64][4096];
OSErr FSRead(short ref,long *n,void *out)
{
    if(ref<64)return model_FSRead(ref,n,out);
    ref-=64;
    if(*n>files[ref].resource-positions[ref])*n=files[ref].resource-positions[ref];
    memcpy(out,resource_bytes[ref]+positions[ref],(size_t)*n);positions[ref]+=*n;
    return 0;
}
OSErr FSClose(short ref) {return model_FSClose(ref>=64 ? ref-64 : ref);}
OSErr SetFPos(short ref,short mode,long at) {return model_SetFPos(ref>=64 ? ref-64 : ref,mode,at);}

OSErr FSpOpenRF(const FSSpec *s,short mode,short *ref)
{ OSErr e=FSpOpenDF(s,mode,ref); if(!e)*ref+=64; return e; }
OSErr LaunchApplication(LaunchParamBlockRec *p)
{ launches++;launched=*p->launchAppSpec;p->launchProcessSN.highLongOfPSN=0;p->launchProcessSN.lowLongOfPSN=42;return launch_error ? ioErr : 0; }
OSErr GetProcessInformation(const ProcessSerialNumber *p,ProcessInfoRec *i)
{ (void)p;*i->processAppSpec=launched;return process_error ? ioErr : 0; }
static int build_journals, build_result_failure;
static int build_journal(void *ctx,const char *event,const char *json)
{
    JsonToken t[128]; (void)ctx;
    if((run_journal_error==1 && !strcmp(event,"run_intent")) || (run_journal_error==2 && !strcmp(event,"run_observed")))return -1;
    if(build_result_failure && !strcmp(event,"build_result"))return -1;
    assert(json_parse(json,strlen(json),t,128)>0); build_journals++;
    return 0;
}
static int fixture(const char *descriptor_text)
{
    int d,i,w,q; reset();build_journals=0;
    d=add(10,"project",1); i=add(files[d].id,"project.json",0);
    strcpy(files[i].bytes,descriptor_text);files[i].size=(long)strlen(descriptor_text);files[i].info.fdType='TEXT';
    i=add(files[d].id,"main.c",0);strcpy(files[i].bytes,"int main(void) { return 0; }\r");files[i].size=(long)strlen(files[i].bytes);files[i].info.fdType='TEXT';
    w=add(10,"Worker01",1);q=add(files[w].id,"buildjobs",1);(void)q;
    strcpy(call.arguments,"{\"path\":\"project\"}");
    assert(build_project_begin(&call,result,sizeof(result),build_journal,NULL,1)==2);return i;
}
static int record_file(long parent,const char *name,const char *bytes)
{
    int i=add(parent,name,0);strcpy(files[i].bytes,bytes);files[i].size=(long)strlen(bytes);files[i].info.fdType='TEXT';return i;
}
static void terminal_check(const char *good,int artifact_ok)
{
    int i,r=2,dir,ready=-1;char terminal[200],build_id[25];
    fixture(good);
    for(i=0;i<100 && ready<0;i++) {
        assert(build_project_step(result,sizeof(result),(uint32_t)i+2,0)==2);
        for(int k=0;k<64;k++)if(files[k].used && !strcmp(files[k].name,"ready"))ready=k;
    }
    assert(ready>=0);dir=files[ready].parent;
    for(i=0;i<64;i++)if(files[i].used && files[i].id==dir)break;
    assert(i<64);  /* the fixture always records dir */
    strcpy(build_id,files[i].name);
    snprintf(terminal,sizeof(terminal),"protocol=1\nid=%s\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\n",build_id);
    record_file(dir,"result",terminal);
    i=add(dir,"build",1);i=add(files[i].id,"native",1);dir=files[i].id;
    record_file(dir,"success.txt","artifact=sample\n");
    i=record_file(dir,"sample","PEF bytes");files[i].info.fdType='APPL';files[i].resource=artifact_ok ? 1 : 0;
    r=build_project_step(result,sizeof(result),500,0);
    for(int step=0;r==2 && step<100;step++)r=build_project_step(result,sizeof(result),501+step,0);
    if(build_result_failure) { assert(r==1 && strstr(result,"JOURNAL_AFTER_BUILD"));
        for(int k=0;k<64;k++)assert(!files[k].used || strcmp(files[k].name,"launch.rec"));
        return; }
    assert(artifact_ok ? r==0 && strstr(result,"\"status\":\"ok\"") && strstr(result,"native:sample") : r==1 && strstr(result,"ARTIFACT_INVALID"));
}

#ifdef TEST_SELFBUILD
#define main build_contract_main
#endif
int main(void)
{
    const char *good="{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\"}\r";
    char recipe[12288];int i,r,steps;
    assert(!build_project_recipe(good,recipe,sizeof(recipe)));
    assert(strstr(recipe,"MrC") && strstr(recipe,"PPCLink -o sample") && !strstr(recipe,"-- Rez"));
    assert(build_project_recipe("{\"protocol\":1}",recipe,sizeof(recipe)));
    const char *bad[]={"main.c;echo","..:main.c",":main.c","src::main.c","src/main.c","main.C","Main.c"};
    for(i=0;i<7;i++) {
        char s[512];snprintf(s,sizeof(s),"{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"%s\"],\"output\":\"sample\"}",bad[i]);assert(build_project_recipe(s,recipe,sizeof(recipe)));
    }
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\",\"main.c\"],\"output\":\"sample\"}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\",\"settings\":{\"flags\":\"-o evil\"}}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\",\"settings\":{\"libraries\":[\"OtherLib\"]}}",recipe,sizeof(recipe)));
    assert(build_project_recipe("{\"protocol\":2,\"protocol\":2,\"toolchain\":\"mpw-ppc-v2\",\"sources\":[\"main.c\"],\"output\":\"sample\"}",recipe,sizeof(recipe)));
    i=fixture(good);assert(build_project_step(result,sizeof(result),2,0)==2);assert(build_project_step(result,sizeof(result),3,0)==2);
    assert(build_project_step(result,sizeof(result),4,0)==2);files[i].bytes[0]='x';
    assert(build_project_step(result,sizeof(result),5,0)==0 && strstr(result,"INPUT_CHANGED") && !creates && !build_journals);
    fixture(good);assert(build_project_step(result,sizeof(result),2,1)==1);
    strcpy(call.arguments,"{\"path\":\"project:\"}");assert(build_project_begin(&call,result,sizeof(result),build_journal,NULL,3)==2);
    assert(build_project_step(result,sizeof(result),4,1)==1);
    fixture(good);bad_close=1;assert(build_project_step(result,sizeof(result),2,0)==0 && strstr(result,"UNREADABLE") && !creates);
    fixture(good);assert(build_project_step(result,sizeof(result),2,1)==1 && strstr(result,"ABANDONED") && !creates);
    fixture(good);r=2;
    for(steps=0;r==2 && steps<100;steps++)r=build_project_step(result,sizeof(result),(uint32_t)(steps+2),0);
    assert(r==2);assert(build_project_step(result,sizeof(result),200,1)==1 && strstr(result,"uncertain"));
    assert(build_journals>=5);
    strcpy(call.name,"write_text");strcpy(call.arguments,"{\"path\":\"WORKER01:BUILDJOBS:fake\",\"text\":\"forged\"}");
    tools_execute_recorded(&call,result,sizeof(result),build_journal,NULL);
    assert(strstr(result,"EXECUTION_EVIDENCE_READ_ONLY"));
    build_result_failure=1;terminal_check(good,1);build_result_failure=0;
    terminal_check(good,0);terminal_check(good,1);
    { char bid[25],runargs[128]; int r;
      field("build_id",bid,sizeof(bid));
      snprintf(runargs,sizeof(runargs),"{\"build_id\":\"%s\"}",bid);
      strcpy(call.arguments,runargs);
      r=run_application_begin(&call,result,sizeof(result),build_journal,NULL,1000);assert(r==2);
      while(r==2)r=run_application_step(result,sizeof(result),1001,0);
      assert(r==0 && launches==1 && strstr(result,"LAUNCHED") && strstr(result,"process_present"));
      strcpy(call.arguments,runargs);assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1100)==2);
      assert(!run_application_step(result,sizeof(result),1101,1) && launches==1);
      run_journal_error=1;assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1200)==2);
      do {r=run_application_step(result,sizeof(result),1201,0);} while(r==2);
      assert(!r && launches==1 && strstr(result,"JOURNAL_BEFORE"));run_journal_error=0;
      launch_error=1;assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1300)==2);
      do {r=run_application_step(result,sizeof(result),1301,0);} while(r==2);
      assert(r==1 && launches==2 && strstr(result,"uncertain"));launch_error=0;
      process_error=1;assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1350)==2);
      do {r=run_application_step(result,sizeof(result),1351,0);} while(r==2);
      assert(r==1 && launches==3 && strstr(result,"PROCESS_OBSERVATION"));process_error=0;
      run_journal_error=2;assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1360)==2);
      do {r=run_application_step(result,sizeof(result),1361,0);} while(r==2);
      assert(r==1 && launches==4 && strstr(result,"JOURNAL_AFTER"));run_journal_error=0;
      for(int k=0;k<64;k++)if(files[k].used && !strcmp(files[k].name,"sample"))resource_bytes[k][0]^=1;
      assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1370)==2);
      do {r=run_application_step(result,sizeof(result),1371,0);} while(r==2);
      assert(!r && launches==4 && strstr(result,"ARTIFACT_CHANGED"));
      for(int k=0;k<64;k++)if(files[k].used && !strcmp(files[k].name,"sample"))resource_bytes[k][0]^=1;
      for(int k=0;k<64;k++)if(files[k].used && !strcmp(files[k].name,"sample"))files[k].bytes[0]^=1;
      assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1400)==2);
      do {r=run_application_step(result,sizeof(result),1401,0);} while(r==2);
      assert(!r && launches==4 && strstr(result,"ARTIFACT_CHANGED"));
      strcpy(call.arguments,"{\"build_id\":\"build-00000000-ffff\"}");
      assert(!run_application_begin(&call,result,sizeof(result),build_journal,NULL,1500) && launches==4);
      strcpy(call.arguments,"{\"build_id\":\"build-00000000-ffff\",\"path\":\"sample\"}");
      assert(!run_application_begin(&call,result,sizeof(result),build_journal,NULL,1500) && strstr(result,"RUN_ARGUMENTS"));
      snprintf(result,sizeof(result),"{\"build_id\":\"%s\"}",bid);
    }
    { char bid[25];int dir=0,log;JsonToken t[64];
      field("build_id",bid,sizeof(bid));
      for(i=0;i<64;i++)if(files[i].used && !strcmp(files[i].name,bid))dir=(int)files[i].id;
      assert(dir);log=record_file(dir,"stdout","");memset(files[log].bytes,'\r',512);files[log].size=512;
      snprintf(call.arguments,sizeof(call.arguments),"{\"build_id\":\"%s\",\"stream\":\"stdout\"}",bid);
      build_project_log(&call,result,sizeof(result));assert(json_parse(result,strlen(result),t,64)>0 && strstr(result,"\"next_byte\":128") && strstr(result,"\"truncated\":true"));
      files[log].info.fdFlags=0x8000;build_project_log(&call,result,sizeof(result));assert(strstr(result,"error"));
    }
    puts("PASS launch authorization, both fork changes, Stop, unknown IDs, journal barriers, process observation and uncertain launch errors");
    puts("PASS build descriptors, metacharacters, duplicate/unsupported settings, source changes, read/close faults, snapshot publication and Stop");
    return 0;
}
