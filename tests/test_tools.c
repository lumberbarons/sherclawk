/* Exercise the actual native executor with a File Manager model. Faults verify
 * that journal barriers, short writes, corrupt reads and rename races cannot
 * silently overwrite an existing file or claim an uncertain create succeeded. */
#include "tools.h"
#include "json.h"
#include <Files.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct File { int used, dir; long parent, id; char name[32], bytes[5000]; long size, resource; FInfo info; } files[64];
static long positions[64];
static int short_write, bad_read, bad_close, rename_race, rename_error, published, flush_error;
static int journals, fail_journal, creates;
static size_t longest_temporary;
static AgentCall call;
static char result[AGENT_RESULT_CAP], args[AGENT_ARGUMENT_CAP];
static int find(long parent, const unsigned char *name)
{
    int i; for (i=0; i<64; i++) if (files[i].used && files[i].parent==parent &&
        strlen(files[i].name)==name[0] && !memcmp(files[i].name,name+1,name[0])) return i;
    return -1;
}
static int add(long parent, const char *name, int dir)
{
    int i; for(i=0;i<64;i++) if(!files[i].used) {
        memset(&files[i],0,sizeof(files[i])); files[i].used=1; files[i].parent=parent;
        files[i].id=i+10; files[i].dir=dir; strcpy(files[i].name,name); return i;
    }
    assert(0); return -1;
}
OSErr FSMakeFSSpec(short vol, long parent, const unsigned char *name, FSSpec *spec)
{
    (void)vol; spec->vRefNum=1;
    unsigned char leafname[32];
    if (!parent) {
        assert(name[0]>=8 && !memcmp(name+1,"Retro68:",8));
        if (name[0]==8) { parent=1;name=(const unsigned char *)"\007Retro68"; }
        else { parent=10;leafname[0]=name[0]-8;memcpy(leafname+1,name+9,leafname[0]);name=leafname; }
    }
    spec->parID=parent; memcpy(spec->name,name,(size_t)name[0]+1);
    return find(parent,name)<0 ? fnfErr : 0;
}
OSErr PBGetCatInfoSync(CInfoPBRec *pb)
{
    int i=find(pb->hFileInfo.ioDirID,pb->hFileInfo.ioNamePtr);
    if(i<0) return fnfErr;
    pb->hFileInfo.ioFlAttrib=files[i].dir ? 16 : 0;
    pb->hFileInfo.ioFlFndrInfo=files[i].info;
    pb->hFileInfo.ioFlRLgLen=files[i].resource; pb->hFileInfo.ioFlMdDat=1234;
    if(files[i].dir) pb->dirInfo.ioDrDirID=files[i].id;
    else pb->hFileInfo.ioFlLgLen=files[i].size;
    return 0;
}
OSErr FSpCreate(const FSSpec *s, unsigned long creator, unsigned long type, short script)
{
    char name[32]; int i; (void)script;
    if(find(s->parID,s->name)>=0) return dupFNErr;
    memcpy(name,s->name+1,s->name[0]); name[s->name[0]]=0; i=add(s->parID,name,0);
    files[i].info.fdType=type; files[i].info.fdCreator=creator; creates++; return 0;
}
OSErr FSpOpenDF(const FSSpec *s, short mode, short *ref)
{
    int i=find(s->parID,s->name); (void)mode; if(i<0)return fnfErr;
    *ref=(short)i; positions[i]=0; return 0;
}
OSErr FSRead(short ref, long *n, void *out)
{
    if(*n>files[ref].size-positions[ref]) *n=files[ref].size-positions[ref];
    memcpy(out,files[ref].bytes+positions[ref],(size_t)*n); positions[ref]+=*n;
    if(bad_read && *n) ((char *)out)[0]^=1;
    return 0;
}
OSErr FSWrite(short ref, long *n, const void *in)
{
    if(short_write && *n) --*n;
    assert(*n<=4096); memcpy(files[ref].bytes,in,(size_t)*n); files[ref].size=*n; return 0;
}
OSErr FSClose(short ref) { (void)ref; return bad_close ? ioErr : 0; }
OSErr SetFPos(short ref, short mode, long pos) { (void)mode; positions[ref]=pos; return 0; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name;(void)vol;return published && flush_error ? ioErr : 0; }
OSErr FSpRename(const FSSpec *s, const unsigned char *name)
{
    int i=find(s->parID,s->name); char dest[32]; assert(i>=0);
    memcpy(dest,name+1,name[0]); dest[name[0]]=0;
    if(rename_race) { int other=add(s->parID,dest,0); strcpy(files[other].bytes,"racer");files[other].size=5; }
    if(find(s->parID,name)>=0)return dupFNErr;
    if(rename_error)return ioErr;
    strcpy(files[i].name,dest);published=1;return 0;
}
long FreeMem(void) { return 100000; }
short Gestalt(long selector, long *v) { (void)selector;*v=0x922;return 0; }
unsigned long TickCount(void) { return 42; }
static int journal(void *ctx, const char *event, const char *json)
{
    JsonToken tokens[128]; (void)ctx;
    assert(json_parse(json,strlen(json),tokens,128)>0);
    { char temp[768]; int mutation=json_member(json,tokens,0,"mutation");
      int n=json_string(json,tokens,json_member(json,tokens,mutation,"temporary_path"),temp,sizeof(temp));
      assert(n>=0);if((size_t)n>longest_temporary)longest_temporary=(size_t)n; }
    journals++; assert(strstr(json,"\"call_id\":\"write1\""));
    if(journals==1) { assert(!strcmp(event,"mutation_intent"));assert(!creates); }
    if(journals==2) { assert(!strcmp(event,"mutation_staged"));assert(creates && !published); }
    if(journals==3) { assert(!strcmp(event,"mutation_committed"));assert(published); }
    return fail_journal==journals ? -1 : 0;
}
static void reset(void)
{
    memset(files,0,sizeof(files)); add(1,"Retro68",1);
    short_write=bad_read=bad_close=rename_race=rename_error=published=flush_error=0;
    journals=fail_journal=creates=0;longest_temporary=0;
    memset(&call,0,sizeof(call));strcpy(call.id,"write1");strcpy(call.name,"write_text");
    strcpy(call.arguments,"{\"path\":\"hello.c\",\"text\":\"caf\\u00e9\\r\\nline\\n\"}");
}
static int run(void) { return tools_execute_recorded(&call,result,sizeof(result),journal,NULL); }
static int leaf(const char *name)
{
    unsigned char p[32];p[0]=(unsigned char)strlen(name);memcpy(p+1,name,p[0]);return find(10,p);
}
int main(void)
{
    int i;
    reset();assert(!run());assert(strstr(result,"CREATED") && journals==3);
    i=leaf("hello.c");assert(i>=0 && files[i].size==10 && !memcmp(files[i].bytes,"caf\x8e\rline\r",10));
    assert(files[i].info.fdType=='TEXT' && !files[i].resource);
    { char revision[80];JsonToken tokens[128];json_parse(result,strlen(result),tokens,128);
      json_string(result,tokens,json_member(result,tokens,0,"revision"),revision,sizeof(revision));
      strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");tools_execute(&call,result,sizeof(result));
      assert(strstr(result,revision) && strstr(result,"caf\xc3\xa9")); }
    strcpy(call.name,"write_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"text\":\"overwrite\"}");
    assert(!run() && strstr(result,"EXISTS") && files[i].size==10 && journals==3);
    reset();tools_execute(&call,result,sizeof(result));assert(strstr(result,"JOURNAL") && !creates);
    for(i=1;i<=3;i++) { reset();fail_journal=i;assert(run());assert(i==3 ? leaf("hello.c")>=0 : leaf("hello.c")<0);assert(i==1 ? !creates : creates==1); }
    reset();short_write=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")<0 && journals==1);
    reset();bad_read=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")<0);
    reset();bad_close=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")<0);
    reset();rename_race=1;assert(!run() && strstr(result,"EXISTS_STAGE_RETAINED"));i=leaf("hello.c");assert(i>=0 && !memcmp(files[i].bytes,"racer",5) && journals==2);
    reset();rename_error=1;assert(run() && strstr(result,"uncertain") && !published);
    reset();flush_error=1;assert(run() && strstr(result,"uncertain") && leaf("hello.c")>=0);
    { const char *bad[]={"{\"path\":\":escape\",\"text\":\"x\"}","{\"path\":\"missing:hello.c\",\"text\":\"x\"}",
        "{\"path\":\"hello.c\",\"text\":\"\\ud83e\\udd80\"}","{\"path\":\"hello.c\",\"text\":\"\\u0001\"}",
        "{\"path\":\"hello.c\",\"text\":\"x\",\"text\":\"y\"}","{\"path\":\"hello.c\",\"text\":\"x\",\"mode\":\"overwrite\"}",
        "{\"path\":\"hello.c\"}","{\"path\":\"hello.c\",\"text\":\"\\u0000\"}","{\"path\":\"hello.c\",\"text\":\"\\u007f\"}"};
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){reset();strcpy(call.arguments,bad[i]);assert(!run() && strstr(result,"error") && !creates && !journals);} }
    reset();i=add(10,"alias",0);files[i].info.fdFlags=0x8000;
    strcpy(call.arguments,"{\"path\":\"alias:hello.c\",\"text\":\"x\"}");assert(!run() && !creates);
    reset();i=add(10,"folder",1);strcpy(call.arguments,"{\"path\":\"folder:hello.c\",\"text\":\"x\"}");assert(!run() && journals==3 && published);
    reset();strcpy(call.arguments,"{\"path\":\"empty.txt\",\"text\":\"\"}");assert(!run() && leaf("empty.txt")>=0 && journals==3);
    reset();memset(args,'x',4096);args[4096]=0;snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"large.c\",\"text\":\"%s\"}",args);
    assert(!run() && leaf("large.c")>=0);
    reset();memset(args,'x',4097);args[4097]=0;snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"large.c\",\"text\":\"%s\"}",args);
    assert(!run() && strstr(result,"ENCODING_LIMIT") && !creates);
    /* A legal UTF-8 destination can have a sibling recovery path longer than
     * 512 bytes when the short final name is replaced by a staging name. */
    reset();
    { char path[512]="", native_name[32], quoted[1100];long parent=10;int d,k;
      for(d=0;d<6;d++) {
          int n=d==5 ? 7 : 31;
          memset(native_name,0xdb,(size_t)n);native_name[n]=0;
          k=add(parent,native_name,1);parent=files[k].id;
          for(k=0;k<n;k++)strcat(path,"\xe2\x82\xac");
          strcat(path,":");
      }
      strcat(path,"a.c");assert(json_quote(path,quoted,sizeof(quoted))>0);
      snprintf(call.arguments,sizeof(call.arguments),"{\"path\":%s,\"text\":\"x\"}",quoted);
      assert(!run() && strstr(result,"CREATED") && journals==3 && longest_temporary>512);
    }
    puts("PASS native executor: create/read/collision, encoding, bounds, journal barriers, I/O faults, rename races and uncertain outcomes");
    return 0;
}
