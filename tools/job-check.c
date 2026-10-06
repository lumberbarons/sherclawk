/* Real native File Manager producer/poller: services events between bounded
 * steps and retains all fixtures. Run the worker on Worker01/nativejobs separately.
 * Command-period stops observation only; it never cancels or replays a job. */
#include "jobs.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <stdio.h>
#include <string.h>
static FILE *logfile;
static NativeJob job;
static int journal(void *ctx,const char *event,const char *json)
{
    (void)ctx;
    return fprintf(logfile,"{\"event\":\"%s\",\"job\":%s}\n",event,json)<0 ||
        fflush(logfile) || FlushVol(NULL,0) ? -1 : 0;
}
static void pages(void)
{
    if(job.stdout_size) { fputs("STDOUT ",logfile); fwrite(job.stdout_page,1,job.stdout_size,logfile); }
    if(job.stderr_size) { fputs("STDERR ",logfile); fwrite(job.stderr_page,1,job.stderr_size,logfile); }
    fflush(logfile);
}
int main(void)
{
    static const char script[]="cat snapshot.txt || exit 1\necho 'native producer stdout' || exit 1\necho 'native producer stderr' >&2 || exit 1\n";
    static char snapshot[3*JOB_PAGE];
    static const JobInput inputs[]={ {"script",script,sizeof(script)-1}, {"snapshot.txt",snapshot,sizeof(snapshot)} };
    FSSpec queue; Str255 path; char full[256],id[25]; EventRecord event;
    WindowPtr window; Rect bounds; int stop=0,failed;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkJobCheck.log","w"); if(!logfile) return 1;
    snprintf(full,sizeof(full),"%sWorker01:nativejobs:",SHERCLAWK_WORKSPACE);
    path[0]=(unsigned char)strlen(full); memcpy(path+1,full,path[0]);
    if(FSMakeFSSpec(0,0,path,&queue)) { fputs("FAIL queue missing\n",logfile); fclose(logfile); return 1; }
    snprintf(id,sizeof(id),"native-%08lx",(unsigned long)TickCount());
    SetRect(&bounds,70,70,570,180);
    window=NewWindow(NULL,&bounds,(const unsigned char *)"\023Sherclawk job check",true,documentProc,(WindowPtr)-1,false,0);
    if(window) { SetPort(window); MoveTo(15,30); DrawString((const unsigned char *)"\037Polling worker; Cmd-period stops"); }
    memset(snapshot,'x',sizeof(snapshot)); snapshot[sizeof(snapshot)-1]='\r';
    failed=jobs_begin(&job,&queue,id,inputs,2,(uint32_t)TickCount(),120UL*60UL,journal,NULL);
    while(!failed && (job.state==JOB_STAGING || job.state==JOB_WAITING)) {
        if(WaitNextEvent(everyEvent,&event,1,NULL)) {
            if((event.what==keyDown || event.what==autoKey) && (event.modifiers & cmdKey) &&
                ((event.message & charCodeMask)=='.' || (event.message & charCodeMask)=='q')) stop=1;
            if(event.what==updateEvt && window) {
                BeginUpdate(window); SetPort(window); MoveTo(15,30);
                DrawString((const unsigned char *)"\037Polling worker; Cmd-period stops"); EndUpdate(window);
            }
        }
        jobs_step(&job,(uint32_t)TickCount(),stop); pages();
    }
    /* Drain at most 16 additional pages in this diagnostic; production callers
     * retain offsets and can request further bounded pages explicitly. */
    if(job.state==JOB_SUCCEEDED) {
        int i;
        for(i=0;i<16;i++) {
            if(jobs_logs(&job)) { failed=1; break; }
            pages(); if(!job.stdout_size && !job.stderr_size) break;
        }
    }
    failed=failed || job.state!=JOB_SUCCEEDED || job.stdout_offset!=3*JOB_PAGE+23 || job.stderr_offset!=23;
    fprintf(logfile,"RESULT failures=%d id=%s state=%d error=%d stdout=%ld stderr=%ld\n",
        failed,id,job.state,job.error,job.stdout_offset,job.stderr_offset);
    fflush(logfile); FlushVol(NULL,0); fclose(logfile); if(window) DisposeWindow(window);
    return failed ? 1 : 0;
}
