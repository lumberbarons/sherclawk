/* Descriptors are data: only enumerated settings and portable relative names
 * enter the trusted MPW recipe. Closed source snapshots are read twice before
 * queue reservation; jobs.c then stages/readbacks them in cooperative steps.
 * The fixed queue creates itself before first publication; the queue stays
 * read-only to model tools. Stop/deadline never cancel or replay published
 * compiler jobs. */
#include "build_project.h"
#include "run_application.h"
#include "json.h"
#include "text.h"
#include "config.h"
#include "selfbuild.h"
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define INPUTS BUILD_INPUTS
#define FILE_BYTES TOOLS_ACCEPTED_FILE_CAP
#define RECIPE_BYTES 12288
#define QUEUE SHERCLAWK_BUILD_QUEUE
#define ADAPTER "mpw-ppc-v2"
typedef BuildPlan Descriptor;
static Descriptor descriptor;
static NativeJob job;
static char arena[TOOLS_SNAPSHOT_CAP + INPUTS + 1], recipe[RECIPE_BYTES], manifest[4096];
static size_t arena_used,source_offsets[INPUTS+1];
static char *input_bytes(int i) { return arena+source_offsets[i]; }
static uint32_t hashes[INPUTS+1];
static char project[512], id[25], diagnostic[257];
static size_t diagnostic_size;
static FSSpec specs[INPUTS+1];
static CInfoPBRec infos[INPUTS+1];
static long sizes[INPUTS+1];
static int active, phase, file_index, second_pass, native_attempted, native_claimed, not_started;
static FSSpec build_queue;
static long offset;
static uint32_t start_ticks;
static AgentJournal journal_fn;
static void *journal_context;
static unsigned long sequence;
static OSErr info(FSSpec *s,CInfoPBRec *p)
{
    memset(p,0,sizeof(*p)); p->hFileInfo.ioNamePtr=s->name;
    p->hFileInfo.ioVRefNum=s->vRefNum; p->hFileInfo.ioDirID=s->parID;
    return PBGetCatInfoSync(p);
}
static int plain(const CInfoPBRec *p)
{
    return !(p->hFileInfo.ioFlAttrib & 16) && !p->hFileInfo.ioFlRLgLen &&
        p->hFileInfo.ioFlFndrInfo.fdType=='TEXT' && !(p->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) &&
        p->hFileInfo.ioFlLgLen>=0;
}
static int same(const CInfoPBRec *a,const CInfoPBRec *b)
{
    return a->hFileInfo.ioDirID==b->hFileInfo.ioDirID && a->hFileInfo.ioFlMdDat==b->hFileInfo.ioFlMdDat &&
        a->hFileInfo.ioFlLgLen==b->hFileInfo.ioFlLgLen && plain(b) &&
        a->hFileInfo.ioFlFndrInfo.fdCreator==b->hFileInfo.ioFlFndrInfo.fdCreator;
}
static uint32_t hash(const char *p,size_t n)
{
    uint32_t h=2166136261UL; size_t i;
    for(i=0;i<n;i++) h=(h^(unsigned char)p[i])*16777619UL;
    return h;
}
static int add(char *out,size_t cap,size_t *at,const char *fmt,...)
{
    va_list args; int n;
    if(*at>=cap)return -1;
    va_start(args,fmt); n=vsnprintf(out+*at,cap-*at,fmt,args); va_end(args);
    if(n<0 || (size_t)n>=cap-*at)return -1;
    *at+=(size_t)n; return 0;
}
static int keys(const char *s,const JsonToken *t,int object,const char *allowed)
{
    int i,j; char key[64],prev[64],match[68];
    if(object<0 || t[object].type!=JSON_OBJECT)return -1;
    for(i=object+1;i<t[object].next;i=t[i+1].next) {
        if(json_string(s,t,i,key,sizeof(key))<0)return -1;
        snprintf(match,sizeof(match),"|%s|",key); if(!strstr(allowed,match))return -1;
        for(j=object+1;j<i;j=t[j+1].next) {
            if(json_string(s,t,j,prev,sizeof(prev))<0 || !strcmp(prev,key))return -1;
        }
    }
    return 0;
}
/* Lowercase names avoid HFS case collisions. Colon paths have no empty,
 * dot, parent, absolute, MPW metacharacter or shell metacharacter components. */
