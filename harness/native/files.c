#include "files.h"
#include <Folders.h>
#include <Processes.h>
#include <OSUtils.h>
#include <stdio.h>
#include <string.h>
int h_spec(const char *path, FSSpec *spec)
{
    Str255 s;
    size_t n = strlen(path);
    if (!n || n > 255) return paramErr;
    s[0] = (unsigned char)n; memcpy(s+1,path,n);
    return FSMakeFSSpec(0,0,s,spec);
}
static int full_path(short vref, long dir, char *out, size_t cap)
{
    CInfoPBRec pb;
    Str255 name;
    char part[257], path[256] = "";
    int depth;
    for (depth=0; depth<32; depth++) {
        memset(&pb,0,sizeof(pb)); pb.dirInfo.ioNamePtr=name;
        pb.dirInfo.ioVRefNum=vref; pb.dirInfo.ioFDirIndex=-1; pb.dirInfo.ioDrDirID=dir;
        if (PBGetCatInfoSync(&pb)) return 0;
        memcpy(part,name+1,name[0]); part[name[0]]=':'; part[name[0]+1]=0;
        if (strlen(part)+strlen(path) >= sizeof(path)) return 0;
        strcat(part,path); strcpy(path,part);
        if (dir == fsRtDirID) {
            if (strlen(path) >= cap) return 0;
            strcpy(out,path); return 1;
        }
        dir=pb.dirInfo.ioDrParID;
    }
    return 0;
}
int h_private_path(const char *name, char *out, size_t cap)
{
    short vref;
    long dir;
    if (FindFolder(kOnSystemDisk,kPreferencesFolderType,kDontCreateFolder,&vref,&dir) ||
        !full_path(vref,dir,out,cap) || strlen(out)+strlen(name) >= cap) return 0;
    strcat(out,name); return 1;
}
int h_read(const char *path, char *out, size_t cap)
{
    FSSpec spec;
    short ref;
    long n, size;
    OSErr err = h_spec(path,&spec), closed;
    if (err == fnfErr) return -2;
    if (err || FSpOpenDF(&spec,fsRdPerm,&ref)) return -1;
    err=GetEOF(ref,&size);
    if (!err && size >= 0 && (unsigned long)size < cap) {
        n=size; err=FSRead(ref,&n,out);
        if (n != size) err=ioErr;
    } else err=ioErr;
    closed=FSClose(ref);
    if (err || closed) return -1;
    out[size]=0; return (int)size;
}
int h_publish(const char *path, const char *data, size_t size)
{
    char staging[256];
    FSSpec dest, stage;
    short ref;
    long n=(long)size;
    OSErr err, closed;
    static char verify[H_REPORT_CAP];
    const char *name=strrchr(path,':');
    if (size >= sizeof(verify) || !name || strlen(name+1)>27 || snprintf(staging,sizeof(staging),"%s.tmp",path)>=(int)sizeof(staging)) return 0;
    if (h_spec(path,&dest) != fnfErr || h_spec(staging,&stage) != fnfErr ||
        FSpCreate(&stage,'HARN','TEXT',0)) return 0;
    if (FSpOpenDF(&stage,fsWrPerm,&ref)) return 0;
    err=FSWrite(ref,&n,data);
    if (!err && (size_t)n != size) err=ioErr;
    if (!err) err=FlushVol(NULL,stage.vRefNum);
    closed=FSClose(ref);
    if (err || closed || h_read(staging,verify,sizeof(verify)) != (int)size || memcmp(verify,data,size)) return 0;
    /* FSpRename never replaces an existing destination. Flush the catalog too. */
    err=FSpRename(&stage,dest.name);
    return !err && !FlushVol(NULL,stage.vRefNum);
}
int h_self_matches(const char *path)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec self, target;
    if (h_spec(path,&target) || GetCurrentProcess(&psn)) return 0;
    memset(&info,0,sizeof(info)); info.processInfoLength=sizeof(info); info.processAppSpec=&self;
    return !GetProcessInformation(&psn,&info) && self.vRefNum == target.vRefNum &&
        self.parID == target.parID && self.name[0] == target.name[0] &&
        !memcmp(self.name+1,target.name+1,self.name[0]);
}
void h_mailpath(char *out, size_t cap, const HRequest *r, const char *kind)
{
    /* 20 hex chars keep HFS names + .tmp below 31 bytes; full run stays in body. */
    snprintf(out,cap,"Retro68:Harness:%s:%s-%.20s",r->slot,kind,r->run);
}
unsigned long h_now(void)
{
    unsigned long now;
    GetDateTime(&now); return now;
}
