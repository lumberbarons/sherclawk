/* Native MPW executor. Own the worker-lock before the ready rename. Only the current producer's
 * validated snapshot is eligible; shell text is never interpreted. Copies and
 * reply-log writes are paged. An uncertain command retains ownership until
 * drained, and its claim remains forever without a success/result record.
 * Flush/readback establishes AFP visibility, not a power-loss guarantee. */
#include "selfbuild.h"
#include "toolserver.h"
#include "tools.h"
#include "config.h"
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static NativeJob snapshot;
static BuildPlan plan;
static FSSpec lock;
static long lock_id,native_dir;
static char directory[256],command[2048],stage[160];
static int initialized,owned,running,unknown,phase,input,step,reply_stream;
static long offset,log_offsets[2];
static ToolServerReply reply;
static void quiet(const char *fmt,...) { (void)fmt; }
static void leaf(long parent,const char *name,FSSpec *s)
{
    memset(s,0,sizeof(*s)); s->vRefNum=snapshot.volume; s->parID=parent;
    s->name[0]=(unsigned char)strlen(name); memcpy(s->name+1,name,s->name[0]);
}
static OSErr catalog(const FSSpec *s,CInfoPBRec *p)
{
    memset(p,0,sizeof(*p)); p->hFileInfo.ioNamePtr=(unsigned char *)s->name;
    p->hFileInfo.ioVRefNum=s->vRefNum; p->hFileInfo.ioDirID=s->parID;
    return PBGetCatInfoSync(p);
}
static int plain(const CInfoPBRec *p)
{
    return !(p->hFileInfo.ioFlAttrib & 16) && !(p->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) &&
        p->hFileInfo.ioFlFndrInfo.fdType!='alis' && !p->hFileInfo.ioFlRLgLen;
}
static OSErr transfer(const FSSpec *s,long at,void *bytes,long n,int write)
{
    short ref; long got=n; OSErr e,c;
    e=FSpOpenDF(s,write ? fsWrPerm : fsRdPerm,&ref); if(e)return e;
    e=SetFPos(ref,fsFromStart,at);
    if(!e)e=write ? FSWrite(ref,&got,bytes) : FSRead(ref,&got,bytes);
    c=FSClose(ref);
    return e && !(e==eofErr && got==n) ? e : c ? c : got==n ? noErr : ioErr;
}
static int absent(long parent,const char *name)
{ FSSpec s; CInfoPBRec p; leaf(parent,name,&s); return catalog(&s,&p)==fnfErr; }
static int create_file(long parent,const char *name,FSSpec *s)
{
    leaf(parent,name,s);
    return !absent(parent,name) || FSpCreate(s,'ttxt','TEXT',smSystemScript) ? -1 : 0;
}
static int record(long parent,const char *name,const char *text)
{
    FSSpec s; Str255 final; char temporary[40],page[JOB_PAGE]; long n=(long)strlen(text);
    CInfoPBRec p;
    if(n>JOB_PAGE || !absent(parent,name))return -1;
    snprintf(temporary,sizeof(temporary),"%s.tmp",name);
    if(create_file(parent,temporary,&s) || transfer(&s,0,(void *)text,n,1) ||
        FlushVol(NULL,s.vRefNum) || catalog(&s,&p) || !plain(&p) || p.hFileInfo.ioFlLgLen!=n ||
        transfer(&s,0,page,n,0) || memcmp(page,text,(size_t)n))return -1;
    final[0]=(unsigned char)strlen(name); memcpy(final+1,name,final[0]);
    return FSpRename(&s,final) || FlushVol(NULL,s.vRefNum) ? -1 : 0;
}
static void release(void)
{
    CInfoPBRec p;
    if(!owned || toolserver_busy())return;
    /* Never remove a replacement lock. A failed unlock is left for inspection. */
    if(!catalog(&lock,&p) && (p.hFileInfo.ioFlAttrib & 16) &&
        !(p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) && p.dirInfo.ioDrDirID==lock_id)
        if(!FSpDelete(&lock))FlushVol(NULL,lock.vRefNum);
    owned=0;
}
static void uncertain(const char *reason,uint32_t now)
{
    char evidence[240];
    unknown=1; running=0;
    toolserver_poll(now,1,&reply);
    snprintf(evidence,sizeof(evidence),"phase=%d\ninput=%d\nstep=%d\nreason=%s",phase,input,step,reason);
    record(snapshot.directory,"native-unknown",evidence);
    release();
}
int selfbuild_init(void)
{
    if(initialized)return 0;
    initialized=!toolserver_init(quiet); return initialized ? 0 : -1;
}
void selfbuild_close(void)
{
    /* Closing with an undrained command deliberately leaves the lock. */
    if(!running)release();
    if(initialized)toolserver_close();
    initialized=owned=running=0;
}
int selfbuild_unknown(void) { return unknown; }
void selfbuild_drain(uint32_t now)
{
    if(initialized && !running) {
        int r=toolserver_poll(now,0,&reply);
        if(owned && r==1) {
            char text[120];
            snprintf(text,sizeof(text),"raw_status=%ld\nabandoned=%d\nmalformed=%d\n",(long)reply.status,reply.abandoned,reply.malformed);
            record(snapshot.directory,"native-drained",text);
        }
        release();
    }
}
/* Refusals that need no side effects. The caller fills in nothing but the
 * queue; `dir` receives its directory ID when the queue is usable. */