static int name_valid(const char *s,int path)
{
    size_t i,component=0; const char *part=s;
    if(!*s || strlen(s)>(path ? 95U : 31U))return 0;
    for(i=0;;i++) {
        char c=s[i];
        if(!c || c==':') {
            if(!component || component>31 || (component==1 && *part=='.') ||
                (component==2 && part[0]=='.' && part[1]=='.') || (c && !path))return 0;
            if(!c)return 1;
            component=0; part=s+i+1;
        } else {
            if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))return 0;
            if(!component && (c=='-' || c=='.'))return 0;
            component++;
        }
    }
}
static int array(const char *s,const JsonToken *t,const char *key,Descriptor *d,int required,const char *ext)
{
    int a=json_member(s,t,0,key),i,k,begin=d->count;
    if(a<0)return required ? -1 : 0;
    if(t[a].type!=JSON_ARRAY)return -1;
    for(i=a+1;i<t[a].next;i=t[i].next) {
        char *p; size_t n;
        if(d->count==INPUTS)return -1;
        p=d->paths[d->count];
        if(json_string(s,t,i,p,96)<0 || !name_valid(p,1))return -1;
        n=strlen(p); if(n<strlen(ext) || strcmp(p+n-strlen(ext),ext))return -1;
        for(k=0;k<d->count;k++)if(!strcmp(p,d->paths[k]))return -1;
        snprintf(d->staged[d->count],32,"input%d%s",d->count,ext); d->count++;
    }
    return required && d->count==begin ? -1 : d->count-begin;
}
static int parse(const char *s,Descriptor *d)
{
    JsonToken t[128]; int a,i,k; char value[64];
    memset(d,0,sizeof(*d)); strcpy(d->creator,"ShCk"); d->stdclib=1;
    if(json_parse(s,strlen(s),t,128)<1 || keys(s,t,0,"|protocol||toolchain||sources||resources||headers||include_paths||output||settings||template|"))return -1;
    a=json_member(s,t,0,"protocol"); if(a<0 || t[a].type!=JSON_PRIMITIVE || t[a].end-t[a].start!=1 || s[t[a].start]!='2')return -1;
    if(json_string(s,t,json_member(s,t,0,"toolchain"),value,sizeof(value))<0 || strcmp(value,ADAPTER))return -1;
    if(json_string(s,t,json_member(s,t,0,"output"),d->output,32)<0 || !name_valid(d->output,0) || !strcmp(d->output,"success.txt"))return -1;
    d->sources=array(s,t,"sources",d,1,".c"); if(d->sources<1)return -1;
    d->resources=array(s,t,"resources",d,0,".r"); if(d->resources<0 || array(s,t,"headers",d,0,".h")<0)return -1;
    a=json_member(s,t,0,"include_paths");
    if(a>=0) {
        if(t[a].type!=JSON_ARRAY)return -1;
        for(i=a+1;i<t[a].next;i=t[i].next) {
            if(d->include_count==3 || json_string(s,t,i,d->includes[d->include_count],96)<0)return -1;
            if(strcmp(d->includes[d->include_count],".") && !name_valid(d->includes[d->include_count],1))return -1;
            for(k=0;k<d->include_count;k++)if(!strcmp(d->includes[k],d->includes[d->include_count]))return -1;
            d->include_count++;
        }
    }
    a=json_member(s,t,0,"template"); if(a>=0 && json_string(s,t,a,value,sizeof(value))<0)return -1;
    a=json_member(s,t,0,"settings");
    if(a>=0) {
        int b;
        if(keys(s,t,a,"|warnings||libraries||creator|"))return -1;
        b=json_member(s,t,a,"warnings");
        if(b>=0 && (json_string(s,t,b,value,sizeof(value))<0 || strcmp(value,"off")))return -1;
        b=json_member(s,t,a,"creator");
        if(b>=0) {
            if(json_string(s,t,b,d->creator,5)<0 || strlen(d->creator)!=4)return -1;
            for(i=0;i<4;i++)if(!((d->creator[i]>='A' && d->creator[i]<='Z') ||
                (d->creator[i]>='a' && d->creator[i]<='z') || (d->creator[i]>='0' && d->creator[i]<='9')))return -1;
        }
        b=json_member(s,t,a,"libraries");
        if(b>=0) {
            int interface=0,standard=0;
            if(t[b].type!=JSON_ARRAY)return -1;
            for(i=b+1;i<t[b].next;i=t[i].next) {
                if(json_string(s,t,i,value,sizeof(value))<0)return -1;
                if(!strcmp(value,"InterfaceLib")) { if(interface++)return -1; }
                else if(!strcmp(value,"StdCLib")) { if(standard++)return -1; }
                else return -1;
            }
            if(!interface)return -1;
            d->stdclib=standard;
        }
    }
    return 0;
}
static void unix_path(const char *s,char *out)
{
    while(*s) { *out++=*s==':' ? '/' : *s; s++; } *out=0;
}
int build_project_plan(const char *s,BuildPlan *p) { return parse(s,p); }
int build_project_command(const BuildPlan *d,int step,char *out,size_t cap,
                          char *stage,size_t stage_cap)
{
    size_t at=0; int i;
    if(step<0 || step>=d->sources+1+d->resources)return -1;
    out[0]=0;
    if(step<d->sources) {
        snprintf(stage,stage_cap,"stage=compile source=%s",d->paths[step]);
        if(add(out,cap,&at,"MrC \":%s\" -o obj%d.o -i \"{CIncludes}\" -w off",d->paths[step],step))return -1;
    } else if(step==d->sources) {
        snprintf(stage,stage_cap,"stage=link started");
        if(add(out,cap,&at,"PPCLink -o %s",d->output))return -1;
        for(i=0;i<d->sources;i++)if(add(out,cap,&at," obj%d.o",i))return -1;
        return add(out,cap,&at," \"{SharedLibraries}\"InterfaceLib%s \"{PPCLibraries}\"StdCRuntime.o \"{PPCLibraries}\"PPCCRuntime.o -t APPL",d->stdclib ? " \"{SharedLibraries}\"StdCLib" : "");
    } else {
        i=step-1;
        snprintf(stage,stage_cap,"stage=resources source=%s",d->paths[i]);
        if(add(out,cap,&at,"Rez \":%s\" -o %s -append -i \"{RIncludes}\"",d->paths[i],d->output))return -1;
    }
    for(i=0;i<d->include_count;i++)
        if(add(out,cap,&at," -i \"%s%s\"",!strcmp(d->includes[i],".") ? "" : ":",!strcmp(d->includes[i],".") ? ":" : d->includes[i]))return -1;
    return 0;
}
int build_project_recipe(const char *s,char *out,size_t cap)
{
    Descriptor d; size_t at=0; int i; char path[96],parent[96],*slash;
    if(parse(s,&d))return -1;
    if(add(out,cap,&at,"#!/bin/sh\n# Trusted adapter " ADAPTER " (MrC 4.1.0f1c1/PPCLink 1.5.2/Rez 3.4.1).\nset -e\nset -u\nmkdir build\nmkdir build/native\ncd build/native\necho 'stage=prepare started'\n"))return -1;
    for(i=0;i<d.count;i++) {
        unix_path(d.paths[i],path); strcpy(parent,path); slash=strrchr(parent,'/');
        if(slash) { *slash=0; if(add(out,cap,&at,"mkdir -p '%s'\n",parent))return -1; }
        if(add(out,cap,&at,"cp '../../%s' '%s'\n/Developer/Tools/SetFile -t TEXT -c ttxt '%s'\n",d.staged[i],path,path))return -1;
    }
    for(i=0;i<d.sources+1+d.resources;i++) {
        char command[2048],stage[160];
        if(build_project_command(&d,i,command,sizeof(command),stage,sizeof(stage)))return -1;
        /* Commands contain only validated names and fixed MPW syntax. Shell
         * single quotes preserve MPW's double quotes without expansion. */
        if(add(out,cap,&at,"echo '%s'\n/Developer/Tools/tlsrvr -- ",stage))return -1;
        /* Preserve MPW quotes through the shell. Portable validated names
         * contain no spaces/apostrophes; fixed macro arguments need quoting. */
        { char *word=command,*end;
          while(*word) {
            end=strchr(word,' '); if(end)*end=0;
            if(add(out,cap,&at,strchr(word,'"') ? "'%s'" : "%s",word))return -1;
            if(!end)break;
            if(add(out,cap,&at," "))return -1;
            word=end+1;
          }
          if(add(out,cap,&at,"\n"))return -1;
        }
    }
    return add(out,cap,&at,"/Developer/Tools/SetFile -t APPL -c %s %s\ntest -s %s\necho 'artifact=%s' > success.txt\necho 'stage=complete status=0'\n",d.creator,d.output,d.output,d.output);
}
static int error_message(char *out,size_t cap,const char *code,const char *message,int stop)
{
    active=0;
    snprintf(out,cap,"{\"status\":\"error\",\"code\":\"%s\",%s%s%s\"build_id\":\"%s\",\"queue\":\"" QUEUE "\",\"adapter\":\"" ADAPTER "\"}",
        code,message ? "\"message\":\"" : "",message ? message : "",message ? "\"," : "",id);
    return stop;
}
static int error(char *out,size_t cap,const char *code,int stop) { return error_message(out,cap,code,NULL,stop); }
/* Actionable text for a build that never started. None of these ran a
 * compiler, so the snapshot is retained but nothing is uncertain. */
