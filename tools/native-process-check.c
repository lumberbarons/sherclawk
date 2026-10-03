/* Native Process Manager evidence: MacRelix /proc lists its own tasks,
 * not classic applications. Run this as a PPC application after tlsrvr.
 * The fixed diagnostic log lives in the isolated Template01 AFP fixture.
 */
#include <Types.h>
#include <Files.h>
#include <Processes.h>
#include <Resources.h>
#include <stdio.h>
#include <string.h>

static void path_for(FSSpec *spec, char *path)
{
    CInfoPBRec pb;
    Str255 name;
    char component[260];
    long parent = spec->parID;
    int n = spec->name[0];
    memcpy(path, spec->name + 1, n);
    path[n] = 0;
    while (parent > 1) {
        memset(&pb, 0, sizeof(pb));
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec->vRefNum;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = parent;
        if (PBGetCatInfoSync(&pb) != noErr)
            break;
        n = name[0];
        if (strlen(path) + n + 1 >= 1024)
            break;
        memcpy(component, name + 1, n);
        component[n] = ':';
        component[n + 1] = 0;
        memmove(path + n + 1, path, strlen(path) + 1);
        memcpy(path, component, n + 1);
        parent = pb.dirInfo.ioDrParID;
    }
}

int main(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec spec;
    Str255 name;
    char path[1024];
    FILE *log = fopen("Retro68:Template01:runtime.log", "w");
    if (!log)
        return 1;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof(info));
        info.processInfoLength = sizeof(info);
        info.processName = name;
        info.processAppSpec = &spec;
        if (GetProcessInformation(&psn, &info) == noErr) {
            path_for(&spec, path);
            fprintf(log, "%s\n", path);
        }
    }
    return fclose(log) == 0 ? 0 : 1;
}
