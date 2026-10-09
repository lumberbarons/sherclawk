/* Native File Manager publication preserves LF scripts, snapshot bytes and
 * complete-file ready visibility. No executor-owned files are ever rewritten.
 * FlushVol is a visibility barrier, not a verified AFP power-loss guarantee. */
#include "jobs.h"
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static void leaf(NativeJob *j, const char *name, FSSpec *s)
{
    memset(s, 0, sizeof(*s)); s->vRefNum=j->volume; s->parID=j->directory;
    s->name[0]=(unsigned char)strlen(name); memcpy(s->name+1,name,s->name[0]);
}
static OSErr info(const FSSpec *s, CInfoPBRec *p)
{
    memset(p,0,sizeof(*p)); p->hFileInfo.ioNamePtr=(unsigned char *)s->name;
    p->hFileInfo.ioVRefNum=s->vRefNum; p->hFileInfo.ioDirID=s->parID;
    return PBGetCatInfoSync(p);
}
static int alias(const CInfoPBRec *p)
{
    return p->hFileInfo.ioFlFndrInfo.fdType=='alis' ||
        (p->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000);
}
static int record(NativeJob *j, const char *event)
{
    char json[256];
    snprintf(json,sizeof(json),"{\"id\":\"%s\",\"volume\":%d,\"directory\":%ld,\"published\":%s,\"state\":%d,\"exit\":%d,\"signal\":%d,\"wait_status\":%d}",
        j->id,j->volume,j->directory,j->published ? "true" : "false",
        j->state,j->exit_code,j->signal,j->wait_status);
    return j->journal(j->context,event,json);
}
static void fail(NativeJob *j, OSErr e)
{
    j->error=e; j->state=j->published ? JOB_UNKNOWN : JOB_ABANDONED;
}
static int id_valid(const char *s)
{
    size_t i,n=strlen(s);
    if(!n || n>24 || !strcmp(s,"worker-lock")) return 0;
    for(i=0;i<n;i++) if(!((s[i]>='a' && s[i]<='z') ||
        (s[i]>='0' && s[i]<='9') || s[i]=='_' || s[i]=='-')) return 0;
    return 1;
}
static int input_valid(const JobInput *in)
{
    const char *reserved[]={"ready","ready.tmp","claimed","started","started.tmp",
        "stdout","stderr","result","result.tmp"};
    size_t k,n=strlen(in->name);
    if(!n || n>31 || in->size>JOB_FILE_MAX || (!in->bytes && in->size)) return 0;
    /* Portable lowercase names avoid HFS case collisions and shell surprises. */
    for(k=0;k<n;k++) if(!((in->name[k]>='a' && in->name[k]<='z') ||
        (in->name[k]>='0' && in->name[k]<='9') || in->name[k]=='_' ||
        in->name[k]=='-' || in->name[k]=='.')) return 0;
    if(!strcmp(in->name,".") || !strcmp(in->name,"..")) return 0;
    for(k=0;k<sizeof(reserved)/sizeof(*reserved);k++) if(!strcmp(in->name,reserved[k])) return 0;
    if(!strcmp(in->name,"script")) {
        const unsigned char *b=in->bytes;
        if(!in->size) return 0;
        for(k=0;k<in->size;k++) if(b[k]==0 || b[k]=='\r') return 0;
    }
    return 1;
}
int jobs_begin(NativeJob *j,const FSSpec *queue,const char *id,
    const JobInput *inputs,int count,uint32_t now,uint32_t timeout,
    AgentJournal journal,void *ctx)
{
    CInfoPBRec p; FSSpec s; int i,k,scripts=0; size_t total=0; OSErr e;
    memset(j,0,sizeof(*j)); j->state=JOB_ABANDONED;
    if(!queue || !journal || !id || !id_valid(id) || !inputs || count<1 || count>JOB_INPUT_MAX ||
        !timeout || timeout>INT32_MAX) return -1;
    for(i=0;i<count;i++) {
        if(!inputs[i].name || !input_valid(&inputs[i])) return -1;
        for(k=0;k<i;k++) if(!strcmp(inputs[i].name,inputs[k].name)) return -1;
        total+=inputs[i].size; scripts+=!strcmp(inputs[i].name,"script");
    }
    if(total>JOB_TOTAL_MAX || scripts!=1) return -1;
    e=info(queue,&p);
    if(e || !(p.hFileInfo.ioFlAttrib & 16) || alias(&p)) { j->error=e ? e : paramErr; return -1; }
    j->volume=queue->vRefNum; j->directory=p.dirInfo.ioDrDirID;
    strcpy(j->id,id); j->journal=journal; j->context=ctx;
    /* Recovery intent names the queue directory; reservation never reused. */
    if(record(j,"job_intent")) return -1;
    leaf(j,id,&s); e=FSpDirCreate(&s,smSystemScript,&j->directory);
    if(e) { j->error=e; return -1; }
    if(FlushVol(NULL,j->volume) || record(j,"job_reserved")) return -1;
    memcpy(j->inputs,inputs,(size_t)count*sizeof(*inputs)); j->count=count;
    j->start=now; j->timeout=timeout; j->next_poll=now; j->state=JOB_STAGING;
    return 0;
}
/* Closed-file range I/O; a short transfer or close error is a failed step. */
static OSErr transfer(NativeJob *j,const char *name,long offset,void *bytes,long n,int write)
{
    FSSpec s; short ref; long got=n; OSErr e,closed;
    leaf(j,name,&s); e=FSpOpenDF(&s,write ? fsWrPerm : fsRdPerm,&ref);
    if(e) return e;
    e=SetFPos(ref,fsFromStart,offset);
    if(!e) e=write ? FSWrite(ref,&got,bytes) : FSRead(ref,&got,bytes);
    closed=FSClose(ref);
    if(e && !(e==eofErr && got==n)) return e;
    return closed ? closed : got==n ? noErr : ioErr;
}
static void stage(NativeJob *j)
{
    FSSpec s; CInfoPBRec p; OSErr e; char page[JOB_PAGE]; long n;
    if(j->input<j->count) {
        JobInput *in=&j->inputs[j->input]; leaf(j,in->name,&s);
        if(!j->verifying && !j->offset) {
            e=FSpCreate(&s,'ShCk','TEXT',smSystemScript);
            if(e) { fail(j,e); return; }
        }
        n=(long)in->size-j->offset; if(n>JOB_PAGE) n=JOB_PAGE;
        e=transfer(j,in->name,j->offset,j->verifying ? (void *)page :
            (void *)((in->bytes ? (const char *)in->bytes : "")+j->offset),n,!j->verifying);
        if(e || (j->verifying && n && memcmp(page,(const char *)in->bytes+j->offset,(size_t)n))) {
            fail(j,e ? e : ioErr); return;
        }
        j->offset+=n;
        if(j->offset==(long)in->size) {
            if(!j->verifying) {
                e=FlushVol(NULL,j->volume);
                if(!e) e=info(&s,&p);
                if(e || p.hFileInfo.ioFlLgLen!=(long)in->size || p.hFileInfo.ioFlRLgLen ||
                    (p.hFileInfo.ioFlAttrib & 16) || alias(&p)) { fail(j,e ? e : ioErr); return; }
                j->verifying=1; j->offset=0;
            } else { j->verifying=0; j->offset=0; j->input++; }
        }
        return;
    }
    if(record(j,"job_staged")) { fail(j,ioErr); return; }
    leaf(j,"ready.tmp",&s); e=FSpCreate(&s,'ShCk','TEXT',smSystemScript);
    if(!e) e=transfer(j,"ready.tmp",0,"protocol=1\n",11,1);
    if(!e) e=FlushVol(NULL,j->volume);
    if(!e) e=transfer(j,"ready.tmp",0,page,11,0);
    if(e || memcmp(page,"protocol=1\n",11)) { fail(j,e ? e : ioErr); return; }
    /* From here any failure is uncertain: AFP may have completed the rename. */
    j->published=1;
    e=FSpRename(&s,(const unsigned char *)"\005ready");
    if(e || FlushVol(NULL,j->volume) || record(j,"job_published")) { fail(j,e ? e : ioErr); return; }
    j->state=JOB_WAITING;
}
/* Bounded decimal parser avoids scanf integer overflow on hostile records. */
static int number(const char **at,const char *key,int maximum,int *value)
{
    const char *p=*at; int n=0, digits=0; size_t len=strlen(key);
    if(strncmp(p,key,len)) return -1;
    p+=len;
    while(*p>='0' && *p<='9') {
        if(n>(maximum-(*p-'0'))/10) return -1;
        n=n*10+(*p++-'0'); digits++;
    }
    if(!digits || *p++!='\n') return -1;
    *at=p; *value=n; return 0;
}
/* Exact line order, final LF, canonical decimals, no NUL/CR/extra fields. */
int jobs_parse_result(NativeJob *j,const char *s,size_t size)
{
    char copy[JOB_RECORD_MAX+1],expected[JOB_RECORD_MAX+1],prefix[80];
    const char *p,*outcome; int exit_code,signal,status; JobState state;
    if(!size || size>JOB_RECORD_MAX || memchr(s,0,size) || memchr(s,'\r',size)) return -1;
    memcpy(copy,s,size); copy[size]=0;
    snprintf(prefix,sizeof(prefix),"protocol=1\nid=%s\noutcome=",j->id);
    if(strncmp(copy,prefix,strlen(prefix))) return -1;
    p=copy+strlen(prefix);
    if(!strcmp(p,"rejected\n")) { j->state=JOB_REJECTED; return 0; }
    if(!strncmp(p,"succeeded\n",10)) { state=JOB_SUCCEEDED; outcome="succeeded"; p+=10; }
    else if(!strncmp(p,"failed\n",7)) { state=JOB_FAILED; outcome="failed"; p+=7; }
    else if(!strncmp(p,"signaled\n",9)) { state=JOB_SIGNALED; outcome="signaled"; p+=9; }
    else return -1;
    if(number(&p,"exit=",255,&exit_code) || number(&p,"signal=",127,&signal) ||
        number(&p,"wait_status=",65535,&status) || *p) return -1;
    if(exit_code!=(status>>8) || signal!=(status & 127) ||
        state!=(signal ? JOB_SIGNALED : exit_code ? JOB_FAILED : JOB_SUCCEEDED)) return -1;
    snprintf(expected,sizeof(expected),"protocol=1\nid=%s\noutcome=%s\nexit=%d\nsignal=%d\nwait_status=%d\n",
        j->id,outcome,exit_code,signal,status);
    if(strlen(expected)!=size || memcmp(s,expected,size)) return -1;
    j->exit_code=exit_code; j->signal=signal; j->wait_status=status; j->state=state;
    return 0;
}
static OSErr log_page(NativeJob *j,const char *name,long *offset,char *page,size_t *size)
{
    FSSpec s; CInfoPBRec p; long n; OSErr e;
    *size=0; leaf(j,name,&s); e=info(&s,&p);
    if(e==fnfErr) return noErr;
    if(e) return e;
    if(alias(&p) || (p.hFileInfo.ioFlAttrib & 16) || p.hFileInfo.ioFlRLgLen ||
        p.hFileInfo.ioFlLgLen<*offset) return paramErr;
    n=p.hFileInfo.ioFlLgLen-*offset; if(n>JOB_PAGE) n=JOB_PAGE;
    if(!n) return noErr;
    e=transfer(j,name,*offset,page,n,0);
    if(!e) { *size=(size_t)n; *offset+=n; }
    return e;
}
int jobs_logs(NativeJob *j)
{
    OSErr a,b;
    a=log_page(j,"stdout",&j->stdout_offset,j->stdout_page,&j->stdout_size);
    b=log_page(j,"stderr",&j->stderr_offset,j->stderr_page,&j->stderr_size);
    if(a || b) { j->error=a ? a : b; return -1; }
    return 0;
}
void jobs_step(NativeJob *j,uint32_t now,int stop)
{
    FSSpec s; CInfoPBRec p; char bytes[JOB_RECORD_MAX]; OSErr e;
    j->stdout_size=j->stderr_size=0;
    if(j->state!=JOB_STAGING && j->state!=JOB_WAITING) return;
    if(stop || (uint32_t)(now-j->start)>=j->timeout) {
        fail(j,noErr); record(j,stop ? "job_stopped" : "job_deadline"); return;
    }
    if(j->state==JOB_STAGING) { stage(j); return; }
    if((int32_t)(now-j->next_poll)<0) return;
    j->next_poll=now+60;
    leaf(j,"result",&s); e=info(&s,&p);
    if(!e) {
        if(alias(&p) || (p.hFileInfo.ioFlAttrib & 16) || p.hFileInfo.ioFlRLgLen ||
            p.hFileInfo.ioFlLgLen<1 || p.hFileInfo.ioFlLgLen>JOB_RECORD_MAX) { fail(j,paramErr); return; }
        e=transfer(j,"result",0,bytes,p.hFileInfo.ioFlLgLen,0);
        if(e || jobs_parse_result(j,bytes,(size_t)p.hFileInfo.ioFlLgLen)) { fail(j,e ? e : paramErr); return; }
        if(record(j,"job_terminal")) { fail(j,ioErr); return; }
    } else if(e!=fnfErr) { fail(j,e); return; }
    if(jobs_logs(j)) fail(j,j->error);
}
