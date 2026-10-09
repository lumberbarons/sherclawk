/* Issue #102: fail-closed ownership, queued noninteractive Quit, no replay. */
#include "application_process.h"
#include "ae_dispatch.h"
#include "json.h"
#include <AERegistry.h>
#include <stdio.h>
#include <string.h>
#define OWNERS 32
#define SNAPSHOT 256
static struct {
    char id[32]; ProcessSerialNumber psn; FSSpec artifact;
    unsigned long launch_date; int attempted;
} owned[OWNERS];
static ProcessSerialNumber before[SNAPSHOT], cursor, self;
static int reclaim, count, slot;
static const char *tracking_reason;
static int quitting, sent, owner, reply_received, malformed;
static int32_t reply_error;
static short return_id;
static uint32_t submitted;
static AgentJournal quit_journal;
static void *quit_context;
static int same_psn(const ProcessSerialNumber *a,const ProcessSerialNumber *b)
{ return a->highLongOfPSN==b->highLongOfPSN && a->lowLongOfPSN==b->lowLongOfPSN; }
static int same_spec(const FSSpec *a,const FSSpec *b)
{ return a->vRefNum==b->vRefNum && a->parID==b->parID && a->name[0]==b->name[0] && !memcmp(a->name,b->name,(size_t)a->name[0]+1); }
static OSErr info(const ProcessSerialNumber *psn,ProcessInfoRec *p,FSSpec *spec)
{
    memset(p,0,sizeof(*p));memset(spec,0,sizeof(*spec));
    p->processInfoLength=sizeof(*p);p->processAppSpec=spec;
    return GetProcessInformation(psn,p);
}
static int permitted(const ProcessSerialNumber *psn,const ProcessInfoRec *p)
{
    ProcessSerialNumber current;
    return !GetCurrentProcess(&current) && !same_psn(psn,&current) &&
        !(psn->highLongOfPSN==0 && (psn->lowLongOfPSN==kSystemProcess || psn->lowLongOfPSN==kNoProcess)) &&
        p->processType=='APPL' && p->processSignature!='MACS' &&
        same_psn(&p->processLauncher,&current);
}
void application_tracking_begin(void)
{
    reclaim=count=0;slot=-1;cursor.highLongOfPSN=0;cursor.lowLongOfPSN=kNoProcess;
    tracking_reason=GetCurrentProcess(&self) ? "CURRENT_PROCESS_UNAVAILABLE" : NULL;
    if(!ae_dispatch_available())tracking_reason="ANSWER_DISPATCH_UNAVAILABLE_OR_EXHAUSTED";
}
void application_tracking_expire(void)
{ tracking_reason="PROCESS_SNAPSHOT_DEADLINE"; }
int application_tracking_step(void)
{
    OSErr e;ProcessInfoRec p;FSSpec spec;
    if(reclaim<OWNERS) {
        int i=reclaim++;
        if(owned[i].id[0]) {
            e=info(&owned[i].psn,&p,&spec);
            if(e==procNotFound || (!e && (!same_spec(&spec,&owned[i].artifact) ||
                p.processLaunchDate!=owned[i].launch_date || !permitted(&owned[i].psn,&p))))owned[i].id[0]=0;
        }
        if(!owned[i].id[0] && slot<0)slot=i;
        return 2;
    }
    if(tracking_reason)return 0;
    if(slot<0) { tracking_reason="OWNERSHIP_FULL";return 0; }
    e=GetNextProcess(&cursor);
    if(e==procNotFound)return 0;
    if(e) {tracking_reason="PROCESS_SNAPSHOT_FAILED";return 0;}
    if(count==SNAPSHOT) {tracking_reason="PROCESS_SNAPSHOT_FULL";return 0;}
    before[count++]=cursor;return 2;
}
const char *application_original_run(const ProcessSerialNumber *psn)
{
    int i;for(i=0;i<OWNERS;i++)if(owned[i].id[0] && same_psn(psn,&owned[i].psn))return owned[i].id;
    return "";
}
const char *application_tracking_reason(const ProcessSerialNumber *psn,const ProcessInfoRec *p)
{
    int i;
    if(tracking_reason)return tracking_reason;
    for(i=0;i<count;i++)if(same_psn(psn,&before[i]))return "PROCESS_PREEXISTED";
    if(application_original_run(psn)[0])return "PROCESS_ALREADY_OWNED";
    if(!permitted(psn,p) || !same_psn(&p->processLauncher,&self))return "LAUNCHER_OR_PROCESS_UNVERIFIED";
    return NULL;
}
void application_tracking_commit(const char *id,const ProcessSerialNumber *psn,const FSSpec *spec,const ProcessInfoRec *p)
{
    if(slot<0 || application_tracking_reason(psn,p))return;
    strcpy(owned[slot].id,id);owned[slot].psn=*psn;owned[slot].artifact=*spec;
    owned[slot].launch_date=p->processLaunchDate;owned[slot].attempted=0;
}
static void quit_answer(const AppleEvent *event)
{
    DescType type;Size got;OSErr e;
    reply_received=1;reply_error=0;
    e=AEGetParamPtr(event,keyErrorNumber,typeSInt32,&type,&reply_error,sizeof(reply_error),&got);
    /* A normal empty answer means accepted, not observed exit. */
    malformed=e!=errAEDescNotFound && (e || type!=typeSInt32 || got!=sizeof(reply_error));
}
static int finish(char *out,size_t cap,const char *status,const char *code,int32_t error,int remove)
{
    int uncertain=!strcmp(status,"uncertain");
    snprintf(out,cap,"{\"status\":\"%s\",\"code\":\"%s\",\"run_id\":\"%s\",\"native_error\":%ld}",status,code,
        owner>=0 ? owned[owner].id : "",(long)error);
    if(quit_journal && quit_journal(quit_context,"quit_observed",out)) {
        snprintf(out,cap,"{\"status\":\"uncertain\",\"code\":\"QUIT_TERMINAL_JOURNAL_FAILED\",\"native_error\":%ld}",(long)error);
        uncertain=1;
    }
    if(sent)ae_forget(return_id);
    if(remove && owner>=0)owned[owner].id[0]=0;
    quitting=0;return uncertain;
}
int quit_application_begin(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *ctx,uint32_t now)
{
    JsonToken t[8];char key[32],id[32];int i;(void)now;
    if(quitting) {snprintf(out,cap,"{\"status\":\"error\",\"code\":\"QUIT_BUSY\"}");return 1;}
    owner=-1;quit_journal=NULL;sent=reply_received=malformed=0;
    if(json_parse(call->arguments,strlen(call->arguments),t,8)!=3 || t[0].type!=JSON_OBJECT ||
        json_string(call->arguments,t,1,key,sizeof(key))<0 || strcmp(key,"run_id") ||
        json_string(call->arguments,t,2,id,sizeof(id))<0)
        return finish(out,cap,"error","QUIT_ARGUMENTS",0,0);
    for(i=0;i<OWNERS;i++)if(owned[i].id[0] && !strcmp(id,owned[i].id)) {owner=i;break;}
    if(owner<0)return finish(out,cap,"error","RUN_NOT_OWNED",0,0);
    if(!journal)return finish(out,cap,"error","JOURNAL_REQUIRED",0,0);
    quit_journal=journal;quit_context=ctx;quitting=1;return 2;
}
int quit_application_step(char *out,size_t cap,uint32_t now,int stop)
{
    OSErr e;ProcessInfoRec p;FSSpec spec;int attempted;
    if(!quitting) {snprintf(out,cap,"{\"status\":\"error\",\"code\":\"NO_ACTIVE_QUIT\"}");return 1;}
    if(stop)return finish(out,cap,sent ? "uncertain" : "error",sent ? "QUIT_STOPPED_AFTER_SEND" : "QUIT_STOPPED_BEFORE_SEND",0,0);
    e=info(&owned[owner].psn,&p,&spec);
    if(e==procNotFound)return finish(out,cap,"ok",sent ? "QUIT_OBSERVED" : "ALREADY_EXITED",0,1);
    if(e)return finish(out,cap,"uncertain","QUIT_OBSERVATION_FAILED",e,0);
    if(!same_spec(&spec,&owned[owner].artifact) || p.processLaunchDate!=owned[owner].launch_date || !permitted(&owned[owner].psn,&p))
        return finish(out,cap,sent ? "uncertain" : "error","PROCESS_IDENTITY_CHANGED",0,1);
    if(sent) {
        if(reply_received && malformed)return finish(out,cap,"uncertain","QUIT_MALFORMED_REPLY",0,0);
        if(reply_received && reply_error)return finish(out,cap,"error","QUIT_REFUSED",reply_error,0);
        if((uint32_t)(now-submitted)>=30UL*60UL)return finish(out,cap,"uncertain","QUIT_TIMEOUT",0,0);
        return 2;
    }
    if(owned[owner].attempted)return finish(out,cap,"error","QUIT_ALREADY_ATTEMPTED",0,0);
    e=ae_reserve(&owned[owner].psn,quit_answer,&return_id);
    if(e)return finish(out,cap,"error","QUIT_DISPATCH_UNAVAILABLE_OR_EXHAUSTED",e,0);
    snprintf(out,cap,"{\"status\":\"pending\",\"run_id\":\"%s\",\"process\":{\"high\":%lu,\"low\":%lu},\"return_id\":%d}",
        owned[owner].id,owned[owner].psn.highLongOfPSN,owned[owner].psn.lowLongOfPSN,return_id);
    if(quit_journal(quit_context,"quit_intent",out)) {
        ae_forget(return_id);quit_journal=NULL;
        return finish(out,cap,"error","QUIT_INTENT_JOURNAL_FAILED",0,0);
    }
    e=ae_send(&owned[owner].psn,kCoreEventClass,kAEQuitApplication,NULL,return_id,&attempted);
    if(attempted) {owned[owner].attempted=1;sent=1;submitted=now;}
    if(e) {
        ae_forget(return_id);
        return finish(out,cap,attempted ? "uncertain" : "error",attempted ? "QUIT_SEND_UNCERTAIN" : "QUIT_EVENT_CONSTRUCTION_FAILED",e,0);
    }
    if(quit_journal(quit_context,"quit_submitted",out))return finish(out,cap,"uncertain","QUIT_SUBMISSION_JOURNAL_FAILED",0,0);
    return 2;
}
