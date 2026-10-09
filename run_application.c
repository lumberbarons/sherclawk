/* LaunchApplication needs a fork-aware FSSpec, never a shell path. Successful
 * builds seal both forks in a private Finder-typed record. Recheck in bounded
 * steps, journal intent before launch, and never retry an uncertain launch.
 * FNV hashes detect changes, not hostile AFP-server writes or power loss. */
#include "run_application.h"
#include "application_process.h"
#include "json.h"
#include "config.h"
#include <Processes.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
#define QUEUE SHERCLAWK_BUILD_QUEUE
#define FORK_LIMIT (1024L*1024L)
typedef struct {
    uint32_t magic;
    char build[25], output[32];
    uint32_t file_id, modified, creator, data_size, resource_size, data_hash, resource_hash;
} Authority;
static Authority seal, observed;
static FSSpec artifact;
static char build_id[25], run_id[32];
static int active, authorizing, fork_index, tracking_done;
static long offset;
static uint32_t started;
static unsigned long sequence;
static AgentJournal journal_fn;
static void *journal_context;
static OSErr catalog(FSSpec *s,CInfoPBRec *p)
{
    memset(p,0,sizeof(*p)); p->hFileInfo.ioNamePtr=s->name;
    p->hFileInfo.ioVRefNum=s->vRefNum; p->hFileInfo.ioDirID=s->parID;
    return PBGetCatInfoSync(p);
}
static int leaf_valid(const char *s,size_t max)
{
    size_t i,n=strlen(s);
    if(!n || n>max || s[0]=='.' || s[0]=='-')return 0;
    for(i=0;i<n;i++)if(!((s[i]>='a' && s[i]<='z') || (s[i]>='0' && s[i]<='9') || s[i]=='_' || s[i]=='-' || s[i]=='.'))return 0;
    return 1;
}
static int id_valid(const char *s)
{
    size_t i;
    if(strlen(s)!=19 || strncmp(s,"build-",6) || s[14]!='-')return 0;
    for(i=6;i<19;i++)if(i!=14 && !((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f')))return 0;
    return 1;
}
static int record_spec(const char *leaf,FSSpec *s)
{
    char path[160]; snprintf(path,sizeof(path),QUEUE ":%s:%s",build_id,leaf);
    return tools_resolve(path,s);
}
static int read_record(Authority *a)
{
    FSSpec s; CInfoPBRec p; short ref; long n=sizeof(*a); OSErr e,c;
    if(record_spec("launch.rec",&s) || catalog(&s,&p) || (p.hFileInfo.ioFlAttrib & 16) ||
        (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || p.hFileInfo.ioFlFndrInfo.fdType!='ShAR' ||
        p.hFileInfo.ioFlFndrInfo.fdCreator!='ShCk' || p.hFileInfo.ioFlRLgLen || p.hFileInfo.ioFlLgLen!=n || FSpOpenDF(&s,fsRdPerm,&ref))return -1;
    e=FSRead(ref,&n,a); c=FSClose(ref);
    if(e || c || n!=sizeof(*a) || a->magic!=0x53484131UL || !memchr(a->build,0,sizeof(a->build)) ||
        !memchr(a->output,0,sizeof(a->output)) || strcmp(a->build,build_id) || !leaf_valid(a->output,31))return -1;
    return 0;
}
static int metadata(Authority *a)
{
    FSSpec s; CInfoPBRec p; char path[192];
    snprintf(path,sizeof(path),QUEUE ":%s:build:native:%s",build_id,seal.output);
    if(tools_resolve(path,&s) || catalog(&s,&p) || (p.hFileInfo.ioFlAttrib & 16) ||
        (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || p.hFileInfo.ioFlFndrInfo.fdType!='APPL' ||
        p.hFileInfo.ioFlLgLen<=0 || p.hFileInfo.ioFlLgLen>FORK_LIMIT ||
        p.hFileInfo.ioFlRLgLen<=0 || p.hFileInfo.ioFlRLgLen>FORK_LIMIT)return -1;
    artifact=s;
    a->file_id=(uint32_t)p.hFileInfo.ioDirID; a->modified=(uint32_t)p.hFileInfo.ioFlMdDat;
    a->creator=(uint32_t)p.hFileInfo.ioFlFndrInfo.fdCreator;
    a->data_size=(uint32_t)p.hFileInfo.ioFlLgLen; a->resource_size=(uint32_t)p.hFileInfo.ioFlRLgLen;
    return 0;
}
static int same_metadata(const Authority *a,const Authority *b)
{
    return a->file_id==b->file_id && a->modified==b->modified && a->creator==b->creator &&
        a->data_size==b->data_size && a->resource_size==b->resource_size;
}
static int fail(char *out,size_t cap,const char *code,int uncertain)
{
    active=0;
    snprintf(out,cap,"{\"status\":\"%s\",\"code\":\"%s\",\"build_id\":\"%s\",\"run_id\":\"%s\"}",uncertain ? "uncertain" : "error",code,build_id,run_id);
    return uncertain;
}
static int begin(int authorize,AgentJournal journal,void *ctx,uint32_t now)
{
    memset(&observed,0,sizeof(observed)); observed.magic=seal.magic;
    strcpy(observed.build,seal.build); strcpy(observed.output,seal.output);
    if(!journal || metadata(&observed))return -1;
    if(!authorize && !same_metadata(&observed,&seal))return -1;
    observed.data_hash=observed.resource_hash=2166136261UL;
    authorizing=authorize; fork_index=0; offset=0; started=now;
    journal_fn=journal; journal_context=ctx; active=1; tracking_done=0;
    if(!authorize)application_tracking_begin();
    return 2;
}
int application_authorize_begin(const char *id,const char *output,AgentJournal journal,void *ctx,uint32_t now)
{
    if(active || !id_valid(id) || !leaf_valid(output,31))return -1;
    memset(&seal,0,sizeof(seal)); seal.magic=0x53484131UL;
    strcpy(build_id,id); strcpy(seal.build,id); strcpy(seal.output,output); run_id[0]=0;
    return begin(1,journal,ctx,now);
}
int run_application_begin(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *ctx,uint32_t now)
{
    JsonToken t[8]; char id[25],key[32];
    if(active) { snprintf(out,cap,"{\"status\":\"error\",\"code\":\"LAUNCH_BUSY\"}");return 1; }
    build_id[0]=run_id[0]=0;
    if(json_parse(call->arguments,strlen(call->arguments),t,8)!=3 || t[0].type!=JSON_OBJECT ||
        json_string(call->arguments,t,1,key,sizeof(key))<0 || strcmp(key,"build_id") ||
        json_string(call->arguments,t,2,id,sizeof(id))<0 || !id_valid(id))return fail(out,cap,"RUN_ARGUMENTS",0);
    strcpy(build_id,id);
    if(!journal)return fail(out,cap,"JOURNAL_REQUIRED",1);
    if(read_record(&seal) || begin(0,journal,ctx,now)<0)return fail(out,cap,"BUILD_NOT_AUTHORIZED_OR_ARTIFACT_CHANGED",0);
    return 2;
}
static int persist(void)
{
    FSSpec s; short ref; long n=sizeof(observed); OSErr e,c; Authority verify;
    if(record_spec("launch.tmp",&s)!=fnfErr || FSpCreate(&s,'ShCk','ShAR',smSystemScript) || FSpOpenDF(&s,fsWrPerm,&ref))return -1;
    e=FSWrite(ref,&n,&observed); c=FSClose(ref);
    if(e || c || n!=sizeof(observed) || FlushVol(NULL,s.vRefNum))return -1;
    /* Verify the closed staged bytes before exposing the authorization name. */
    if(FSpOpenDF(&s,fsRdPerm,&ref))return -1;
    n=sizeof(verify); e=FSRead(ref,&n,&verify); c=FSClose(ref);
    if(e || c || n!=sizeof(verify) || memcmp(&verify,&observed,sizeof(verify)) ||
        FSpRename(&s,(const unsigned char *)"\012launch.rec") || FlushVol(NULL,s.vRefNum) ||
        read_record(&verify) || memcmp(&verify,&observed,sizeof(verify)))return -1;
    return 0;
}
/* Reserve the run ID in the queue, across app restarts and guest reboots.
 * Retain this closed intent record even if launch/observation is uncertain. */
static int reserve_run(const char *json)
{
    char path[128],verify[512]; FSSpec s; short ref;
    long n=(long)strlen(json),got=n; OSErr e,c;
    if(n>=(long)sizeof(verify))return -1;
    snprintf(path,sizeof(path),QUEUE ":%s",run_id);
    if(tools_resolve(path,&s)!=fnfErr || FSpCreate(&s,'ShCk','ShRR',smSystemScript) || FSpOpenDF(&s,fsWrPerm,&ref))return -1;
    e=FSWrite(ref,&got,json); c=FSClose(ref);
    if(e || c || got!=n || FlushVol(NULL,s.vRefNum) || FSpOpenDF(&s,fsRdPerm,&ref))return -1;
    got=n;e=FSRead(ref,&got,verify);c=FSClose(ref);
    return e || c || got!=n || memcmp(json,verify,(size_t)n) ? -1 : 0;
}
int run_application_step(char *out,size_t cap,uint32_t now,int stop)
{
    Authority current=observed; short ref; OSErr e,c; long n;
    unsigned char page[1024]; uint32_t *h;
    if(!active)return fail(out,cap,"NO_ACTIVE_LAUNCH",1);
    if(stop || ((authorizing || fork_index<2) && (uint32_t)(now-started)>=60UL*60UL))return fail(out,cap,"LAUNCH_VERIFICATION_STOPPED",0);
    if(metadata(&current) || !same_metadata(&current,&observed))return fail(out,cap,"ARTIFACT_CHANGED",0);
    if(fork_index<2) {
        n=(long)(fork_index ? observed.resource_size : observed.data_size)-offset;
        if(n>1024)n=1024;
        e=fork_index ? FSpOpenRF(&artifact,fsRdPerm,&ref) : FSpOpenDF(&artifact,fsRdPerm,&ref);
        if(e)return fail(out,cap,"ARTIFACT_FORK_OPEN",0);
        e=SetFPos(ref,fsFromStart,offset); long got=n;
        if(!e)e=FSRead(ref,&got,page);
        c=FSClose(ref);
        if(e || c || got!=n)return fail(out,cap,"ARTIFACT_FORK_READ",0);
        h=fork_index ? &observed.resource_hash : &observed.data_hash;
        for(long i=0;i<n;i++)*h=(*h^page[i])*16777619UL;
        offset+=n;
        if(offset==(long)(fork_index ? observed.resource_size : observed.data_size)) {fork_index++;offset=0;}
        return 2;
    }
    if(authorizing) {
        snprintf(out,cap,"{\"status\":\"ok\",\"build_id\":\"%s\",\"snapshot\":\"" QUEUE ":%s\",\"launch_supported\":true}",build_id,build_id);
        if(journal_fn(journal_context,"artifact_authorization_intent",out) ||
            journal_fn(journal_context,"artifact_authorization_ready",out) || persist())return fail(out,cap,"AUTHORIZATION_PERSISTENCE",1);
        active=0;return 0;
    }
    if(memcmp(&seal,&observed,sizeof(seal)))return fail(out,cap,"ARTIFACT_CHANGED",0);
    /* Recheck the authority after the multi-turn fork scan. */
    if(read_record(&current) || memcmp(&seal,&current,sizeof(seal)))return fail(out,cap,"AUTHORITY_CHANGED",0);
    if(!tracking_done) {
        /* Tracking must not turn an otherwise valid launch into a failure. */
        if((uint32_t)(now-started)>=60UL*60UL)application_tracking_expire();
        else if(application_tracking_step()==2)return 2;
        tracking_done=1;
    }
    snprintf(run_id,sizeof(run_id),"run-%08lx-%04lx",(unsigned long)now,(++sequence)&0xffffUL);
    snprintf(out,cap,"{\"status\":\"pending\",\"run_id\":\"%s\",\"build_id\":\"%s\",\"snapshot\":\"" QUEUE ":%s\"}",run_id,build_id,build_id);
    if(journal_fn(journal_context,"run_intent",out))return fail(out,cap,"JOURNAL_BEFORE_LAUNCH",0);
    if(reserve_run(out))return fail(out,cap,"RUN_RESERVATION_FAILED_NO_LAUNCH",1);
    LaunchParamBlockRec launch; ProcessInfoRec process; FSSpec actual; Str255 name;
    memset(&launch,0,sizeof(launch)); launch.launchBlockID=extendedBlock;
    launch.launchEPBLength=extendedBlockLen; launch.launchFileFlags=0;
    launch.launchControlFlags=launchContinue | launchDontSwitch | launchNoFileFlags;
    launch.launchAppSpec=&artifact;
    e=LaunchApplication(&launch);
    active=0;
    /* A launch error does not prove absence of side effects. */
    if(e)return fail(out,cap,"LAUNCH_OS_ERROR_NO_RETRY",1);
    memset(&process,0,sizeof(process)); process.processInfoLength=sizeof(process);
    process.processName=name; process.processAppSpec=&actual;
    e=GetProcessInformation(&launch.launchProcessSN,&process);
    if(e || actual.vRefNum!=artifact.vRefNum || actual.parID!=artifact.parID ||
        actual.name[0]!=artifact.name[0] || memcmp(actual.name,artifact.name,(size_t)artifact.name[0]+1))return fail(out,cap,"PROCESS_OBSERVATION_UNCERTAIN",1);
    const char *reason=application_tracking_reason(&launch.launchProcessSN,&process);
    snprintf(out,cap,"{\"quit_supported\":%s,\"quit_reason\":\"%s\",\"original_run_id\":\"%s\",\"status\":\"ok\",\"code\":\"LAUNCHED\",\"run_id\":\"%s\",\"build_id\":\"%s\",\"snapshot\":\"" QUEUE ":%s\",\"artifact\":\"" QUEUE ":%s:build:native:%s\",\"process\":{\"high\":%lu,\"low\":%lu},\"observation\":\"process_present\",\"smoke_test\":\"not_performed\"}",reason ? "false" : "true",reason ? reason : "",application_original_run(&launch.launchProcessSN),run_id,build_id,build_id,build_id,seal.output,(unsigned long)launch.launchProcessSN.highLongOfPSN,(unsigned long)launch.launchProcessSN.lowLongOfPSN);
    if(journal_fn(journal_context,"run_observed",out))return fail(out,cap,"JOURNAL_AFTER_LAUNCH_NO_RETRY",1);
    if(!reason)application_tracking_commit(run_id,&launch.launchProcessSN,&artifact,&process);
    return 0;
}
