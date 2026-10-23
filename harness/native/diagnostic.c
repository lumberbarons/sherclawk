#include "diagnostic.h"
#include "files.h"
#include <Events.h>
#include <Processes.h>
#include <stdlib.h>
#include <string.h>
static HReport report;
static char buffer[H_REPORT_CAP], private_config[256];
static int active;
static int publish(const char *outcome)
{
    char path[256];
    int n;
    strcpy(report.outcome,outcome);
    n=h_report_format(&report,buffer,sizeof(buffer));
    h_mailpath(path,sizeof(path),&report.request,"report");
    return n > 0 && h_publish(path,buffer,(size_t)n);
}
int harness_begin(const char *diagnostic)
{
    int n;
    if (!h_private_path("Harness active",private_config,sizeof(private_config))) return -1;
    n=h_read(private_config,buffer,H_REQUEST_CAP);
    if (n == -2) return 0;
    if (n < 0 || !h_request_parse(buffer,(size_t)n,&report.request)) return -1;
    active=1;
    if (strcmp(diagnostic,report.request.diagnostic) ||
        !h_self_matches(report.request.application) || h_now() >= report.request.deadline) {
        publish("harness_error"); return -1;
    }
    return 1;
}
const char *harness_workspace(void) { return active ? report.request.workspace : NULL; }
void harness_check(int passed, const char *name)
{
    char path[256], stop[H_REQUEST_CAP];
    HRequest request;
    EventRecord event;
    int n;
    if (!active) return;
    /* Yield at each bounded assertion boundary. Stop ends observation only;
     * it does not cancel or undo any Toolbox operation already performed. */
    WaitNextEvent(everyEvent,&event,0,NULL);
    h_mailpath(path,sizeof(path),&report.request,"stop");
    n=h_read(path,stop,sizeof(stop));
    if (n >= 0 && h_request_parse(stop,(size_t)n,&request) && h_request_equal(&request,&report.request)) {
        publish("stopped"); exit(1);
    }
    if (h_now() >= report.request.deadline) { publish("unknown"); exit(1); }
    if (!h_report_check(&report,passed,name)) { publish("harness_error"); exit(1); }
}
int harness_finish(int failures)
{
    if (!active) return failures ? 1 : 0;
    /* Executors can fail outside check(): preserve that failure as a named assertion. */
    if (failures && !report.failures) h_report_check(&report,0,"diagnostic execution failed");
    return publish(report.failures ? "failed" : "passed") ? !!report.failures : 1;
}
