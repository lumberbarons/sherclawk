#include "run.h"
#include <string.h>
void h_run_init(HRun *r,const char *slot,const char *boot)
{
    memset(r,0,sizeof(*r)); strcpy(r->slot,slot); strcpy(r->boot,boot);
}
static void terminal(HRun *r,const char *outcome)
{
    r->report.total=0; r->report.failures=0;
    strcpy(r->report.outcome,outcome); r->state=H_PUBLISH;
}
void h_run_step(HRun *r,const HRunHost *h)
{
    HRequest expected;
    unsigned long now;
    int result, present;
    switch (r->state) {
    case H_IDLE:
        if (!h->request(h->ctx,&r->report.request) ||
            strcmp(r->report.request.slot,r->slot) || strcmp(r->report.request.boot,r->boot)) return;
        result=h->claim(h->ctx,&r->report.request);
        if (result < 0) { r->state=H_QUARANTINED; return; }
        if (!result) return;
        now=h->now(h->ctx);
        if (r->report.request.deadline <= now || r->report.request.deadline-now > 600) terminal(r,"harness_error");
        else r->state=H_CONFIGURE;
        return;
    case H_CONFIGURE:
        if (!h->configure(h->ctx,&r->report.request)) terminal(r,"harness_error");
        else r->state=H_LAUNCH;
        return;
    case H_LAUNCH:
        if (h->now(h->ctx) >= r->report.request.deadline) { terminal(r,"unknown"); return; }
        if (h->stopped(h->ctx,&r->report.request)) { terminal(r,"stopped"); return; }
        if (!h->launch(h->ctx,&r->report.request)) terminal(r,"unknown");
        else r->state=H_OBSERVE;
        return;
    case H_OBSERVE:
        if (h->now(h->ctx) >= r->report.request.deadline) { terminal(r,"unknown"); return; }
        expected=r->report.request;
        result=h->report(h->ctx,&expected,&r->report);
        r->report.request=expected;
        if (result < 0) { terminal(r,"harness_error"); return; }
        present=h->present(h->ctx);
        if (result == 1) {
            if (present == 0 || (strcmp(r->report.outcome,"passed") && strcmp(r->report.outcome,"failed"))) r->state=H_PUBLISH;
            else if (h->stopped(h->ctx,&expected)) terminal(r,"stopped");
            else if (present < 0) terminal(r,"unknown");
        } else if (h->stopped(h->ctx,&expected)) terminal(r,"stopped");
        else if (present != 1) terminal(r,"unknown");
        return;
    case H_PUBLISH:
        result=h->publish(h->ctx,&r->report);
        if (result && (!strcmp(r->report.outcome,"passed") || !strcmp(r->report.outcome,"failed"))) r->state=H_CLEANUP;
        else r->state=H_QUARANTINED;
        return;
    case H_CLEANUP:
        r->state=h->cleanup(h->ctx) ? H_IDLE : H_QUARANTINED;
        return;
    default: return;
    }
}
