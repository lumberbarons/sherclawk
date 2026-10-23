#include "harness/native/contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char request[] =
    "HARNESS/1\rslot1\r11111111111111111111111111111111\r"
    "22222222222222222222222222222222\r33333333333333333333333333333333\r"
    "SherclawkWriteCheck\rRetro68:Harness:r:SherclawkWriteCheck\r"
    "Retro68:Harness:r:workspace:\r4000000000\rEND\r";
int main(void)
{
    HRequest r, other;
    HReport report;
    char buf[H_REPORT_CAP], name[H_NAME_CAP];
    unsigned int i;
    assert(h_request_parse(request, strlen(request), &r));
    assert(!h_request_parse(request, strlen(request)-1, &other));
    strcpy(buf, request); strcat(buf, "junk\r");
    assert(!h_request_parse(buf, strlen(buf), &other));
    assert(h_request_format(&r, buf, sizeof(buf)) == (int)strlen(request));
    assert(!strcmp(buf, request));
    other = r; other.boot[0] = 'a'; assert(!h_request_equal(&r, &other));
    memset(&report, 0, sizeof(report)); report.request = r;
    strcpy(report.outcome, "passed");
    assert(h_report_format(&report, buf, sizeof(buf)) < 0); /* zero checks cannot pass */
    assert(h_report_check(&report, 1, "created file"));
    assert(h_report_format(&report, buf, sizeof(buf)) > 0);
    assert(h_report_parse(buf, strlen(buf), &r, &report));
    assert(!h_report_parse(buf, strlen(buf), &other, &report));
    assert(!h_report_parse(buf, strlen(buf)-1, &r, &report));
    assert(h_report_check(&report, 0, "deliberate failure"));
    assert(h_report_format(&report, buf, sizeof(buf)) < 0); /* false pass */
    strcpy(report.outcome, "failed");
    assert(h_report_format(&report, buf, sizeof(buf)) > 0);
    assert(h_report_parse(buf, strlen(buf), &r, &report));
    assert(report.total == 2 && report.failures == 1);
    strcpy(report.request.workspace, "Retro68::escape:");
    assert(h_report_format(&report, buf, sizeof(buf)) < 0);
    strcpy(buf,request); memcpy(strstr(buf,"4000000000"),"9999999999",10);
    assert(!h_request_parse(buf,strlen(buf),&other));
    memset(&report,0,sizeof(report)); report.request=r; strcpy(report.outcome,"passed");
    memset(name,'x',sizeof(name)-1); name[sizeof(name)-1]=0;
    assert(!h_report_check(&report,1,"embedded\rrecord"));
    for(i=0;i<H_CHECK_MAX;i++) assert(h_report_check(&report,1,name));
    assert(!h_report_check(&report,1,"overflow"));
    assert(h_report_format(&report,buf,sizeof(buf)) > 0);
    assert(h_report_parse(buf,strlen(buf),&r,&report) && report.total==H_CHECK_MAX);
    puts("harness contract: passed");
    return 0;
}
