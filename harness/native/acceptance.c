/* Explicitly registered harness self-test fixture, never a model tool.
 * A closed workspace:scenario file selects the failure being injected. */
#include "diagnostic.h"
#include "files.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <stdio.h>
#include <string.h>
static HReport report;
static char buffer[H_REPORT_CAP];
int main(void)
{
    char path[256], mode[40];
    int n, i;
    EventRecord event;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    if (harness_begin("HarnessAcceptance") != 1) return 1;
    snprintf(path,sizeof(path),"%sscenario",harness_workspace());
    n=h_read(path,mode,sizeof(mode)); if (n < 0) return 1;
    if (!strcmp(mode,"early")) return 1;
    if (!strcmp(mode,"badconfig")) {
        if (!h_private_path("Harness active",path,sizeof(path)) || remove(path) ||
            !h_publish(path,"BROKEN\r",7)) return 1;
        return harness_begin("HarnessAcceptance") < 0 ? 1 : 0;
    }
    if (!strcmp(mode,"target")) return harness_begin("WrongDiagnostic") < 0 ? 1 : 0;
    if (!strcmp(mode,"partial") || !strcmp(mode,"identity")) {
        if (!h_private_path("Harness active",path,sizeof(path))) return 1;
        n=h_read(path,buffer,H_REQUEST_CAP);
        if (n < 0 || !h_request_parse(buffer,(size_t)n,&report.request)) return 1;
        h_mailpath(path,sizeof(path),&report.request,"report");
        if (!strcmp(mode,"identity")) report.request.generation[0]=report.request.generation[0]=='a' ? 'b' : 'a';
        strcpy(report.outcome,"passed"); h_report_check(&report,1,"fixture pass");
        n=h_report_format(&report,buffer,sizeof(buffer));
        if (!strcmp(mode,"partial")) n/=2;
        if (n > 0) h_publish(path,buffer,(size_t)n);
        return 1;
    }
    if (!strcmp(mode,"wait")) {
        for (i=0; i<180; i++) {
            WaitNextEvent(everyEvent,&event,60,NULL);
            harness_check(1,"cooperative wait boundary");
        }
    }
    harness_check(strcmp(mode,"fail") != 0,"deliberate assertion");
    return harness_finish(!strcmp(mode,"fail"));
}