static int refusal(const FSSpec *queue,long *dir)
{
    CInfoPBRec q;
    if(!initialized)return SELFBUILD_UNAVAILABLE;
    if(owned || running || toolserver_busy())return SELFBUILD_BUSY;
    if(catalog(queue,&q) || !(q.hFileInfo.ioFlAttrib & 16) ||
        (q.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))return SELFBUILD_BLOCKED;
    snapshot.volume=queue->vRefNum;  /* leaf()/absent() resolve names on it; idle here */
    if(!absent(q.dirInfo.ioDrDirID,"STOP") || !absent(q.dirInfo.ioDrDirID,"worker-lock"))return SELFBUILD_BLOCKED;
    *dir=q.dirInfo.ioDrDirID; return 0;
}
int selfbuild_check(const FSSpec *queue) { long dir; return refusal(queue,&dir); }
SelfBuildStart selfbuild_begin(const NativeJob *j,const FSSpec *queue,const BuildPlan *p)
{
    CInfoPBRec cat; FSSpec s; char marker[12],text[160]; BuildPlan checked; OSErr e; long dir; int why;
    unknown=0;
    why=refusal(queue,&dir); if(why)return (SelfBuildStart)why;
    snapshot=*j; plan=*p; phase=input=step=0;
    leaf(dir,"worker-lock",&lock);
    e=FSpDirCreate(&lock,smSystemScript,&lock_id);
    if(e)return SELFBUILD_BLOCKED;  /* lost the race, or the volume refuses */
    owned=1;
    if(FlushVol(NULL,lock.vRefNum))goto fail;
    /* Check all native outputs before claiming. Existing claims never replay. */
    { const char *names[]={"claimed","started","started.tmp","stdout","stderr","result","result.tmp","build","native-executor","native-executor.tmp","native-unknown","native-unknown.tmp","native-drained","native-drained.tmp"};
      size_t k; for(k=0;k<sizeof(names)/sizeof(*names);k++)if(!absent(j->directory,names[k]))goto fail; }
    /* Revalidate descriptor from the immutable published representation. All
     * inputs, including descriptor and script, are compared to disk below. */
    if(j->count!=p->count+3 || strcmp(j->inputs[0].name,"project.json") ||
        build_project_plan(j->inputs[0].bytes,&checked) || memcmp(&checked,p,sizeof(checked)))goto fail;
    for(int i=0;i<p->count;i++)if(strcmp(j->inputs[i+1].name,p->staged[i]))goto fail;
    leaf(j->directory,"ready",&s);
    if(catalog(&s,&cat) || !plain(&cat) || cat.hFileInfo.ioFlLgLen!=11 ||
        transfer(&s,0,marker,11,0) || memcmp(marker,"protocol=1\n",11))goto fail;
    if(FSpRename(&s,(const unsigned char *)"\007claimed") || FlushVol(NULL,s.vRefNum))goto fail;
    snprintf(text,sizeof(text),"protocol=1\nid=%s\nstate=claimed\n",j->id);
    if(record(j->directory,"started",text) ||
        record(j->directory,"native-executor","executor=Sherclawk\nadapter=mpw-ppc-v2\n"))goto fail;
    if(create_file(j->directory,"stdout",&s) || create_file(j->directory,"stderr",&s))goto fail;
    running=1; phase=0; input=step=0; offset=0; log_offsets[0]=log_offsets[1]=0;
    if(snprintf(directory,sizeof(directory),"%s" SHERCLAWK_BUILD_QUEUE ":%s:build:native:",tools_workspace(),j->id)>=(int)sizeof(directory))goto fail;
    return SELFBUILD_STARTED;
fail:
    uncertain("native claim/setup uncertain; no replay\n",j->start); return SELFBUILD_UNCERTAIN;
}
static int folder(long parent,const char *name,long *id,int existing)
{
    FSSpec s; CInfoPBRec p; OSErr e; leaf(parent,name,&s); e=catalog(&s,&p);
    if(e==fnfErr)return FSpDirCreate(&s,smSystemScript,id) || FlushVol(NULL,s.vRefNum) ? -1 : 0;
    if(e || !existing || !(p.hFileInfo.ioFlAttrib & 16) ||
        (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))return -1;
    *id=p.dirInfo.ioDrDirID; return 0;
}
static int destination(int i,FSSpec *s)
{
    char path[96],*part,*colon; long parent=native_dir;
    strcpy(path,plan.paths[i]); part=path;
    while((colon=strchr(part,':'))) {
        *colon=0; if(folder(parent,part,&parent,1))return -1; part=colon+1;
    }
    leaf(parent,part,s); return 0;
}
static int log_bytes(int stream,const char *bytes,long n)
{
    FSSpec s; CInfoPBRec p; char verify[JOB_PAGE];
    leaf(snapshot.directory,stream ? "stderr" : "stdout",&s);
    if(n>JOB_PAGE || catalog(&s,&p) || !plain(&p) || p.hFileInfo.ioFlLgLen!=log_offsets[stream] ||
        transfer(&s,log_offsets[stream],(void *)bytes,n,1) || FlushVol(NULL,s.vRefNum) ||
        transfer(&s,log_offsets[stream],verify,n,0) || memcmp(verify,bytes,(size_t)n))return -1;
    log_offsets[stream]+=n; return 0;
}
static int terminal(int code)
{
    char text[240];
    snprintf(text,sizeof(text),"protocol=1\nid=%s\noutcome=%s\nexit=%d\nsignal=0\nwait_status=%d\n",
        snapshot.id,code ? "failed" : "succeeded",code,code<<8);
    if(record(snapshot.directory,"result",text))return -1;
    running=0; release(); return 0;
}
/* ToolServer raw 2 is MPW failure, not shell exit 2. Negative/oversized
 * statuses are never invented successes; no retry on memory failures. */
