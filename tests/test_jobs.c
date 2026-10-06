/* Native job protocol fault model: publication barriers, bounded event steps,
 * malformed completion, log continuation, Stop/deadline and no replay. */
#include "jobs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct File { int used,folder; long parent,id,size,resource; unsigned short flags; char name[32],bytes[JOB_TOTAL_MAX]; };
static struct File files[32];
static long position[32];
static int open_count,read_bytes,write_bytes,short_write,corrupt_read,close_error,flush_error,rename_error;
static int records,journal_error,creates,renames;
static NativeJob job;
static FSSpec queue={1,1,{5,'q','u','e','u','e'}};
static const char script[]="echo native\n";
static const JobInput inputs[]={ {"script",script,sizeof(script)-1}, {"main.c","int main(void) {}\r",18} };
static int find(long parent,const unsigned char *name)
{
    int i; for(i=0;i<32;i++) if(files[i].used && files[i].parent==parent && strlen(files[i].name)==name[0] &&
        !memcmp(files[i].name,name+1,name[0])) return i;
    return -1;
}
static int add(long parent,const char *name,int folder,const char *bytes,size_t size)
{
    int i; for(i=0;i<32;i++) if(!files[i].used) {
        memset(&files[i],0,sizeof(files[i])); files[i].used=1; files[i].folder=folder;
        files[i].parent=parent; files[i].id=i+10; strcpy(files[i].name,name);
        if(size) memcpy(files[i].bytes,bytes,size);
        files[i].size=(long)size; return i;
    }
    abort();
}
OSErr PBGetCatInfoSync(CInfoPBRec *p)
{
    int i=find(p->hFileInfo.ioDirID,p->hFileInfo.ioNamePtr); if(i<0) return fnfErr;
    p->hFileInfo.ioFlAttrib=files[i].folder ? 16 : 0;
    p->hFileInfo.ioFlRLgLen=files[i].resource;
    p->hFileInfo.ioFlFndrInfo.fdFlags=files[i].flags;
    if(files[i].folder) p->dirInfo.ioDrDirID=files[i].id;
    else p->hFileInfo.ioFlLgLen=files[i].size;
    return 0;
}
OSErr FSpCreate(const FSSpec *s,unsigned long creator,unsigned long type,short script_code)
{
    char name[32]; (void)creator;(void)type;(void)script_code;
    if(find(s->parID,s->name)>=0) return dupFNErr;
    memcpy(name,s->name+1,s->name[0]); name[s->name[0]]=0;
    add(s->parID,name,0,NULL,0); creates++; return 0;
}
OSErr FSpDirCreate(const FSSpec *s,short code,long *id)
{
    OSErr e=FSpCreate(s,0,0,code); int i;
    if(e) return e;
    i=find(s->parID,s->name); files[i].folder=1; *id=files[i].id; return 0;
}
OSErr FSpOpenDF(const FSSpec *s,short mode,short *ref)
{
    int i=find(s->parID,s->name); (void)mode; if(i<0) return fnfErr;
    *ref=(short)i; position[i]=0; open_count++; return 0;
}
OSErr SetFPos(short ref,short mode,long offset) { (void)mode; position[ref]=offset; return 0; }
OSErr FSWrite(short ref,long *n,const void *bytes)
{
    if(short_write && *n) --*n;
    assert(position[ref]+*n<=JOB_TOTAL_MAX); memcpy(files[ref].bytes+position[ref],bytes,(size_t)*n);
    position[ref]+=*n; if(position[ref]>files[ref].size) files[ref].size=position[ref];
    write_bytes+=(int)*n; return 0;
}
OSErr FSRead(short ref,long *n,void *bytes)
{
    if(*n>files[ref].size-position[ref]) *n=files[ref].size-position[ref];
    memcpy(bytes,files[ref].bytes+position[ref],(size_t)*n); position[ref]+=*n; read_bytes+=(int)*n;
    if(corrupt_read && *n) ((char *)bytes)[0]^=1;
    return 0;
}
OSErr FSClose(short ref) { (void)ref; open_count--; return close_error ? ioErr : 0; }
OSErr FlushVol(const unsigned char *name,short volume) { (void)name;(void)volume; return flush_error ? ioErr : 0; }
OSErr FSpRename(const FSSpec *s,const unsigned char *name)
{
    int i=find(s->parID,s->name); assert(i>=0); renames++;
    assert(open_count==0);
    if(find(s->parID,name)>=0) return dupFNErr;
    if(rename_error) return ioErr;
    memcpy(files[i].name,name+1,name[0]); files[i].name[name[0]]=0; return 0;
}
static int journal(void *ctx,const char *event,const char *json)
{
    (void)ctx; assert(strstr(json,"\"id\":\"native1\"")); records++;
    if(!strcmp(event,"job_staged")) assert(!renames && !open_count);
    if(!strcmp(event,"job_published")) assert(renames==1 && !open_count);
    return records==journal_error ? -1 : 0;
}
static void reset(void)
{
    memset(files,0,sizeof(files)); add(1,"queue",1,NULL,0);
    open_count=read_bytes=write_bytes=short_write=corrupt_read=close_error=flush_error=rename_error=0;
    records=journal_error=creates=renames=0;
}
static int begin(void) { return jobs_begin(&job,&queue,"native1",inputs,2,0,7200,journal,NULL); }
static void step(uint32_t now,int stop)
{
    read_bytes=write_bytes=0; jobs_step(&job,now,stop);
    assert(read_bytes<=2*JOB_PAGE+JOB_RECORD_MAX && write_bytes<=JOB_PAGE && !open_count);
}
static void publish(void)
{
    int i; for(i=0;i<100 && job.state==JOB_STAGING;i++) step(0,0);
    assert(job.state==JOB_WAITING && job.published && renames==1 && records==4);
}
static void result_record(const char *text) { add(job.directory,"result",0,text,strlen(text)); }
static void parser(void)
{
    static const char *bad[]={
        "protocol=2\nid=native1\noutcome=rejected\n",
        "protocol=1\nid=other\noutcome=rejected\n",
        "protocol=1\nid=native1\noutcome=rejected",
        "protocol=1\nid=native1\noutcome=succeeded\nexit=7\nsignal=0\nwait_status=1792\n",
        "protocol=1\nid=native1\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=1\n",
        "protocol=1\nid=native1\noutcome=succeeded\nexit=000\nsignal=0\nwait_status=0\n",
        "protocol=1\nid=native1\noutcome=succeeded\nexit=9999999999999999999999999999999\nsignal=0\nwait_status=0\n",
        "protocol=1\nid=native1\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\nextra=1\n"};
    const char *good[]={
        "protocol=1\nid=native1\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\n",
        "protocol=1\nid=native1\noutcome=failed\nexit=7\nsignal=0\nwait_status=1792\n",
        "protocol=1\nid=native1\noutcome=signaled\nexit=0\nsignal=15\nwait_status=15\n",
        "protocol=1\nid=native1\noutcome=rejected\n"};
    size_t i,k; strcpy(job.id,"native1");
    for(i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(jobs_parse_result(&job,bad[i],strlen(bad[i]))<0);
    for(i=0;i<sizeof(good)/sizeof(*good);i++) {
        assert(!jobs_parse_result(&job,good[i],strlen(good[i])));
        for(k=0;k<strlen(good[i]);k++) assert(jobs_parse_result(&job,good[i],k)<0);
    }
}
int main(void)
{
    int i,n; char bytes[3*JOB_PAGE]; JobInput invalid[2];
    parser(); reset(); assert(!begin()); publish();
    assert(!memcmp(files[2].bytes,script,sizeof(script)-1));
    assert(!memcmp(files[4].bytes,"protocol=1\n",11));
    assert(begin()<0 && renames==1); /* Never reuse a published reservation. */
    reset(); assert(!begin()); publish(); memset(bytes,'x',sizeof(bytes));
    add(job.directory,"stdout",0,bytes,sizeof(bytes)); add(job.directory,"stderr",0,"error\n",6);
    step(0,0); assert(job.stdout_size==JOB_PAGE && job.stderr_size==6);
    step(1,0); assert(!job.stdout_size && !job.stderr_size);
    result_record("protocol=1\nid=native1\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\n");
    step(60,0); assert(job.state==JOB_SUCCEEDED && job.stdout_offset==2*JOB_PAGE);
    assert(!jobs_logs(&job) && job.stdout_size==JOB_PAGE);
    assert(!jobs_logs(&job) && job.stdout_size==0);
    reset(); assert(!begin()); step(0,1); assert(job.state==JOB_ABANDONED && !renames);
    reset(); assert(!begin()); publish(); step(0,1); assert(job.state==JOB_UNKNOWN && renames==1);
    step(0,0); assert(renames==1); /* stop cannot republish */
    reset(); assert(!begin()); publish(); step(7200,0); assert(job.state==JOB_UNKNOWN);
    reset(); assert(!jobs_begin(&job,&queue,"native1",inputs,2,UINT32_MAX-10,100,journal,NULL));
    while(job.state==JOB_STAGING) step(UINT32_MAX-10,0);
    step(40,0); assert(job.state==JOB_WAITING); step(90,0); assert(job.state==JOB_UNKNOWN);
    for(i=1;i<=4;i++) {
        reset(); journal_error=i;
        n=begin();
        if(i<=2) assert(n<0 && !renames);
        else { assert(!n); while(job.state==JOB_STAGING) step(0,0);
            assert(job.state==(i==4 ? JOB_UNKNOWN : JOB_ABANDONED)); assert(renames==(i==4)); }
    }
    reset(); assert(!begin()); short_write=1; step(0,0); assert(job.state==JOB_ABANDONED && !renames);
    reset(); assert(!begin()); step(0,0); corrupt_read=1; step(0,0); assert(job.state==JOB_ABANDONED);
    reset(); assert(!begin()); close_error=1; step(0,0); assert(job.state==JOB_ABANDONED && !open_count);
    reset(); assert(!begin()); flush_error=1; step(0,0); assert(job.state==JOB_ABANDONED);
    reset(); assert(!begin()); rename_error=1;
    while(job.state==JOB_STAGING) step(0,0);
    assert(job.state==JOB_UNKNOWN && renames==1);
    reset(); assert(!begin()); publish(); result_record("protocol=1\nid=native1\noutcome=succeeded\n");
    step(0,0); assert(job.state==JOB_UNKNOWN);
    reset(); assert(!begin()); publish();
    add(job.directory,"result.tmp",0,"invalid",7); step(0,0); assert(job.state==JOB_WAITING);
    i=add(job.directory,"stdout",0,"abc",3); step(60,0); files[i].size=2;
    step(120,0); assert(job.state==JOB_UNKNOWN);
    reset(); files[0].flags=0x8000; assert(begin()<0 && !records);
    reset(); invalid[0]=inputs[0]; invalid[1]=inputs[1]; invalid[1].name="READY";
    assert(jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)<0 && !records);
    invalid[1].name="ready";
    assert(jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)<0 && !records);
    invalid[1]=inputs[0];
    assert(jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)<0);
    invalid[0].bytes="bad\r"; invalid[0].size=4;
    assert(jobs_begin(&job,&queue,"native1",invalid,1,0,7200,journal,NULL)<0);
    invalid[0].size=0;
    assert(jobs_begin(&job,&queue,"native1",invalid,1,0,7200,journal,NULL)<0);
    reset(); memset(bytes,'x',sizeof(bytes)); invalid[0]=inputs[0]; invalid[1]=inputs[1];
    invalid[1].bytes=bytes; invalid[1].size=sizeof(bytes);
    assert(!jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)); publish();
    assert(files[3].size==sizeof(bytes) && !memcmp(files[3].bytes,bytes,sizeof(bytes)));
    reset(); invalid[0]=inputs[0]; invalid[1]=inputs[1]; invalid[1].bytes=NULL; invalid[1].size=0;
    assert(!jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)); publish();
    reset(); assert(!begin()); publish(); journal_error=records+1;
    result_record("protocol=1\nid=native1\noutcome=succeeded\nexit=0\nsignal=0\nwait_status=0\n");
    step(0,0); assert(job.state==JOB_UNKNOWN);
    reset(); assert(!begin());
    while(job.input<job.count && job.state==JOB_STAGING) step(0,0);
    short_write=1; step(0,0); assert(job.state==JOB_ABANDONED && !renames);
    reset(); assert(!begin()); publish();
    i=add(job.directory,"result",0,"invalid",7); files[i].flags=0x8000;
    step(0,0); assert(job.state==JOB_UNKNOWN);
    reset(); assert(!begin()); publish();
    i=add(job.directory,"stdout",0,"abc",3); files[i].resource=1;
    step(0,0); assert(job.state==JOB_UNKNOWN);
    reset(); {
        static char boundary[JOB_FILE_MAX];
        memset(boundary,'z',sizeof(boundary)); invalid[0]=inputs[0]; invalid[1]=inputs[1];
        invalid[1].bytes=boundary; invalid[1].size=sizeof(boundary);
        assert(!jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL));
        while(job.state==JOB_STAGING) step(0,0);
        assert(job.state==JOB_WAITING && files[3].size==JOB_FILE_MAX &&
            !memcmp(files[3].bytes,boundary,sizeof(boundary)));
        invalid[1].size=JOB_FILE_MAX+1;
        assert(jobs_begin(&job,&queue,"native1",invalid,2,0,7200,journal,NULL)<0);
    }
    puts("native job checks passed"); return 0;
}
