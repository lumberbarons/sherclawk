/* Guest check for fs_catalog's const contract: PBGetCatInfoSync by directory
 * ID and name must not write through ioNamePtr. The spec's whole name buffer,
 * including the bytes past the Pascal length, is poisoned and compared after
 * the call, for a file and a directory, with the canonical name and with a
 * case-different one. A case-insensitive volume would match the latter and is
 * where a write-back of the on-disk name would show; the AFP workspace answers
 * fnfErr instead, so that lookup is allowed to miss while the buffer must
 * still come back untouched. */
#include "tools.h"
#include "fsutil.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Script.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
static FILE *logfile;
static int failures;
static void check(int condition, const char *name)
{
    fprintf(logfile,"%s %s\n",condition ? "PASS" : "FAIL",name);
    if (!condition) failures++;
    fflush(logfile);
}
/* Poison every byte of the name buffer past the length, then call. */
static void probe(const FSSpec *src, int flip_case, const char *label)
{
    FSSpec spec = *src, orig;
    CInfoPBRec pb;
    unsigned i;
    OSErr err;
    memset(spec.name + 1 + spec.name[0], 0xA5, sizeof(spec.name) - 1 - spec.name[0]);
    if (flip_case)
        for (i = 1; i <= spec.name[0]; i++) {
            unsigned char c = spec.name[i];
            if (isupper(c)) spec.name[i] = (unsigned char)tolower(c);
            else if (islower(c)) spec.name[i] = (unsigned char)toupper(c);
        }
    orig = spec;
    err = fs_catalog(&spec, &pb);
    fprintf(logfile,"NOTE %s err=%d name=%.*s\n",label,(int)err,(int)spec.name[0],spec.name + 1);
    if (flip_case) check(!err || err == fnfErr,label);
    else check(!err,label);
    check(!memcmp(&spec,&orig,sizeof(spec)),"  spec (name buffer included) untouched");
    check(pb.hFileInfo.ioNamePtr == spec.name,"  ioNamePtr still points at the caller's buffer");
}
int main(void)
{
    FSSpec root, file;
    InitGraf(&qd.thePort);InitFonts();InitWindows();InitMenus();TEInit();InitDialogs(NULL);InitCursor();
    logfile=fopen(SHERCLAWK_WORKSPACE "SherclawkCatalogCheck.log","w");if(!logfile)return 1;
    check(!tools_workspace_root(&root),"workspace root spec");
    check(!tools_resolve("SherclawkCatalogCheck.log",&file),"log file spec");
    probe(&file,0,"file, canonical name");
    probe(&file,1,"file, case-flipped name");
    probe(&root,0,"directory, canonical name");
    probe(&root,1,"directory, case-flipped name");
    fprintf(logfile,"RESULT failures=%d\n",failures);fflush(logfile);FlushVol(NULL,0);fclose(logfile);
    return failures ? 1 : 0;
}
