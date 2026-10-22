#include "harness/native/run.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    HRequest request;
    unsigned long now;
    int available, claimed, configured, launched, present, report, stop, published, cleanup;
    int fail_config, fail_launch, fail_publish, fail_cleanup;
    char result[16];
} Fake;
static unsigned long now(void *p) { return ((Fake *)p)->now; }
static int request(void *p,HRequest *r) { Fake *f=p; *r=f->request; return f->available; }
static int claim(void *p,const HRequest *r) { Fake *f=p; (void)r; if(f->claimed)return 0; f->claimed=1; return 1; }
static int configure(void *p,const HRequest *r) { Fake *f=p; (void)r; f->configured++; return !f->fail_config; }
static int launch(void *p,const HRequest *r) { Fake *f=p; (void)r; f->launched++; return !f->fail_launch; }
static int report(void *p,const HRequest *r,HReport *out) {
    Fake *f=p;
    if(f->report<=0)return f->report;
    memset(out,0,sizeof(*out)); out->request=*r; strcpy(out->outcome,"passed");
    h_report_check(out,f->report!=2,"guest assertion");
    if(f->report==2)strcpy(out->outcome,"failed");
    if(f->report==3)strcpy(out->outcome,"stopped");
    return 1;
}
static int present(void *p) { return ((Fake *)p)->present; }
static int stopped(void *p,const HRequest *r) { (void)r; return ((Fake *)p)->stop; }
static int publish(void *p,const HReport *r) { Fake *f=p; f->published++; strcpy(f->result,r->outcome); return !f->fail_publish; }
static int cleanup(void *p) { Fake *f=p; f->cleanup++; return !f->fail_cleanup; }
static HRunHost host(Fake *f) {
    HRunHost h={f,now,request,claim,configure,launch,report,present,stopped,publish,cleanup}; return h;
}
static void setup(Fake *f,HRun *run) {
    memset(f,0,sizeof(*f));
    strcpy(f->request.slot,"slot1"); memset(f->request.boot,'b',32);
    f->request.deadline=200; f->now=100; f->available=1; f->present=1;
    h_run_init(run,"slot1",f->request.boot);
}
static void steps(HRun *r,HRunHost *h,int n) { while(n--)h_run_step(r,h); }
int main(void)
{
    static HRun run;
    Fake f;
    HRunHost h=host(&f);
    setup(&f,&run); f.request.boot[0]='a'; steps(&run,&h,8);
    assert(!f.claimed && !f.launched); /* stale readiness */
    setup(&f,&run); f.claimed=1; steps(&run,&h,8);
    assert(!f.configured && !f.launched); /* duplicate claim */
    setup(&f,&run); steps(&run,&h,3); assert(f.claimed && f.configured==1 && f.launched==1);
    f.report=1; f.present=0; steps(&run,&h,3);
    assert(!strcmp(f.result,"passed") && f.cleanup==1 && run.state==H_IDLE);
    steps(&run,&h,4); assert(f.launched==1); /* same request cannot launch twice */
    setup(&f,&run); f.fail_config=1; steps(&run,&h,6);
    assert(!strcmp(f.result,"harness_error") && !f.launched && run.state==H_QUARANTINED);
    setup(&f,&run); f.fail_launch=1; steps(&run,&h,6);
    assert(!strcmp(f.result,"unknown") && f.launched==1 && run.state==H_QUARANTINED);
    setup(&f,&run); steps(&run,&h,3); f.present=0; steps(&run,&h,3);
    assert(!strcmp(f.result,"unknown") && !f.cleanup); /* early exit */
    setup(&f,&run); steps(&run,&h,3); f.report=-1; steps(&run,&h,3);
    assert(!strcmp(f.result,"harness_error")); /* partial/wrong identity */
    setup(&f,&run); steps(&run,&h,3); f.now=200; f.report=1; f.present=0; steps(&run,&h,3);
    assert(!strcmp(f.result,"unknown")); /* report after observation deadline */
    setup(&f,&run); steps(&run,&h,3); f.stop=1; steps(&run,&h,3);
    assert(!strcmp(f.result,"stopped") && f.present && !f.cleanup);
    setup(&f,&run); steps(&run,&h,3); f.report=1; f.present=0; f.fail_publish=1; steps(&run,&h,3);
    assert(run.state==H_QUARANTINED && !f.cleanup);
    setup(&f,&run); steps(&run,&h,3); f.report=1; f.present=0; f.fail_cleanup=1; steps(&run,&h,3);
    assert(run.state==H_QUARANTINED);
    setup(&f,&run); steps(&run,&h,3); f.report=2; f.present=0; steps(&run,&h,3);
    assert(!strcmp(f.result,"failed") && run.state==H_IDLE);
    setup(&f,&run); steps(&run,&h,3); f.report=3; steps(&run,&h,3);
    assert(!strcmp(f.result,"stopped") && run.state==H_QUARANTINED && !f.cleanup);
    setup(&f,&run); f.request.deadline=701; steps(&run,&h,4);
    assert(!strcmp(f.result,"harness_error") && !f.launched);
    setup(&f,&run); steps(&run,&h,2); f.stop=1; steps(&run,&h,4);
    assert(!strcmp(f.result,"stopped") && !f.launched);
    puts("harness controller: passed"); return 0;
}
