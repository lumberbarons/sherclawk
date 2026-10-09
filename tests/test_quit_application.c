/* Real launch verification, ownership, Quit and shared answer dispatcher. */
#define TEST_SELFBUILD 1
#define TEST_REAL_TOOLSERVER 1
#include "test_build_project.c"
#undef main
#include "application_process.h"
#include <stdarg.h>
OSErr PBHGetVInfoSync(HParamBlockRec *p) {(void)p;return fnfErr;}
OSErr PBDTGetPath(DTPBRec *p) {(void)p;return fnfErr;}
OSErr PBDTGetAPPLSync(DTPBRec *p) {(void)p;return fnfErr;}
static void log_message(const char *s,...) {(void)s;}

static char runargs[128],handle[32];
static const char *journal_fault;
static int quit_records;
static int quit_log(void *ctx,const char *event,const char *json)
{
    JsonToken t[128];(void)ctx;assert(json_parse(json,strlen(json),t,128)>0);
    quit_records++;
    return journal_fault && !strcmp(event,journal_fault) ? -1 : 0;
}
static void set_self(void)
{process_count=1;process_self=0;processes[0].hi=0;processes[0].lo=7;}
static int launch_again(void)
{
    int r,turns=0;
    strcpy(call.arguments,runargs);
    assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1000)==2);
    do {r=run_application_step(result,sizeof(result),1001,0);turns++;assert(turns<400);}while(r==2);
    field("run_id",handle,sizeof(handle));return r;
}
static void fresh(void)
{
    int w,q,b,d,n,r;
    process_absent=1;application_tracking_begin();while(application_tracking_step()==2){}
    process_absent=process_error=identity_changed=launch_error=run_journal_error=0;
    /* Quit tests seal an artifact directly; build executor behavior belongs to
     * the native build suites, not this real Apple-event transport fixture. */
    reset();
    w=add(10,"Worker01",1);q=add(files[w].id,"buildjobs",1);
    b=add(files[q].id,"build-00000001-0001",1);d=add(files[b].id,"build",1);
    n=add(files[d].id,"native",1);n=add(files[n].id,"sample",0);
    strcpy(files[n].bytes,"Joy!peffpwpc............................");
    files[n].size=(long)strlen(files[n].bytes);files[n].resource=100;files[n].info.fdType='APPL';
    r=application_authorize_begin("build-00000001-0001","sample",build_journal,NULL,1);
    while(r==2)r=run_application_step(result,sizeof(result),2,0);
    assert(!r);
    strcpy(runargs,"{\"build_id\":\"build-00000001-0001\"}");
    set_self();launch_psn=42;process_signature='SHTP';launcher_override=0;process_enumeration_error=0;journal_fault=NULL;ae_fail_stage=0;
    assert(!launch_again() && strstr(result,"\"quit_supported\":true"));
}
static int quit_begin(void)
{
    snprintf(call.arguments,sizeof(call.arguments),"{\"run_id\":\"%s\"}",handle);
    return quit_application_begin(&call,result,sizeof(result),quit_log,NULL,2000);
}
static void submit(void)
{
    int n=ae_sends,disposals=ae_disposals;
    assert(quit_begin()==2);assert(quit_application_step(result,sizeof(result),2001,0)==2);
    assert(ae_sends==n+1 && !ae_live && ae_disposals==disposals+3);
    assert(ae_target.lowLongOfPSN==42 && ae_last_event==kAEQuitApplication && ae_last_mode==(kAEQueueReply | kAENeverInteract));
}
static void discard_answer(const AppleEvent *event) {(void)event;}
static void unchanged_sends(int n) {assert(ae_sends==n && !ae_live);}
int main(void)
{
    int n,r,i;short old_id,id;ProcessSerialNumber target={0,42};
    ae_fail_stage=1;assert(ae_dispatch_init()==memFullErr && !ae_dispatch_ready());
    ae_fail_stage=2;assert(ae_dispatch_init()==ioErr && !ae_dispatch_ready());
    ae_fail_stage=0;assert(!toolserver_init(log_message));
    n=ae_sends;
    strcpy(call.arguments,"{\"run_id\":\"arbitrary\"}");assert(!quit_application_begin(&call,result,sizeof(result),quit_log,NULL,1));
    assert(strstr(result,"RUN_NOT_OWNED"));unchanged_sends(n);
    strcpy(call.arguments,"{\"run_id\":\"arbitrary\",\"process\":42}");assert(!quit_application_begin(&call,result,sizeof(result),quit_log,NULL,1));assert(strstr(result,"QUIT_ARGUMENTS"));
    fresh();n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,1));unchanged_sends(n);
    submit();old_id=ae_last_id;
    for(i=1;i<=8;i*=2) {
        ae_attribute_fault=i;ae_deliver(old_id,42,0,1,-128);
        assert(quit_application_step(result,sizeof(result),2002,0)==2);
    }
    ae_attribute_fault=0;
    ae_deliver(old_id,99,0,1,-128);assert(quit_application_step(result,sizeof(result),2002,0)==2);
    ae_deliver((short)(old_id-1),42,0,1,-128);assert(quit_application_step(result,sizeof(result),2003,0)==2);
    ae_deliver(old_id,42,0,0,0);assert(quit_application_step(result,sizeof(result),2004,0)==2);
    process_absent=1;assert(!quit_application_step(result,sizeof(result),2005,0) && strstr(result,"QUIT_OBSERVED"));
    n=ae_sends;assert(!quit_begin() && strstr(result,"RUN_NOT_OWNED"));unchanged_sends(n);
    fresh();process_absent=1;n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,0) && strstr(result,"ALREADY_EXITED"));unchanged_sends(n);
    fresh();process_error=1;n=ae_sends;assert(quit_begin()==2);assert(quit_application_step(result,sizeof(result),2001,0)==1 && strstr(result,"OBSERVATION_FAILED"));unchanged_sends(n);
    fresh();identity_changed=1;n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,0) && strstr(result,"IDENTITY_CHANGED"));unchanged_sends(n);
    fresh();submit();ae_deliver(ae_last_id,42,0,1,-128);assert(!quit_application_step(result,sizeof(result),2002,0) && strstr(result,"QUIT_REFUSED") && strstr(result,"-128"));
    n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2003,0) && strstr(result,"ALREADY_ATTEMPTED"));unchanged_sends(n);
    fresh();submit();assert(quit_application_step(result,sizeof(result),3801,0)==1 && strstr(result,"QUIT_TIMEOUT"));
    fresh();submit();old_id=ae_last_id;assert(quit_application_step(result,sizeof(result),2002,1)==1 && strstr(result,"AFTER_SEND"));
    fresh();submit();ae_deliver(old_id,42,0,1,-128);assert(quit_application_step(result,sizeof(result),2002,0)==2);ae_deliver(ae_last_id,42,1,1,0);assert(quit_application_step(result,sizeof(result),2003,0)==1 && strstr(result,"MALFORMED"));
    fresh();submit();process_error=1;assert(quit_application_step(result,sizeof(result),2002,0)==1 && strstr(result,"OBSERVATION_FAILED"));
    fresh();submit();identity_changed=1;assert(quit_application_step(result,sizeof(result),2002,0)==1 && strstr(result,"IDENTITY_CHANGED"));
    fresh();journal_fault="quit_intent";n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,0) && strstr(result,"INTENT_JOURNAL_FAILED"));unchanged_sends(n);
    journal_fault=NULL;submit();process_absent=1;journal_fault="quit_observed";assert(quit_application_step(result,sizeof(result),2002,0)==1 && strstr(result,"TERMINAL_JOURNAL_FAILED"));
    fresh();journal_fault="quit_submitted";n=ae_sends;assert(quit_begin()==2);assert(quit_application_step(result,sizeof(result),2001,0)==1 && ae_sends==n+1 && strstr(result,"SUBMISSION_JOURNAL_FAILED"));
    fresh();ae_fail_stage=6;n=ae_sends;assert(quit_begin()==2);assert(quit_application_step(result,sizeof(result),2001,0)==1 && ae_sends==n+1 && strstr(result,"SEND_UNCERTAIN"));assert(!ae_live);
    ae_fail_stage=0;n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2002,0) && strstr(result,"ALREADY_ATTEMPTED"));unchanged_sends(n);
    for(i=3;i<=4;i++) {
        fresh();ae_fail_stage=i;n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,0) && strstr(result,"CONSTRUCTION_FAILED"));unchanged_sends(n);
        ae_fail_stage=0;submit();assert(quit_application_step(result,sizeof(result),2002,1)==1);
    }
    /* A returned pre-existing PSN retains only its original handle. */
    fresh();{ char original[32];strcpy(original,handle);process_count=2;processes[1].hi=0;processes[1].lo=42;
        assert(!launch_again() && strstr(result,"PROCESS_PREEXISTED") && strstr(result,original));
        n=ae_sends;assert(!quit_begin() && strstr(result,"RUN_NOT_OWNED"));unchanged_sends(n);
        strcpy(handle,original);assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,1)); }
    fresh();process_absent=1;application_tracking_begin();while(application_tracking_step()==2){}process_absent=0;
    process_count=2;processes[1].hi=0;processes[1].lo=42;
    assert(!launch_again() && strstr(result,"PROCESS_PREEXISTED") && strstr(result,"\"original_run_id\":\"\""));
    n=ae_sends;assert(!quit_begin());unchanged_sends(n);
    /* No registry recovery from run_observed journal failure or launch uncertainty. */
    fresh();process_absent=1;application_tracking_begin();while(application_tracking_step()==2){}process_absent=0;
    run_journal_error=2;assert(launch_again()==1);n=ae_sends;assert(!quit_begin() && strstr(result,"RUN_NOT_OWNED"));unchanged_sends(n);run_journal_error=0;
    launch_error=1;assert(launch_again()==1);assert(!quit_begin());unchanged_sends(n);launch_error=0;
    /* ToolServer and Quit share the actual handler without reply crossover. */
    fresh();submit();old_id=ae_last_id;
    process_count=2;processes[1].hi=0;processes[1].lo=43;
    assert(!toolserver_send("Retro68:","Echo test",1));id=ae_last_id;
    assert(id!=old_id && ae_last_event==kAEDoScript);
    assert(!strcmp(ae_script,"Set Exit 0\rDirectory \"Retro68:\"\rEcho test < Dev:Null\rSet CommandStatus {Status}\rDirectory \"{MPW}\"\rExit {CommandStatus}\r"));
    ae_deliver(id,42,0,1,-128);assert(quit_application_step(result,sizeof(result),2002,0)==2);
    {ToolServerReply reply;assert(!toolserver_poll(2,0,&reply));
     ae_deliver(old_id,43,0,1,-128);assert(!toolserver_poll(3,0,&reply));
     ae_deliver(id,43,0,1,0);assert(toolserver_poll(4,0,&reply)==1 && !reply.malformed && !reply.status);}
    assert(quit_application_step(result,sizeof(result),2003,1)==1);
    /* Finder, self, system and a different launcher fail closed. */
    for(i=0;i<4;i++) {
        fresh();n=ae_sends;
        if(i==0)process_signature='MACS';
        if(i==1)launcher_override=99;
        if(i==2)launch_psn=7;
        if(i==3)launch_psn=kSystemProcess;
        assert(!launch_again() && strstr(result,"\"quit_supported\":false"));
        assert(!quit_begin() && strstr(result,"RUN_NOT_OWNED"));unchanged_sends(n);
    }
    /* Generated artifacts use ShCk too; only the harness's exact PSN is self. */
    fresh();process_signature='ShCk';launch_psn=44;
    assert(!launch_again() && strstr(result,"\"quit_supported\":true"));
    n=ae_sends;assert(quit_begin()==2);assert(quit_application_step(result,sizeof(result),2001,0)==2);
    assert(ae_sends==n+1 && ae_target.lowLongOfPSN==44);
    assert(quit_application_step(result,sizeof(result),2002,1)==1);
    /* Unavailable/incomplete snapshots still launch, without new permission. */
    fresh();process_enumeration_error=1;assert(!launch_again() && strstr(result,"SNAPSHOT_FAILED"));n=ae_sends;assert(!quit_begin());unchanged_sends(n);process_enumeration_error=0;
    fresh();for(i=1;i<257;i++){processes[i].hi=0;processes[i].lo=(unsigned long)i+100;}
    process_count=257;assert(!launch_again() && strstr(result,"SNAPSHOT_FULL"));n=ae_sends;assert(!quit_begin());unchanged_sends(n);
    fresh();strcpy(call.arguments,runargs);
    assert(run_application_begin(&call,result,sizeof(result),build_journal,NULL,1000)==2);
    assert(run_application_step(result,sizeof(result),1001,0)==2);
    assert(run_application_step(result,sizeof(result),1002,0)==2);
    assert(!run_application_step(result,sizeof(result),4600,0) && strstr(result,"SNAPSHOT_DEADLINE"));field("run_id",handle,sizeof(handle));
    n=ae_sends;assert(!quit_begin());unchanged_sends(n);
    /* Fill 32 ownership slots using the same admission interlock. */
    fresh();
    for(i=1;i<32;i++) {
        ProcessInfoRec p={0};char run[32];target.lowLongOfPSN=(unsigned long)i+100;
        application_tracking_begin();while(application_tracking_step()==2){}
        p.processType='APPL';p.processSignature='SHTP';p.processLaunchDate=10;GetCurrentProcess(&p.processLauncher);
        assert(!application_tracking_reason(&target,&p));snprintf(run,sizeof(run),"capacity-%d",i);
        application_tracking_commit(run,&target,&launched,&p);
    }
    assert(!launch_again() && strstr(result,"OWNERSHIP_FULL"));n=ae_sends;assert(!quit_begin());unchanged_sends(n);
    /* Reclamation is cooperative; absent entries free capacity. */
    process_absent=1;application_tracking_begin();r=application_tracking_step();assert(r==2);
    for(i=1;i<32;i++)assert(application_tracking_step()==2);
    process_absent=0;assert(application_tracking_step()==2);assert(!application_tracking_step());
    ae_dispatch_close();assert(!launch_again() && strstr(result,"DISPATCH_UNAVAILABLE"));n=ae_sends;assert(!quit_begin());unchanged_sends(n);
    /* Lifetime IDs never recycle, including close/reinitialization. */
    assert(!ae_dispatch_init());fresh();
    ae_dispatch_close();assert(!ae_dispatch_init());
    assert(ae_reserve(&target,NULL,&id));
    for(i=0;i<32767;i++) {r=ae_reserve(&target,discard_answer, &id);if(r)break;ae_forget(id);}
    assert(r && i<32767);
    n=ae_sends;assert(quit_begin()==2);assert(!quit_application_step(result,sizeof(result),2001,0) && strstr(result,"EXHAUSTED"));unchanged_sends(n);
    assert(!launch_again() && strstr(result,"DISPATCH_UNAVAILABLE_OR_EXHAUSTED") && strstr(result,"\"quit_supported\":false"));
    assert(quit_records>0 && !ae_live);puts("PASS owned application Quit, journal and process interlocks, asynchronous replies, Stop and unique return IDs");return 0;
}