static const char *start_code(int why)
{
    return why==SELFBUILD_BUSY ? "NATIVE_EXECUTOR_BUSY" :
        why==SELFBUILD_BLOCKED ? "NATIVE_QUEUE_BLOCKED" : "NATIVE_EXECUTOR_UNAVAILABLE";
}
static const char *start_message(int why)
{
    return why==SELFBUILD_BUSY ? "An earlier ToolServer command is still draining, so no build was started. Tell the user and wait before requesting another build." :
        why==SELFBUILD_BLOCKED ? "The build queue holds a worker-lock or STOP marker, or cannot be used, so no build was started. Tell the user to inspect Worker01:buildjobs; do not try to remove either." :
        "MPW ToolServer is unavailable, so no build ran. Tell the user to install MPW ToolServer with the SDK, or quit and relaunch Sherclawk, and retry.";
}
static int resolve_input(int i)
{
    char path[768];
    snprintf(path,sizeof(path),"%s:%s",project,i ? descriptor.paths[i-1] : "project.json");
    return tools_resolve(path,&specs[i]) || info(&specs[i],&infos[i]) || !plain(&infos[i]) ? -1 : 0;
}
int build_project_begin(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *ctx,uint32_t now)
{
    JsonToken t[16]; FSSpec folder; CInfoPBRec p;
    if(active) { snprintf(out,cap,"{\"status\":\"error\",\"code\":\"BUILD_BUSY\"}"); return 1; }
    id[0]=0;
    if(json_parse(call->arguments,strlen(call->arguments),t,16)<1 || keys(call->arguments,t,0,"|path|") ||
        json_string(call->arguments,t,json_member(call->arguments,t,0,"path"),project,sizeof(project))<0)return error(out,cap,"PROJECT_PATH",0);
    /* list_files returns folder paths with a trailing colon. Accept that same
     * spelling, but leave validation of all other components to tools_resolve. */
    { size_t n=strlen(project); if(n && project[n-1]==':')project[n-1]=0; }
    if(tools_resolve(project,&folder) || info(&folder,&p) || !(p.hFileInfo.ioFlAttrib & 16) ||
        (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))return error(out,cap,"PROJECT_PATH",0);
    if(!journal)return error(out,cap,"JOURNAL_REQUIRED",1);
    memset(&job,0,sizeof(job)); job.state=JOB_ABANDONED;
    journal_fn=journal; journal_context=ctx; start_ticks=now;
    if(resolve_input(0))return error(out,cap,"DESCRIPTOR_TEXT_OR_SIZE",0);
    sizes[0]=infos[0].hFileInfo.ioFlLgLen;
    if(sizes[0]>TOOLS_DESCRIPTOR_CAP)return error_message(out,cap,"SNAPSHOT_SIZE_LIMIT", TOOLS_SNAPSHOT_LIMIT_MESSAGE, 0);
    source_offsets[0]=0;arena_used=(size_t)sizes[0]+1;hashes[0]=2166136261UL;
    native_attempted=native_claimed=not_started=0; diagnostic_size=0; diagnostic[0]=0; phase=0; file_index=0; offset=0; second_pass=0; active=1;
    return 2;
}
static int read_step(int i)
{
    CInfoPBRec p; FSSpec current; short ref; long n=sizes[i]-offset,got; OSErr e,closed;
    char page[JOB_PAGE],path[768];
    if(n>JOB_PAGE)n=JOB_PAGE;
    snprintf(path,sizeof(path),"%s:%s",project,i ? descriptor.paths[i-1] : "project.json");
    if(tools_resolve(path,&current) || current.vRefNum!=specs[i].vRefNum || current.parID!=specs[i].parID ||
        info(&current,&p) || !same(&infos[i],&p))return -1;
    e=FSpOpenDF(&current,fsRdPerm,&ref); if(e)return -1;
    e=SetFPos(ref,fsFromStart,offset); got=n;
    if(!e)e=FSRead(ref,&got,page);
    closed=FSClose(ref);
    if((e && !(e==eofErr && got==n)) || closed || got!=n)return -1;
    if(second_pass) { if(memcmp(page,input_bytes(i)+offset,(size_t)n))return -1; }
    else {
        long k;
        for(k=0;k<n;k++) {
            unsigned char c=(unsigned char)page[k];
            if(c==127 || (c<32 && c!='\r' && c!='\t'))return -1;
            hashes[i]=(hashes[i]^c)*16777619UL;
        }
        memcpy(input_bytes(i)+offset,page,(size_t)n);
    }
    offset+=n; input_bytes(i)[sizes[i]]=0;
    if(offset==sizes[i]) {
        offset=0;
        if(!second_pass)second_pass=1;
        else { second_pass=0; return 1; }
    }
    return 0;
}
static int finish(char *out,size_t cap,uint32_t now)
{
    const char *status=job.state==JOB_SUCCEEDED ? "ok" : job.state==JOB_UNKNOWN ? "uncertain" : "error";
    char artifact[256],q[520],utf8[1025],qd[1300],reason[400];
    artifact[0]=0; reason[0]=0;
    /* A rejected record means ToolServer never received the first command. */
    if(job.state==JOB_REJECTED)not_started=SELFBUILD_UNAVAILABLE;
    if(not_started)snprintf(reason,sizeof(reason),"\"code\":\"%s\",\"message\":\"%s\",",start_code(not_started),start_message(not_started));
    if(job.state==JOB_SUCCEEDED) {
        FSSpec s; CInfoPBRec p; char expected[80],observed[80]; short ref; long n; OSErr e,c;
        snprintf(artifact,sizeof(artifact),QUEUE ":%s:build:native:success.txt",id);
        snprintf(expected,sizeof(expected),"artifact=%s\n",descriptor.output);
        if(tools_resolve(artifact,&s) || info(&s,&p) || !plain(&p) || p.hFileInfo.ioFlLgLen!=(long)strlen(expected) || FSpOpenDF(&s,fsRdPerm,&ref))return error(out,cap,"ARTIFACT_RECORD_INVALID",1);
        n=(long)strlen(expected); e=FSRead(ref,&n,observed); c=FSClose(ref);
        if(e || c || n!=(long)strlen(expected) || memcmp(observed,expected,(size_t)n))return error(out,cap,"ARTIFACT_RECORD_INVALID",1);
        snprintf(artifact,sizeof(artifact),QUEUE ":%s:build:native:%s",id,descriptor.output);
        if(tools_resolve(artifact,&s) || info(&s,&p) || (p.hFileInfo.ioFlAttrib & 16) ||
            (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || p.hFileInfo.ioFlFndrInfo.fdType!='APPL' || p.hFileInfo.ioFlLgLen<=0 || p.hFileInfo.ioFlRLgLen<=0)return error(out,cap,"ARTIFACT_INVALID",1);
    }
    json_quote(artifact,q,sizeof(q));
    for(size_t k=0;k<diagnostic_size;k++)if((unsigned char)diagnostic[k]<32 && diagnostic[k]!='\r' && diagnostic[k]!='\n' && diagnostic[k]!='\t')diagnostic[k]='?';
    if(text_to_utf8(diagnostic,diagnostic_size,utf8,sizeof(utf8))<0 || json_quote(utf8,qd,sizeof(qd))<0)strcpy(qd,"\"[Use read_build_log]\"");
    snprintf(out,cap,"{\"status\":\"%s\",%s\"build_id\":\"%s\",\"snapshot\":\"" QUEUE ":%s\",\"adapter\":\"" ADAPTER "\",\"state\":%d,\"exit\":%d,\"os_error\":%d,\"artifact\":%s,\"logs\":\"read_build_log with stream stdout or stderr and byte offset\",\"next_stdout_byte\":0,\"next_stderr_byte\":0,\"launch_supported\":%s,\"diagnostics\":%s}",status,reason,id,id,job.state,job.exit_code,job.error,q,job.state==JOB_SUCCEEDED ? "true" : "false",qd);
    if(phase!=4 && journal_fn(journal_context,"build_result",out))return error(out,cap,"JOURNAL_AFTER_BUILD",1);
    if(job.state==JOB_SUCCEEDED && phase==2) {
        if(application_authorize_begin(id,descriptor.output,journal_fn,journal_context,now)<0)
            return error(out,cap,"ARTIFACT_AUTHORIZATION_BEGIN",1);
        phase=3; return 2;
    }
    active=0;
    return job.state==JOB_UNKNOWN || job.state==JOB_ABANDONED || job.state==JOB_REJECTED ? 1 : 0;
}
/* The fixed queue is app-owned state and may be absent on a fresh workspace.
 * Create it one level at a time from the workspace root with the create_folder
 * rules: journal intent, FSpDirCreate, flush, verify a non-alias folder. A file
 * or alias occupying a name is refused. Creation is idempotent and nothing has
 * been published yet, so a failed bootstrap may be retried. */
static int queue_record(const char *event,const char *folder)
{
    char record[256];
    if(!journal_fn)return -1;
    snprintf(record,sizeof(record),"{\"build_id\":\"%s\",\"queue\":\"" QUEUE "\",\"folder\":\"%s\",\"os_error\":0}",id,folder);
    return journal_fn(journal_context,event,record);
}
static int ensure_queue(void)
{
    char rest[64],folder[32],*part,*colon;
    FSSpec parent,spec;
    CInfoPBRec pb;
    long dir,created;
    OSErr e;
    if(tools_workspace_root(&parent) || info(&parent,&pb) || !(pb.hFileInfo.ioFlAttrib & 16) ||
        (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || strlen(QUEUE)>=sizeof(rest))return -1;
    dir=pb.dirInfo.ioDrDirID;
    strcpy(rest,QUEUE); part=rest;
    for(;;) {
        size_t n; Str255 name;
        int made=0;
        colon=strchr(part,':');
        n=colon ? (size_t)(colon-part) : strlen(part);
        if(!n || n>31)return -1;
        memcpy(folder,part,n); folder[n]=0;
        name[0]=(unsigned char)n; memcpy(name+1,part,n);
        e=FSMakeFSSpec(parent.vRefNum,dir,name,&spec);
        if(!e)e=info(&spec,&pb);
        if(e==fnfErr) {
            if(queue_record("queue_intent",folder))return -1;
            e=FSpDirCreate(&spec,smSystemScript,&created);
            if(e==dupFNErr)e=noErr;
            if(!e)e=FlushVol(NULL,spec.vRefNum);
            if(!e)e=info(&spec,&pb);
            made=1;
        }
        if(e || !(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))return -1;
        if(made && queue_record("queue_created",folder))return -1;
        dir=pb.dirInfo.ioDrDirID;
        if(!colon)break;
        part=colon+1;
    }
    return 0;
}
int build_project_step(char *out,size_t cap,uint32_t now,int stop)
{
    if(!active)return error(out,cap,"NO_ACTIVE_BUILD",1);
    if(phase<2 && (stop || (uint32_t)(now-start_ticks)>=300UL*60UL))return error(out,cap,"SNAPSHOT_ABANDONED",1);
    if(phase<2) {
        int r=read_step(file_index);
        if(r<0)return error(out,cap,"INPUT_CHANGED_OR_UNREADABLE",0);
        if(!r)return 2;
        if(phase==0) {
            if(parse(input_bytes(0),&descriptor) || build_project_recipe(input_bytes(0),recipe,sizeof(recipe)))return error(out,cap,"DESCRIPTOR_UNSUPPORTED_USE_PROTOCOL_2",0);
            for(int i=1;i<=descriptor.count;i++) {
                if(resolve_input(i))return error(out,cap,"INPUT_MISSING_TEXT_OR_SIZE",0);
                sizes[i]=infos[i].hFileInfo.ioFlLgLen;
                if(sizes[i]>FILE_BYTES || (size_t)sizes[i]+1>sizeof(arena)-arena_used)return error_message(out,cap,"SNAPSHOT_SIZE_LIMIT", TOOLS_SNAPSHOT_LIMIT_MESSAGE, 0);
                source_offsets[i]=arena_used;arena_used+=(size_t)sizes[i]+1;hashes[i]=2166136261UL;
            }
            phase=1; file_index=1; return 2;
        }
        if(++file_index<=descriptor.count)return 2;
        {
            FSSpec queue; JobInput inputs[JOB_INPUT_MAX]; size_t at=0; int i;
            snprintf(id,sizeof(id),"build-%08lx-%04lx",(unsigned long)now,(++sequence)&0xffffUL);
            if(add(manifest,sizeof(manifest),&at,"{\"build_id\":\"%s\",\"adapter\":\"" ADAPTER "\",\"recipe_hash\":\"%08lx\",\"inputs\":[",id,(unsigned long)hash(recipe,strlen(recipe))))return error(out,cap,"MANIFEST_LIMIT",0);
            for(i=0;i<=descriptor.count;i++) {
                char q[1024],path[768];
                snprintf(path,sizeof(path),"%s:%s",project,i ? descriptor.paths[i-1] : "project.json");
                if(json_quote(path,q,sizeof(q))<0 || add(manifest,sizeof(manifest),&at,"%s{\"path\":%s,\"snapshot_file\":\"%s\",\"revision\":\"full-%08lx-%08lx-%08lx-%08lx\"}",i ? "," : "",q,i ? descriptor.staged[i-1] : "project.json",(unsigned long)infos[i].hFileInfo.ioDirID,(unsigned long)infos[i].hFileInfo.ioFlMdDat,(unsigned long)sizes[i],(unsigned long)hashes[i]))return error(out,cap,"MANIFEST_LIMIT",0);
                inputs[i].name=i ? descriptor.staged[i-1] : "project.json"; inputs[i].bytes=input_bytes(i); inputs[i].size=(size_t)sizes[i];
            }
            if(add(manifest,sizeof(manifest),&at,"]}"))return error(out,cap,"MANIFEST_LIMIT",0);
            if(arena_used-(size_t)(descriptor.count+1)+strlen(recipe)+strlen(manifest)>TOOLS_SNAPSHOT_CAP)
                return error_message(out,cap,"SNAPSHOT_SIZE_LIMIT", TOOLS_SNAPSHOT_LIMIT_MESSAGE, 0);
            if(tools_resolve(QUEUE,&queue)) {
                if(ensure_queue() || tools_resolve(QUEUE,&queue))return error(out,cap,"QUEUE_MISSING",0);
            }
            /* Refuse before reserving anything when the native executor cannot
             * take the build; begin below still decides any later race. */
            { int why=selfbuild_check(&queue);
              if(why)return error_message(out,cap,start_code(why),start_message(why),1); }
            if(journal_fn(journal_context,"build_snapshot",manifest))return error(out,cap,"JOURNAL_SNAPSHOT",1);
            inputs[i].name="manifest.json"; inputs[i].bytes=manifest; inputs[i++].size=strlen(manifest);
            inputs[i].name="script"; inputs[i].bytes=recipe; inputs[i++].size=strlen(recipe);
            build_queue=queue;
            if(jobs_begin(&job,&queue,id,inputs,i,now,300UL*60UL,journal_fn,journal_context))return error(out,cap,"JOB_RESERVATION_FAILED",1);
            phase=2; return 2;
        }
    }
    if(phase==3) {
        int r=run_application_step(out,cap,now,stop);
        if(r==2)return 2;
        if(r || !strstr(out,"\"status\":\"ok\""))return error(out,cap,"ARTIFACT_AUTHORIZATION_FAILED",1);
        phase=4; return finish(out,cap,now);
    }
    if(job.state==JOB_WAITING && !native_attempted && !stop) {
        SelfBuildStart started;
        native_attempted=1;
        started=selfbuild_begin(&job,&build_queue,&descriptor);
        native_claimed=started==SELFBUILD_STARTED;
        if(started==SELFBUILD_UNCERTAIN)job.state=JOB_UNKNOWN;
        else if(!native_claimed) {
            /* Nothing was claimed and no other executor exists: the published
             * snapshot stays inert evidence and a fresh build starts over. */
            job.state=JOB_ABANDONED; not_started=(int)started;
        }
    }
    if(native_claimed)selfbuild_step(now,stop);
    if(native_claimed && selfbuild_unknown())job.state=JOB_UNKNOWN;
    jobs_step(&job,now,stop);
    if(job.stderr_size && diagnostic_size<128) {
        size_t n=job.stderr_size;
        if(n>128-diagnostic_size)n=128-diagnostic_size;
        memcpy(diagnostic+diagnostic_size,job.stderr_page,n); diagnostic_size+=n;
        diagnostic[diagnostic_size]=0;
    }
    return job.state==JOB_STAGING || job.state==JOB_WAITING ? 2 : finish(out,cap,now);
}
void build_project_log(const AgentCall *call,char *out,size_t cap)
{
    JsonToken t[24]; char build[25],stream[16],number[32],path[256],bytes[257],utf8[1025],quoted[1300];
    FSSpec s; CInfoPBRec p; long offset_value=0,n; short ref; OSErr e,c; int a,k;
    if(json_parse(call->arguments,strlen(call->arguments),t,24)<1 || keys(call->arguments,t,0,"|build_id||stream||start_byte|") ||
        json_string(call->arguments,t,json_member(call->arguments,t,0,"build_id"),build,sizeof(build))<0 || strncmp(build,"build-",6) || !name_valid(build,0) ||
        json_string(call->arguments,t,json_member(call->arguments,t,0,"stream"),stream,sizeof(stream))<0 || (strcmp(stream,"stdout") && strcmp(stream,"stderr")))goto invalid;
    a=json_member(call->arguments,t,0,"start_byte");
    if(a>=0) {
        int len=t[a].end-t[a].start;
        if(t[a].type!=JSON_PRIMITIVE || len<1 || len>=31)goto invalid;
        memcpy(number,call->arguments+t[a].start,(size_t)len); number[len]=0;
        for(k=0;k<len;k++) { if(number[k]<'0' || number[k]>'9' || offset_value>(2147483647L-(number[k]-'0'))/10)goto invalid; offset_value=offset_value*10+number[k]-'0'; }
    }
    snprintf(path,sizeof(path),QUEUE ":%s:%s",build,stream);
    if(tools_resolve(path,&s) || info(&s,&p) || (p.hFileInfo.ioFlAttrib & 16) || p.hFileInfo.ioFlRLgLen ||
        (p.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || p.hFileInfo.ioFlFndrInfo.fdType=='alis' || offset_value>p.hFileInfo.ioFlLgLen)goto invalid;
    n=p.hFileInfo.ioFlLgLen-offset_value; if(n>128)n=128;
    if(FSpOpenDF(&s,fsRdPerm,&ref))goto invalid;
    e=SetFPos(ref,fsFromStart,offset_value); long got=n;
    if(!e)e=FSRead(ref,&got,bytes);
    c=FSClose(ref);
    if(e || c || got!=n)goto invalid;
    /* Replace binary controls for display; raw log bytes remain on disk. */
    for(k=0;k<n;k++)if((unsigned char)bytes[k]<32 && bytes[k]!='\n' && bytes[k]!='\r' && bytes[k]!='\t')bytes[k]='?';
    if(text_to_utf8(bytes,(size_t)n,utf8,sizeof(utf8))<0 || json_quote(utf8,quoted,sizeof(quoted))<0)goto invalid;
    snprintf(out,cap,"{\"status\":\"ok\",\"build_id\":\"%s\",\"stream\":\"%s\",\"start_byte\":%ld,\"next_byte\":%ld,\"truncated\":%s,\"text\":%s}",build,stream,offset_value,offset_value+n,offset_value+n<p.hFileInfo.ioFlLgLen ? "true" : "false",quoted); return;
invalid:
    snprintf(out,cap,"{\"status\":\"error\",\"code\":\"BUILD_LOG_ARGUMENT_OR_FILE\"}");
}
