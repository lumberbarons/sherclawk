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
static int editing, renames, fault_rename, change_after_stage, stage_bad_read, stage_short_read, busy, swapped_publish;
static int opens[64];
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
    else { pb->hFileInfo.ioFlLgLen=files[i].size;pb->hFileInfo.ioDirID=files[i].id; }
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
    int i=find(s->parID,s->name); if(i<0)return fnfErr;
    if((mode==fsRdWrPerm && (busy || opens[i])) || opens[i]==fsRdWrPerm)return ioErr;
    opens[i]=mode; *ref=(short)i; positions[i]=0; return 0;
}
OSErr FSRead(short ref, long *n, void *out)
{
    if(*n>files[ref].size-positions[ref]) *n=files[ref].size-positions[ref];
    if(stage_short_read && *n && !strncmp(files[ref].name,"Sherclawk tmp",13)) --*n;
    memcpy(out,files[ref].bytes+positions[ref],(size_t)*n); positions[ref]+=*n;
    if((bad_read || (stage_bad_read && !strncmp(files[ref].name,"Sherclawk tmp",13))) && *n) ((char *)out)[0]^=1;
    return 0;
}
OSErr FSWrite(short ref, long *n, const void *in)
{
    if(short_write && *n) --*n;
    assert(*n<=4096); memcpy(files[ref].bytes,in,(size_t)*n); files[ref].size=*n; return 0;
}
OSErr FSClose(short ref) { opens[ref]=0; return bad_close ? ioErr : 0; }
OSErr SetFPos(short ref, short mode, long pos) { (void)mode; positions[ref]=pos; return 0; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name;(void)vol;return published && flush_error ? ioErr : 0; }
OSErr FSpRename(const FSSpec *s, const unsigned char *name)
{
    int i=find(s->parID,s->name); char dest[32]; assert(i>=0);
    renames++;
    memcpy(dest,name+1,name[0]); dest[name[0]]=0;
    if(rename_race && (!fault_rename || fault_rename==renames)) { int other=add(s->parID,dest,0); strcpy(files[other].bytes,"racer");files[other].size=5; }
    if(find(s->parID,name)>=0)return dupFNErr;
    if(rename_error && (!fault_rename || fault_rename==renames))return ioErr;
    strcpy(files[i].name,dest);published=1;
    if(editing && renames==2 && swapped_publish) files[i].id++;
    return 0;
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
    if(journals==3) { assert(!strcmp(event,editing ? "mutation_backed_up" : "mutation_committed"));assert(published); }
    if(journals==4) { assert(editing && !strcmp(event,"mutation_committed") && published); }
    if(editing && journals==2 && change_after_stage) files[1].bytes[0]^=1;
    return fail_journal==journals ? -1 : 0;
}
static void reset(void)
{
    memset(files,0,sizeof(files)); add(1,"Retro68",1);
    short_write=bad_read=bad_close=rename_race=rename_error=published=flush_error=0;
    journals=fail_journal=creates=0;longest_temporary=0;
    editing=renames=fault_rename=change_after_stage=stage_bad_read=stage_short_read=busy=swapped_publish=0;
    memset(opens,0,sizeof(opens));
    memset(&call,0,sizeof(call));strcpy(call.id,"write1");strcpy(call.name,"write_text");
    strcpy(call.arguments,"{\"path\":\"hello.c\",\"text\":\"caf\\u00e9\\r\\nline\\n\"}");
}
static int run(void) { return tools_execute_recorded(&call,result,sizeof(result),journal,NULL); }
static int leaf(const char *name)
{
    unsigned char p[32];p[0]=(unsigned char)strlen(name);memcpy(p+1,name,p[0]);return find(10,p);
}
static void field(const char *name, char *out, size_t cap)
{
    JsonToken tokens[128];assert(json_parse(result,strlen(result),tokens,128)>0);
    assert(json_string(result,tokens,json_member(result,tokens,0,name),out,cap)>=0);
}
static int edit_setup(const char *source, const char *old, const char *replacement)
{
    char revision[80], qo[AGENT_ARGUMENT_CAP], qn[AGENT_ARGUMENT_CAP];
    int i;
    reset(); i=add(10,"hello.c",0);strcpy(files[i].bytes,source);files[i].size=(long)strlen(source);files[i].info.fdType='TEXT';
    strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");tools_execute(&call,result,sizeof(result));
    field("revision",revision,sizeof(revision));
    assert(json_quote(old,qo,sizeof(qo))>=0 && json_quote(replacement,qn,sizeof(qn))>=0);
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"hello.c\",\"expected_revision\":\"%s\",\"old_text\":%s,\"new_text\":%s}",revision,qo,qn);
    strcpy(call.name,"edit_text");editing=1;return i;
}
static void edit_checks(void)
{
    char backup[768], revision[80], updated[80], saved[AGENT_ARGUMENT_CAP], large[4098];
    int i, b, j;
    i=edit_setup("caf\x8e\rreturn 0;\r","return 0;","return 1;\n/* caf\xc3\xa9 */\r\n");
    field("revision",revision,sizeof(revision)); /* read result still present */
    assert(!run() && strstr(result,"EDITED") && journals==4);
    field("backup_path",backup,sizeof(backup));b=leaf(backup);assert(b==i);
    assert(!strcmp(files[b].bytes,"caf\x8e\rreturn 0;\r") && !opens[b]);
    i=leaf("hello.c");assert(i>=0 && !strcmp(files[i].bytes,"caf\x8e\rreturn 1;\r/* caf\x8e */\r\r"));
    field("revision",updated,sizeof(updated));assert(strcmp(updated,revision));
    strcpy(saved,call.arguments);strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"start_byte\":5}");tools_execute(&call,result,sizeof(result));
    assert(strstr(result,updated) && strstr(result,"\"editable\":true"));
    strcpy(call.name,"edit_text");strcpy(call.arguments,saved);assert(!run() && strstr(result,"REVISION_MISMATCH") && journals==4);
    i=edit_setup("aaa\r","aa","b");assert(!run() && strstr(result,"AMBIGUOUS_MATCH") && !creates && !journals && !strcmp(files[i].bytes,"aaa\r"));
    edit_setup("one\rone\r","one","two");assert(!run() && strstr(result,"AMBIGUOUS_MATCH") && !creates);
    edit_setup("one\r","missing","two");assert(!run() && strstr(result,"NO_MATCH") && !journals);
    edit_setup("one\r","one","one");assert(!run() && strstr(result,"NO_CHANGE") && !journals);
    edit_setup("one\r","","two");assert(!run() && strstr(result,"ENCODING_LIMIT") && !journals);
    edit_setup("one\r","one","\xf0\x9f\xa6\x80");assert(!run() && strstr(result,"ENCODING_LIMIT") && !journals);
    edit_setup("one\r","one","\177");assert(!run() && strstr(result,"NOT_TEXT") && !journals);
    edit_setup("one\r","one\n","");assert(!run() && strstr(result,"EDITED") && files[leaf("hello.c")].size==0);
    i=edit_setup("one\r","one","two");files[i].bytes[0]='x';assert(!run() && strstr(result,"REVISION_MISMATCH") && !creates && !journals);
    i=edit_setup("one\r","one","two");files[i].id++;assert(!run() && strstr(result,"REVISION_MISMATCH") && !creates);
    edit_setup("one\n","one","two");assert(!run() && strstr(result,"LINE_ENDINGS") && !creates);
    i=edit_setup("one\r","one","two");files[i].resource=1;assert(!run() && strstr(result,"NOT_TEXT") && !creates);
    i=edit_setup("one\r","one","two");files[i].info.fdFlags=0x8000;assert(!run() && strstr(result,"NOT_TEXT") && !creates);
    edit_setup("one\r","one","two");busy=1;assert(!run() && strstr(result,"BUSY") && !journals);
    edit_setup("one\r","one","two");assert(tools_execute_recorded(&call,result,sizeof(result),NULL,NULL) && strstr(result,"JOURNAL") && !creates);
    for(j=1;j<=4;j++) {
        i=edit_setup("one\r","one","two");fail_journal=j;assert(run());
        assert(j<3 ? leaf("hello.c")==i : files[i].used && !strcmp(files[i].bytes,"one\r"));
        assert(!opens[i]);assert(j==4 ? !strcmp(files[leaf("hello.c")].bytes,"two\r") : !strcmp(files[i].bytes,"one\r"));
    }
    i=edit_setup("one\r","one","two");short_write=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")==i && journals==1);
    i=edit_setup("one\r","one","two");stage_bad_read=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")==i);
    i=edit_setup("one\r","one","two");stage_short_read=1;assert(!run() && strstr(result,"STAGE_FAILED") && leaf("hello.c")==i);
    i=edit_setup("one\r","one","two");change_after_stage=1;assert(!run() && strstr(result,"CHANGED_STAGE_RETAINED") && leaf("hello.c")==i && journals==2);
    for(j=1;j<=2;j++) {
        i=edit_setup("one\r","one","two");rename_error=1;fault_rename=j;assert(run() && strstr(result,"uncertain"));
        assert(!strcmp(files[i].bytes,"one\r") && !opens[i]);assert(j==1 ? leaf("hello.c")==i : leaf("hello.c")<0);
        i=edit_setup("one\r","one","two");rename_race=1;fault_rename=j;assert(run() && strstr(result,"uncertain"));
        assert(!strcmp(files[i].bytes,"one\r"));assert(j==1 ? leaf("hello.c")==i : !strcmp(files[leaf("hello.c")].bytes,"racer"));
    }
    i=edit_setup("one\r","one","two");flush_error=1;assert(run() && strstr(result,"uncertain") && !strcmp(files[i].bytes,"one\r"));
    i=edit_setup("one\r","one","two");swapped_publish=1;assert(run() && strstr(result,"uncertain") && !strcmp(files[i].bytes,"one\r"));
    memset(large,'x',4096);large[4095]='z';large[4096]=0;
    i=edit_setup(large,"z","y");field("revision",revision,sizeof(revision));strcpy(saved,call.arguments);
    strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"start_byte\":3500}");tools_execute(&call,result,sizeof(result));assert(strstr(result,revision));
    strcpy(call.name,"edit_text");strcpy(call.arguments,saved);assert(!run() && files[leaf("hello.c")].size==4096 && files[leaf("hello.c")].bytes[4095]=='y');
    edit_setup(large,"z","yy");assert(!run() && strstr(result,"LIMIT") && !creates);
    i=edit_setup(large,"z","y");files[i].bytes[4096]='z';files[i].size=4097;assert(!run() && strstr(result,"LIMIT") && !creates);
    /* Guard includes bytes outside the displayed page, even at same size/date. */
    large[4096]=0;i=edit_setup(large,"z","y");files[i].bytes[3500]='a';assert(!run() && strstr(result,"REVISION_MISMATCH") && !creates);
    edit_setup("one\r","one","two");strcpy(call.arguments,"{\"path\":\"hello.c\",\"expected_revision\":\"scan-stale\",\"old_text\":\"one\",\"new_text\":\"two\"}");assert(!run() && strstr(result,"ARGUMENTS") && !creates);
    edit_setup("one\r","one","two");strcpy(call.arguments,"{\"path\":\"hello.c\",\"expected_revision\":\"full-x\",\"old_text\":\"one\",\"old_text\":\"two\",\"new_text\":\"x\"}");assert(!run() && strstr(result,"ARGUMENTS") && !creates);
    /* All three recovery paths must fit a result before creating any file. */
    i=edit_setup("one\r","one","two");field("revision",revision,sizeof(revision));
    { char path[512]="", name[32], quoted[1100];long parent=10;int d,k;
      for(d=0;d<6;d++) {
          int n=d==5 ? 7 : 31;
          memset(name,0xdb,(size_t)n);name[n]=0;k=add(parent,name,1);parent=files[k].id;
          for(k=0;k<n;k++)strcat(path,"\xe2\x82\xac");strcat(path,":");
      }
      files[i].parent=parent;strcat(path,"hello.c");assert(json_quote(path,quoted,sizeof(quoted))>0);
      snprintf(call.arguments,sizeof(call.arguments),"{\"path\":%s,\"expected_revision\":\"%s\",\"old_text\":\"one\",\"new_text\":\"two\"}",quoted,revision);
      assert(!run() && strstr(result,"LIMIT") && !creates && !journals && !strcmp(files[i].bytes,"one\r"));
    }
    puts("PASS exact edit: whole-file/page guards, unique matches, backups, locks, encoding, limits, journal barriers and publication faults");
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
    edit_checks();
    return 0;
}
