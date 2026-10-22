/* One-shot provisioning helper: fork-aware install on the private boot disk.
 * Build once per slot. The original image is never opened by this helper. */
#include "files.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Folders.h>
#include <stdio.h>
#include <string.h>
#ifndef HARNESS_SLOT
#define HARNESS_SLOT 1
#endif
static int copy_fork(const FSSpec *source, const FSSpec *dest, int resource)
{
    short in, out;
    char data[8192];
    long n;
    OSErr err, closed;
    EventRecord event;
    err=resource ? FSpOpenRF(source,fsRdPerm,&in) : FSpOpenDF(source,fsRdPerm,&in);
    if (err) return 0;
    err=resource ? FSpOpenRF(dest,fsWrPerm,&out) : FSpOpenDF(dest,fsWrPerm,&out);
    if (err) { FSClose(in); return 0; }
    do {
        n=sizeof(data); err=FSRead(in,&n,data);
        if (err && err != eofErr) break;
        if (n) {
            long written=n;
            if (FSWrite(out,&written,data) || written != n) { err=ioErr; break; }
        }
        WaitNextEvent(everyEvent,&event,0,NULL);
    } while (err != eofErr);
    closed=FSClose(in);
    if (FSClose(out)) closed=ioErr;
    return err == eofErr && !closed;
}
int main(void)
{
    short vref;
    long dir;
    FSSpec source, dest;
    FInfo info;
    Str255 name="\pHarnessController";
    char private[256], identity[16], logpath[100];
    FILE *log;
    int ok;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    snprintf(logpath,sizeof(logpath),"Retro68:HarnessInstall%d.log",HARNESS_SLOT);
    log=fopen(logpath,"w"); if (!log) return 1;
    ok=!h_spec("Retro68:HarnessController",&source) && !FSpGetFInfo(&source,&info) &&
        !FindFolder(kOnSystemDisk,kStartupFolderType,kCreateFolder,&vref,&dir) &&
        FSMakeFSSpec(vref,dir,name,&dest) == fnfErr &&
        !FSpCreate(&dest,info.fdCreator,info.fdType,0) && copy_fork(&source,&dest,0) &&
        copy_fork(&source,&dest,1) && !FSpSetFInfo(&dest,&info) && !FlushVol(NULL,vref);
    fprintf(log,"controller installed=%d\n",ok); fflush(log);
    snprintf(identity,sizeof(identity),"slot%d\r",HARNESS_SLOT);
    ok=ok && h_private_path("Harness slot",private,sizeof(private)) && h_publish(private,identity,strlen(identity));
    fprintf(log,"RESULT installed=%d slot=%d\n",ok,HARNESS_SLOT); fclose(log); FlushVol(NULL,0);
    return ok ? 0 : 1;
}