static int status_code(long status)
{ return status==-1 ? 127 : status==2 ? 1 : status>=0 && status<=255 ? (int)status : -1; }
void selfbuild_step(uint32_t now,int stop)
{
    FSSpec s,dest; CInfoPBRec p; char page[JOB_PAGE],text[256]; long n; int r; OSErr e;
    if(!running)return;
    if(stop || (uint32_t)(now-snapshot.start)>=snapshot.timeout)goto fail;
    if(phase==0) {
        /* Re-read every published input in bounded pages before executing. */
        const JobInput *in=&snapshot.inputs[input]; leaf(snapshot.directory,in->name,&s);
        n=(long)in->size-offset; if(n>JOB_PAGE)n=JOB_PAGE;
        if(catalog(&s,&p) || !plain(&p) || p.hFileInfo.ioFlLgLen!=(long)in->size ||
            transfer(&s,offset,page,n,0) || memcmp(page,(const char *)in->bytes+offset,(size_t)n))goto fail;
        offset+=n;
        if(offset==(long)in->size) { offset=0; if(++input==snapshot.count) {phase=1;input=0;} }
    } else if(phase==1) {
        long build;
        if(folder(snapshot.directory,"build",&build,0) || folder(build,"native",&native_dir,0) ||
            log_bytes(0,"stage=prepare started\n",22))goto fail;
        phase=2;
    } else if(phase==2) {
        const JobInput *in=&snapshot.inputs[input+1];
        leaf(snapshot.directory,plan.staged[input],&s);
        if(destination(input,&dest))goto fail;
        if(!offset && FSpCreate(&dest,'ttxt','TEXT',smSystemScript))goto fail;
        n=(long)in->size-offset; if(n>JOB_PAGE)n=JOB_PAGE;
        if(transfer(&s,offset,page,n,0) || memcmp(page,(const char *)in->bytes+offset,(size_t)n) ||
            transfer(&dest,offset,page,n,1) || FlushVol(NULL,dest.vRefNum) ||
            transfer(&dest,offset,page,n,0) || memcmp(page,(const char *)in->bytes+offset,(size_t)n))goto fail;
        offset+=n;
        if(offset==(long)in->size) { offset=0; if(++input==plan.count)phase=3; }
    } else if(phase==3) {
        if(!absent(lock.parID,"STOP") ||
            build_project_command(&plan,step,command,sizeof(command),stage,sizeof(stage)))goto fail;
        snprintf(text,sizeof(text),"%s\n",stage);
        if(log_bytes(0,text,(long)strlen(text)))goto fail;
        e=toolserver_send(directory,command,now);
        if(e && !step && !toolserver_busy()) {
            /* ToolServer could not be found or launched and nothing was
             * delivered, so no command ran: a definite rejection. */
            static const char note[]="ToolServer could not be found or launched; no command ran.\n";
            snprintf(text,sizeof(text),"protocol=1\nid=%s\noutcome=rejected\n",snapshot.id);
            if(log_bytes(1,note,(long)sizeof(note)-1) || record(snapshot.directory,"result",text))goto fail;
            running=0; release(); return;
        }
        if(e)goto fail;
        phase=4;
    } else if(phase==4) {
        r=toolserver_poll(now,0,&reply);
        if(r<0)goto fail;
        if(r==1) {
            if(reply.malformed || reply.abandoned || status_code(reply.status)<0)goto fail;
            reply_stream=0; offset=0; phase=5;
        }
    } else if(phase==5) {
        const char *bytes=reply_stream ? reply.diagnostic : reply.output;
        n=(long)strlen(bytes)-offset; if(n>JOB_PAGE)n=JOB_PAGE;
        if(n && log_bytes(reply_stream,bytes+offset,n))goto fail;
        offset+=n;
        if(offset==(long)strlen(bytes)) {
            offset=0;
            if(++reply_stream==2) {
                snprintf(text,sizeof(text),"stage=command step=%d raw_status=%ld exit=%d\n",step,(long)reply.status,status_code(reply.status));
                if(log_bytes(0,text,(long)strlen(text)))goto fail;
                if(reply.status) { if(terminal(status_code(reply.status)))goto fail; }
                else phase=++step==plan.sources+1+plan.resources ? 6 : 3;
            }
        }
    } else if(phase==6) {
        FInfo finder; unsigned char header[12];
        leaf(native_dir,plan.output,&s);
        if(catalog(&s,&p) || (p.hFileInfo.ioFlAttrib & 16) || (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) ||
            p.hFileInfo.ioFlLgLen<40 || p.hFileInfo.ioFlRLgLen<=0 ||
            transfer(&s,0,header,12,0) || memcmp(header,"Joy!peffpwpc",12) || FSpGetFInfo(&s,&finder))goto fail;
        finder.fdType='APPL'; finder.fdCreator=((unsigned long)(unsigned char)plan.creator[0]<<24) |
            ((unsigned long)(unsigned char)plan.creator[1]<<16) | ((unsigned long)(unsigned char)plan.creator[2]<<8) | (unsigned char)plan.creator[3];
        if(FSpSetFInfo(&s,&finder) || FlushVol(NULL,s.vRefNum) || catalog(&s,&p) ||
            p.hFileInfo.ioFlFndrInfo.fdType!='APPL' || p.hFileInfo.ioFlFndrInfo.fdCreator!=finder.fdCreator)goto fail;
        snprintf(text,sizeof(text),"artifact=%s\n",plan.output);
        if(record(native_dir,"success.txt",text) || log_bytes(0,"stage=complete status=0\n",24) || terminal(0))goto fail;
    }
    return;
fail:
    uncertain("native execution outcome unknown; no replay\n",now);
}
