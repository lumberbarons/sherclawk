/* Exercise the actual native executor with a File Manager model. Faults verify
 * that journal barriers, short writes, corrupt reads and rename races cannot
 * silently overwrite an existing file or claim an uncertain create succeeded. */
#include "tools.h"
#include "view_image.h"
#include "json.h"
#include "build/project-template.h"
#include <Files.h>
#include <Resources.h>
#include <Aliases.h>
#include <Processes.h>
#include <Fonts.h>
#include <Quickdraw.h>
#include <QuickdrawText.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct File { int used, dir; long parent, id; char name[32], bytes[20000]; long size, resource; FInfo info;
    unsigned long crdat, mddat; } files[64];
static long positions[64];
static int dir_error, dir_leftover, dir_race;
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
        files[i].id=i+10; files[i].dir=dir; strcpy(files[i].name,name);
        files[i].crdat=1111; files[i].mddat=1234; return i;
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
    int i;
    if(pb->hFileInfo.ioFDirIndex>0) {
        int n=0;
        for(i=0;i<64;i++) if(files[i].used && files[i].parent==pb->hFileInfo.ioDirID && ++n==pb->hFileInfo.ioFDirIndex) break;
        if(i==64)return fnfErr;
        pb->hFileInfo.ioNamePtr[0]=(unsigned char)strlen(files[i].name);
        memcpy(pb->hFileInfo.ioNamePtr+1,files[i].name,strlen(files[i].name));
    } else i=find(pb->hFileInfo.ioDirID,pb->hFileInfo.ioNamePtr);
    if(i<0) return fnfErr;
    pb->hFileInfo.ioFlAttrib=files[i].dir ? 16 : 0;
    pb->hFileInfo.ioFlFndrInfo=files[i].info;
    pb->hFileInfo.ioFlRLgLen=files[i].resource; pb->hFileInfo.ioFlMdDat=files[i].mddat;
    pb->hFileInfo.ioFlCrDat=files[i].crdat; pb->hFileInfo.ioFlBkDat=files[i].mddat;
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
OSErr FSpDirCreate(const FSSpec *s, short script, long *id)
{
    char name[32]; int i; (void)script;
    if(dir_race) { memcpy(name,s->name+1,s->name[0]); name[s->name[0]]=0; add(s->parID,name,0); }
    if(find(s->parID,s->name)>=0) return dupFNErr;
    memcpy(name,s->name+1,s->name[0]); name[s->name[0]]=0;
    if(dir_error) { if(dir_leftover) add(s->parID,name,1); return ioErr; }
    i=add(s->parID,name,1); *id=files[i].id; creates++; return 0;
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
    assert(positions[ref]+*n<=(long)sizeof(files[ref].bytes)); memcpy(files[ref].bytes+positions[ref],in,(size_t)*n); positions[ref]+=*n; if(positions[ref]>files[ref].size)files[ref].size=positions[ref]; return 0;
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
    if((editing && renames==2 && swapped_publish) || (!strcmp(call.name,"create_project") && swapped_publish)) files[i].id++;
    return 0;
}
/* ---------- Read-only inspection model: resources, aliases, processes,
 * fonts and QuickDraw text state. A test alias record is one byte: index+1
 * selects a file and 0xFE means a target outside the workspace volume. ----- */
struct Resource { int used, file; ResType type; short id; unsigned char rname[256]; long size; unsigned char data[1024]; };
static struct Resource resources[16];
static short res_ref, res_error;
static struct TestProcess { unsigned long hi, lo; const char *name, *app; } processes[4];
static int process_count, process_self, process_front;
static struct TestFont { FMFontFamily id; const char *name; } test_fonts[8];
static int test_font_count;
static GrafPort test_port;
static int add_resource(int file, ResType type, short id, const char *name, const void *data, long size)
{
    int r; for(r=0;r<16;r++) if(!resources[r].used) {
        memset(&resources[r],0,sizeof(resources[r]));
        resources[r].used=1; resources[r].file=file; resources[r].type=type; resources[r].id=id;
        if(name){resources[r].rname[0]=(unsigned char)strlen(name);memcpy(resources[r].rname+1,name,resources[r].rname[0]);}
        resources[r].size=size; assert(size<=(long)sizeof(resources[r].data));
        if(size)memcpy(resources[r].data,data,(size_t)size);
        return r;
    }
    assert(0); return -1;
}
static int res_file(void) { return res_ref>=100 ? res_ref-100 : -1; }
static int res_index(int file, ResType type, short which, int by_id)
{
    int r, n=0;
    for(r=0;r<16;r++) if(resources[r].used && resources[r].file==file && resources[r].type==type) {
        if(by_id){ if(resources[r].id==which) return r; }
        else if(++n==which) return r;
    }
    return -1;
}
short FSpOpenResFile(const FSSpec *s, signed char permission)
{
    int i=find(s->parID,s->name), r, count=0;
    (void)permission; res_error=0;
    if(i<0){res_error=fnfErr;return fnfErr;}
    for(r=0;r<16;r++)if(resources[r].used && resources[r].file==i)count++;
    if(!count){res_error=resNotFound;return resNotFound;}
    return (short)(100+i);
}
short CurResFile(void) { return res_ref; }
void UseResFile(short ref) { res_ref=ref; }
void CloseResFile(short ref) { (void)ref; res_error=0; }
void SetResLoad(Boolean load) { (void)load; }
short ResError(void) { short e=res_error; res_error=0; return e; }
short Count1Types(void)
{
    int f=res_file(), r, k; short n=0;
    if(f<0)return 0;
    res_error=0;
    for(r=0;r<16;r++) if(resources[r].used && resources[r].file==f) {
        for(k=0;k<r;k++) if(resources[k].used && resources[k].file==f && resources[k].type==resources[r].type) break;
        if(k==r)n++;
    }
    return n;
}
void Get1IndType(ResType *type, short index)
{
    int f=res_file(), r, k; short n=0;
    *type=0; res_error=0;
    if(f<0){res_error=resNotFound;return;}
    for(r=0;r<16;r++) if(resources[r].used && resources[r].file==f) {
        for(k=0;k<r;k++) if(resources[k].used && resources[k].file==f && resources[k].type==resources[r].type) break;
        if(k!=r)continue;
        if(++n==index){*type=resources[r].type;return;}
    }
    res_error=resNotFound;
}
short Count1Resources(ResType type)
{
    int f=res_file(), r; short n=0;
    if(f<0)return 0;
    res_error=0;
    for(r=0;r<16;r++)if(resources[r].used && resources[r].file==f && resources[r].type==type)n++;
    return n;
}
Handle Get1IndResource(ResType type, short index)
{
    int f=res_file(), r; res_error=0;
    r=res_index(f,type,index,0);
    if(r<0){res_error=resNotFound;return (Handle)0;}
    return (Handle)&resources[r];
}
Handle Get1Resource(ResType type, short id)
{
    int f=res_file(), r; res_error=0;
    r=res_index(f,type,id,1);
    if(r<0){res_error=resNotFound;return (Handle)0;}
    return (Handle)&resources[r];
}
void GetResInfo(Handle h, short *id, ResType *type, Str255 name)
{
    struct Resource *r=(struct Resource *)h;
    res_error=0;
    if(!r){res_error=paramErr;return;}
    *id=r->id; *type=r->type;
    memcpy(name,r->rname,(size_t)r->rname[0]+1);
}
long GetResourceSizeOnDisk(Handle h)
{
    struct Resource *r=(struct Resource *)h;
    res_error=0; return r ? r->size : -1;
}
void ReadPartialResource(Handle h, long offset, void *buffer, long count)
{
    struct Resource *r=(struct Resource *)h;
    res_error=0;
    if(!r || offset<0 || count<0 || offset+count>r->size){res_error=paramErr;return;}
    memcpy(buffer,r->data+offset,(size_t)count);
}
void ReleaseResource(Handle h) { (void)h; res_error=0; }
OSErr PtrToHand(const void *source, Handle *destination, Size count)
{
    unsigned char *block=malloc((size_t)count+1);
    if(!block)return -108;
    memcpy(block,source,(size_t)count); *destination=block; return 0;
}
void DisposeHandle(Handle value) { free(value); }
OSErr ResolveAlias(const FSSpec *from, AliasHandle alias, FSSpec *target, Boolean *changed)
{
    unsigned char *record=(unsigned char *)alias;
    (void)from; *changed=0;
    if(!record)return paramErr;
    if(record[0]==0xFE){target->vRefNum=2;target->parID=99;target->name[0]=8;memcpy(target->name+1,"External",8);return 0;}
    if(record[0]==0 || record[0]>64)return -1;
    { int i=record[0]-1; if(!files[i].used)return -1;
      target->vRefNum=1; target->parID=files[i].parent;
      target->name[0]=(unsigned char)strlen(files[i].name);
      memcpy(target->name+1,files[i].name,target->name[0]); }
    return 0;
}
OSErr GetNextProcess(ProcessSerialNumber *psn)
{
    int i;
    if(!psn->highLongOfPSN && !psn->lowLongOfPSN) {
        if(process_count<1)return procNotFound;
        psn->highLongOfPSN=processes[0].hi; psn->lowLongOfPSN=processes[0].lo; return 0;
    }
    for(i=0;i<process_count;i++)if(processes[i].hi==psn->highLongOfPSN && processes[i].lo==psn->lowLongOfPSN)break;
    if(i>=process_count-1)return procNotFound;
    psn->highLongOfPSN=processes[i+1].hi; psn->lowLongOfPSN=processes[i+1].lo; return 0;
}
OSErr GetFrontProcess(ProcessSerialNumber *psn)
{
    if(process_count<1)return procNotFound;
    psn->highLongOfPSN=processes[process_front].hi; psn->lowLongOfPSN=processes[process_front].lo; return 0;
}
OSErr GetCurrentProcess(ProcessSerialNumber *psn)
{
    if(process_count<1)return procNotFound;
    psn->highLongOfPSN=processes[process_self].hi; psn->lowLongOfPSN=processes[process_self].lo; return 0;
}
#ifndef TEST_EXTERNAL_PROCESS_INFO
OSErr GetProcessInformation(const ProcessSerialNumber *psn, ProcessInfoRec *info)
{
    int i;
    for(i=0;i<process_count;i++)if(processes[i].hi==psn->highLongOfPSN && processes[i].lo==psn->lowLongOfPSN)break;
    if(i>=process_count)return procNotFound;
    info->processName[0]=(unsigned char)strlen(processes[i].name);
    memcpy(info->processName+1,processes[i].name,info->processName[0]);
    if(info->processAppSpec) {
        if(processes[i].app){
            info->processAppSpec->name[0]=(unsigned char)strlen(processes[i].app);
            memcpy(info->processAppSpec->name+1,processes[i].app,info->processAppSpec->name[0]);
        } else info->processAppSpec->name[0]=0;
    }
    return 0;
}
#endif
void GetPort(GrafPtr *port) { *port=&test_port; }
void TextFont(short font) { test_port.txFont=font; }
void TextSize(short size) { test_port.txSize=size; }
void TextFace(StyleParameter face) { test_port.txFace=(Style)face; }
short TextWidth(const void *text, short first, short count)
{
    (void)text;
    if(count<=first)return 0;
    return (short)((count-first)*(test_port.txSize/2+1));
}
void GetFontInfo(FontInfo *info)
{
    info->ascent=(short)(test_port.txSize-3); info->descent=3;
    info->widMax=test_port.txSize; info->leading=1;
}
OSStatus FMCreateFontFamilyIterator(const FMFilter *filter, void *ref, OptionBits options, FMFontFamilyIterator *iterator)
{ (void)filter;(void)ref;(void)options; memset(iterator,0,sizeof(*iterator)); return 0; }
OSStatus FMGetNextFontFamily(FMFontFamilyIterator *iterator, FMFontFamily *family)
{
    unsigned long i=iterator->reserved[0];
    if(i>=(unsigned long)test_font_count)return -1;
    *family=test_fonts[i].id; iterator->reserved[0]++; return 0;
}
OSStatus FMDisposeFontFamilyIterator(FMFontFamilyIterator *iterator) { (void)iterator; return 0; }
OSStatus FMGetFontFamilyName(FMFontFamily family, Str255 name)
{
    int i;
    for(i=0;i<test_font_count;i++)if(test_fonts[i].id==family){
        name[0]=(unsigned char)strlen(test_fonts[i].name);
        memcpy(name+1,test_fonts[i].name,name[0]); return 0;
    }
    name[0]=0; return -1;
}
FMFontFamily FMGetFontFamilyFromName(const unsigned char *name)
{
    int i;
    for(i=0;i<test_font_count;i++)
        if((unsigned char)strlen(test_fonts[i].name)==name[0] && !memcmp(test_fonts[i].name,name+1,name[0]))
            return test_fonts[i].id;
    return -1;
}
long FreeMem(void) { return 100000; }
short Gestalt(long selector, long *v) { (void)selector;*v=0x922;return 0; }
unsigned long TickCount(void) { return 42; }
static int journal(void *ctx, const char *event, const char *json)
{
    JsonToken tokens[128]; (void)ctx;
    assert(json_parse(json,strlen(json),tokens,128)>0);
    journals++; assert(strstr(json,"\"call_id\":\"write1\""));
    if(!strcmp(call.name,"create_project")) {
        assert(journals==1 ? !strcmp(event,"mutation_intent") && !creates :
               journals==2 ? !strcmp(event,"mutation_staged") && creates==4 && !published :
               journals==3 && !strcmp(event,"mutation_committed") && published);
        return fail_journal==journals ? -1 : 0;
    }
    if(!strcmp(call.name,"create_folder")) {
        assert(journals==1 ? !strcmp(event,"mutation_intent") && !creates : !strcmp(event,"mutation_committed") && creates);
        return fail_journal==journals ? -1 : 0;
    }
    { char temp[768]; int mutation=json_member(json,tokens,0,"mutation");
      int n=json_string(json,tokens,json_member(json,tokens,mutation,"temporary_path"),temp,sizeof(temp));
      assert(n>=0);if((size_t)n>longest_temporary)longest_temporary=(size_t)n; }
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
    memset(resources,0,sizeof(resources)); res_ref=0; res_error=0;
    dir_error=dir_leftover=dir_race=0;
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
          for(k=0;k<n;k++)strcat(path,"\xe2\x82\xac");
          strcat(path,":");
      }
      files[i].parent=parent;strcat(path,"hello.c");assert(json_quote(path,quoted,sizeof(quoted))>0);
      snprintf(call.arguments,sizeof(call.arguments),"{\"path\":%s,\"expected_revision\":\"%s\",\"old_text\":\"one\",\"new_text\":\"two\"}",quoted,revision);
      assert(!run() && strstr(result,"LIMIT") && !creates && !journals && !strcmp(files[i].bytes,"one\r"));
    }
    puts("PASS exact edit: whole-file/page guards, unique matches, backups, locks, encoding, limits, journal barriers and publication faults");
}
static void search_checks(void)
{
    int a,b,d,pages=0,total=0;
    char cursor[256], saved[512];
    JsonToken tokens[128];
    reset();d=add(10,"Sources",1);a=add(files[d].id,"hello.c",0);
    strcpy(files[a].bytes,"first\r\ncaf\216 lobster\rlobster again\n");files[a].size=(long)strlen(files[a].bytes);files[a].info.fdType='TEXT';
    b=add(10,"binary.c",0);memcpy(files[b].bytes,"lobster\0",8);files[b].size=8;
    strcpy(call.name,"search_text");strcpy(call.arguments,"{\"root\":\"\",\"query\":\"lobster\"}");tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"matches\":[]") && strstr(result,"\"truncated\":false"));
    strcpy(saved,"{\"root\":\"\",\"query\":\"lobster\",\"recursive\":true,\"limit\":1");
    strcpy(call.arguments,saved);strcat(call.arguments,"}");
    do {
        tools_execute(&call,result,sizeof(result));assert(json_parse(result,strlen(result),tokens,128)>0);
        assert(!strstr(result,"\"status\":\"error\""));
        if(strstr(result,"Sources:hello.c")){total++;assert(strstr(result,total==1 ? "\"line\":2" : "\"line\":3"));}
        if(strstr(result,"\"truncated\":false"))break;
        field("next_cursor",cursor,sizeof(cursor));snprintf(call.arguments,sizeof(call.arguments),"%s,\"cursor\":\"%s\"}",saved,cursor);
        assert(++pages<10);
    } while(1);
    assert(total==2);
    strcpy(call.arguments,"{\"root\":\"Sources:\",\"query\":\"lobster\",\"cursor\":\"\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"Sources:hello.c") && strstr(result,"\"line\":2"));
    files[d].info.fdFlags=0x8000;strcpy(call.arguments,"{\"root\":\"Sources\",\"query\":\"lobster\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"FOLDER"));
    files[d].info.fdFlags=0;
    reset();a=add(10,"large.c",0);memset(files[a].bytes,'x',10000);memcpy(files[a].bytes+4094,"lobster",7);files[a].size=10000;
    strcpy(call.name,"search_text");strcpy(call.arguments,"{\"root\":\"\",\"query\":\"lobster\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"byte\":4094") && strstr(result,"\"truncated\":true"));
    field("next_cursor",cursor,sizeof(cursor));snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"\",\"query\":\"lobster\",\"cursor\":\"%s\"}",cursor);tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"matches\":[]") && strstr(result,"\"truncated\":false"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"x\",\"cursor\":\"9:1:0:1:0\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"x\",\"query\":\"y\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"x\",\"cursor\":\"0:1:999999999999999999999:1:0\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"x\",\"recursive\":1}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    reset();a=add(10,"hello.c",0);strcpy(files[a].bytes,"aaaa\r\ncaf\216");files[a].size=(long)strlen(files[a].bytes);
    strcpy(call.name,"search_text");strcpy(call.arguments,"{\"root\":\"\",\"query\":\"aa\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"byte\":0") && strstr(result,"\"byte\":1") && strstr(result,"\"byte\":2"));
    strcpy(call.arguments,"{\"root\":\"\",\"query\":\"caf\\u00e9\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"line\":2"));
    files[a].resource=1;tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"matches\":[]") && strstr(result,"\"skipped\":1"));
    files[a].resource=0;bad_close=1;tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"code\":\"READ\""));
    puts("PASS search: recursion, pagination, absolute CR/CRLF lines, binary and alias refusal, chunk-boundary match and bounded continuation");
}
static void folder_checks(void)
{
    int i;
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");
    assert(!run() && strstr(result,"CREATED_FOLDER") && journals==2 && creates==1);
    i=leaf("src");assert(i>=0 && files[i].dir);
    strcpy(call.arguments,"{\"path\":\"src:inner\"}");journals=creates=0;assert(!run() && strstr(result,"CREATED_FOLDER") && files[i+1].used && !strcmp(files[i+1].name,"inner") && files[i+1].parent==files[i].id && files[i+1].dir);
    strcpy(call.arguments,"{\"path\":\"src\"}");journals=0;assert(!run() && strstr(result,"EXISTS") && !journals);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"hello.c\"}");add(10,"hello.c",0);
    assert(!run() && strstr(result,"EXISTS") && !creates && !journals);
    { const char *bad[]={"{\"path\":\"missing:inner\"}","{\"path\":\":escape\"}","{\"path\":\"src:\"}","{\"path\":\"\"}",
        "{}","{\"path\":\"a\",\"path\":\"b\"}","{\"path\":\"a\",\"mode\":\"x\"}","{\"path\":\"\\ud83e\\udd80\"}"};
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){reset();strcpy(call.name,"create_folder");strcpy(call.arguments,bad[i]);
        assert(!run() && strstr(result,"error") && !creates && !journals);} }
    reset();i=add(10,"alias",1);files[i].info.fdFlags=0x8000;strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"alias:x\"}");
    assert(!run() && !creates && !journals);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"JOURNAL") && !creates);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");fail_journal=1;assert(run() && !creates && leaf("src")<0);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");fail_journal=2;
    assert(run() && strstr(result,"JOURNAL_AFTER_PUBLISH") && strstr(result,"uncertain") && leaf("src")>=0);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");dir_error=1;
    assert(!run() && strstr(result,"CREATE_FAILED") && leaf("src")<0 && journals==1);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");dir_error=dir_leftover=1;
    assert(run() && strstr(result,"CREATE_FOLDER_UNCERTAIN") && leaf("src")>=0);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");dir_race=1;
    assert(!run() && strstr(result,"EXISTS") && !creates);
    reset();strcpy(call.name,"create_folder");strcpy(call.arguments,"{\"path\":\"src\"}");flush_error=published=1;
    assert(run() && strstr(result,"CREATE_FOLDER_UNVERIFIED") && leaf("src")>=0);
    puts("PASS create_folder: create-only, nested levels, path/alias refusal, journal barriers, races and uncertain outcomes");
}
static void project_checks(void)
{
    int i, j, parent;
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");
    assert(!run() && strstr(result,"CREATED_PROJECT") && journals==3 && creates==4);
    parent=leaf("Project");assert(parent>=0 && files[parent].dir);
    for(i=0;i<3;i++) {
        unsigned char name[32];name[0]=(unsigned char)strlen(project_inputs[i].name);
        memcpy(name+1,project_inputs[i].name,name[0]);j=find(files[parent].id,name);
        assert(j>=0 && files[j].info.fdType=='TEXT' && files[j].info.fdCreator=='ttxt' && !files[j].resource);
        assert(files[j].size==(long)strlen(project_inputs[i].bytes) && !memcmp(files[j].bytes,project_inputs[i].bytes,(size_t)files[j].size));
        assert(!memchr(files[j].bytes,10,(size_t)files[j].size));
    }
    assert(!run() && strstr(result,"EXISTS") && creates==4 && journals==3);
    { const char *bad[]={"{}","{\"path\":\"Project\",\"template\":\"other\"}","{\"path\":\"a\",\"path\":\"b\"}",
        "{\"path\":\"missing:Project\"}","{\"path\":\":escape\"}","{\"path\":\"Project:\"}","{\"path\":\"\"}"};
      for(i=0;i<7;i++){reset();strcpy(call.name,"create_project");strcpy(call.arguments,bad[i]);assert(!run() && strstr(result,"error") && !creates && !journals);} }
    reset();i=add(10,"alias",1);files[i].info.fdFlags=0x8000;strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"alias:Project\"}");assert(!run() && !creates);
    for(i=1;i<=3;i++) {
        reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");fail_journal=i;
        assert(run());assert(i==3 ? leaf("Project")>=0 && strstr(result,"uncertain") : leaf("Project")<0);
        assert(i==1 ? !creates : creates==4);
    }
    for(i=0;i<3;i++) {
        reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");
        if(i==0)short_write=1;else if(i==1)bad_read=1;else bad_close=1;
        assert(run() && strstr(result,"PROJECT_STAGE_RETAINED") && leaf("Project")<0 && journals==1);
    }
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");rename_race=1;
    assert(run() && strstr(result,"EXISTS_STAGE_RETAINED"));i=leaf("Project");assert(i>=0 && !files[i].dir && !strcmp(files[i].bytes,"racer"));
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");rename_error=1;
    assert(run() && strstr(result,"uncertain") && leaf("Project")<0);
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");flush_error=1;
    assert(run() && strstr(result,"uncertain") && leaf("Project")>=0);
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");swapped_publish=1;
    assert(run() && strstr(result,"uncertain") && leaf("Project")>=0);
    reset();i=add(10,"Parent",1);strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Parent:Project\"}");
    assert(!run() && strstr(result,"CREATED_PROJECT") && files[i+1].parent==files[i].id);
    reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");tools_execute(&call,result,sizeof(result));assert(strstr(result,"JOURNAL") && !creates);
    puts("PASS create_project: exact template, CR/TEXT, collisions, path/alias refusal, stage faults, journal barriers and uncertain publication");
}
/* Read-only inspection tools share this File Manager model: Finder identity,
 * formatted dates, process paging, font iteration and metrics, alias targets,
 * and bounded resource map/byte reads including 'STR ' and 'vers' decoding. */
static void inspect_checks(void)
{
    int i, dir, file, alias;
    char cursor[64];
    reset(); i=add(10,"hello.c",0);
    files[i].info.fdType='TEXT'; files[i].info.fdCreator='ttxt';
    files[i].info.fdFlags=0x2400; files[i].crdat=0; files[i].mddat=2082844800UL;
    strcpy(call.name,"get_file_info"); strcpy(call.arguments,"{\"path\":\"hello.c\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"kind\":\"file\"") && strstr(result,"\"file_type\":\"TEXT\""));
    assert(strstr(result,"\"creator\":\"ttxt\"") && strstr(result,"\"custom_icon\":true"));
    assert(strstr(result,"\"bundle\":true") && strstr(result,"\"invisible\":false") && strstr(result,"\"locked\":false"));
    assert(strstr(result,"\"created\":\"1904-01-01 00:00:00\"") && strstr(result,"\"modified\":\"1970-01-01 00:00:00\""));
    assert(strstr(result,"\"alias\":false") && strstr(result,"\"label\":0"));
    dir=add(10,"Folder",1); strcpy(call.arguments,"{\"path\":\"Folder\"}");
    tools_execute(&call,result,sizeof(result)); assert(strstr(result,"\"kind\":\"folder\""));
    strcpy(call.arguments,"{\"path\":\"missing.c\"}");
    tools_execute(&call,result,sizeof(result)); assert(strstr(result,"\"code\":\"FILE\""));
    strcpy(call.arguments,"{\"path\":\"hello.c\",\"path\":\"other\"}");
    tools_execute(&call,result,sizeof(result)); assert(strstr(result,"ARGUMENTS"));

    reset();
    process_count=3; process_self=1; process_front=0;
    processes[0].hi=0;processes[0].lo=1;processes[0].name="Finder";processes[0].app=NULL;
    processes[1].hi=0;processes[1].lo=2;processes[1].name="Sherclawk";processes[1].app=NULL;
    processes[2].hi=0;processes[2].lo=3;processes[2].name="Worker";processes[2].app="Worker01";
    strcpy(call.name,"list_processes");strcpy(call.arguments,"{\"limit\":2}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"name\":\"Finder\"")&&strstr(result,"\"front\":true"));
    assert(strstr(result,"\"name\":\"Sherclawk\"")&&strstr(result,"\"self\":true"));
    assert(strstr(result,"\"truncated\":true")&&strstr(result,"\"next_cursor\":\"00000000:00000002\""));
    strcpy(call.arguments,"{\"limit\":2,\"cursor\":\"00000000:00000002\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"name\":\"Worker\"")&&strstr(result,"\"app\":\"Worker01\""));
    assert(strstr(result,"\"truncated\":false")&&strstr(result,"\"next_cursor\":null"));
    strcpy(call.arguments,"{\"cursor\":\"bogus\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    process_count=0;

    reset();
    test_font_count=3;
    test_fonts[0].id=0;test_fonts[0].name="Chicago";
    test_fonts[1].id=3;test_fonts[1].name="Geneva";
    test_fonts[2].id=4;test_fonts[2].name="Monaco";
    test_port.txFont=0;test_port.txSize=12;test_port.txFace=0;
    strcpy(call.name,"list_fonts");strcpy(call.arguments,"{\"limit\":2}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"id\":0")&&strstr(result,"\"name\":\"Chicago\"")&&strstr(result,"\"id\":3"));
    assert(strstr(result,"\"truncated\":true")&&strstr(result,"\"next_cursor\":2"));
    strcpy(call.arguments,"{\"limit\":2,\"cursor\":2}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"id\":4")&&strstr(result,"\"name\":\"Monaco\"")&&strstr(result,"\"truncated\":false"));
    strcpy(call.name,"measure_text");
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font_id\":0,\"size\":12,\"style\":\"plain\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"font\":\"Chicago\"")&&strstr(result,"\"width\":21"));
    assert(strstr(result,"\"ascent\":9")&&strstr(result,"\"descent\":3")&&strstr(result,"\"leading\":1"));
    assert(strstr(result,"\"line_height\":13")&&strstr(result,"\"style_bits\":0"));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font\":\"Geneva\",\"style\":\"bold, italic\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"font_id\":3")&&strstr(result,"\"style\":\"bold,italic\"")&&strstr(result,"\"style_bits\":3"));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font\":\"Nope\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"NOT_FOUND"));
    strcpy(call.arguments,"{\"text\":\"ABC\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font\":\"Chicago\",\"font_id\":0}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"font_id\":0") && !strstr(result,"\"status\":\"error\""));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font\":\"\",\"font_id\":0}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"font_id\":0") && !strstr(result,"\"status\":\"error\""));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font\":\"Chicago\",\"font_id\":3}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"text\":\"ABC\",\"font_id\":0,\"style\":\"loud\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"text\":\"line\\nbreak\",\"font_id\":0}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"NOT_TEXT"));
    test_font_count=0;

    reset();
    file=add(10,"Target.c",0); alias=add(10,"Alias to Target",0);
    files[alias].info.fdType='alis';files[alias].info.fdFlags=0x8000;
    { unsigned char record=(unsigned char)(file+1);
      add_resource(alias,'alis',0,"",&record,1); files[alias].resource=1; }
    strcpy(call.name,"resolve_alias");strcpy(call.arguments,"{\"path\":\"Alias to Target\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"was_changed\":false")&&strstr(result,"\"target_name\":\"Target.c\""));
    assert(strstr(result,"\"target_kind\":\"file\"")&&strstr(result,"\"target_exists\":true"));
    assert(strstr(result,"\"relative_path\":\"Target.c\"")&&strstr(result,"\"outside_workspace\":false"));
    dir=add(10,"Sub",1); file=add(files[dir].id,"Deep.c",0); alias=add(10,"Deep Alias",0);
    files[alias].info.fdType='alis';files[alias].info.fdFlags=0x8000;
    { unsigned char record=(unsigned char)(file+1);
      add_resource(alias,'alis',0,"",&record,1); files[alias].resource=1; }
    strcpy(call.arguments,"{\"path\":\"Deep Alias\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"relative_path\":\"Sub:Deep.c\""));
    alias=add(10,"External Alias",0);files[alias].info.fdType='alis';files[alias].info.fdFlags=0x8000;
    { unsigned char record=0xFE;
      add_resource(alias,'alis',0,"",&record,1); files[alias].resource=1; }
    strcpy(call.arguments,"{\"path\":\"External Alias\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"target_name\":\"External\"")&&strstr(result,"\"relative_path\":null"));
    assert(strstr(result,"\"outside_workspace\":true"));
    alias=add(10,"Missing Record",0);files[alias].info.fdFlags=0x8000;
    { unsigned char record=1; add_resource(alias,'alis',1,"",&record,1);files[alias].resource=1; }
    strcpy(call.arguments,"{\"path\":\"Missing Record\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"code\":\"ALIAS\""));
    alias=add(10,"Empty Record",0);files[alias].info.fdFlags=0x8000;
    add_resource(alias,'alis',0,"",NULL,0);files[alias].resource=1;
    strcpy(call.arguments,"{\"path\":\"Empty Record\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ALIAS_LIMIT"));
    strcpy(call.arguments,"{\"path\":\"Target.c\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"NOT_ALIAS"));
    strcpy(call.arguments,"{\"path\":\"Alias to Target\",\"cursor\":\"x\"}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));

    reset();
    file=add(10,"Res.bin",0); files[file].info.fdType='TEST'; files[file].resource=400;
    add(10,"NoFork.c",0);
    { const unsigned char str_data[]={5,'H','e','l','l','o'};
      const unsigned char text_data[]="line one\rline two";
      const unsigned char test_data[]={1,0,127,255};
      const unsigned char vers_data[]={0x01,0x02,0x80,0x00,0x00,0x00,10,'1','.','0','.','2',' ','t','e','s','t',
          16,'b','u','i','l','t',' ','f','o','r',' ','c','h','e','c','k','s'};
      unsigned char blob[300]; int k; for(k=0;k<300;k++)blob[k]=(unsigned char)(k*7+3);
      add_resource(file,'STR ',128,"greeting",str_data,(long)sizeof(str_data));
      add_resource(file,'TEXT',129,"",text_data,(long)sizeof(text_data)-1);
      add_resource(file,'TEST',-2,"",test_data,(long)sizeof(test_data));
      add_resource(file,'BLOB',300,"long blob",blob,300);
      add_resource(file,'vers',1,"",vers_data,(long)sizeof(vers_data));
    }
    strcpy(call.name,"list_resources");strcpy(call.arguments,"{\"path\":\"Res.bin\",\"limit\":16}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"type\":\"STR \"")&&strstr(result,"\"type\":\"TEXT\"")&&strstr(result,"\"type\":\"vers\""));
    assert(strstr(result,"\"id\":128")&&strstr(result,"\"id\":-2")&&strstr(result,"\"id\":300"));
    assert(strstr(result,"\"name\":\"greeting\"")&&strstr(result,"\"bytes\":6"));
    assert(strstr(result,"\"types\":5")&&strstr(result,"\"truncated\":false")&&strstr(result,"\"next_cursor\":null"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"limit\":1}");
    { int pages=0,total=0;
      do { JsonToken tokens[128];
          tools_execute(&call,result,sizeof(result));
          assert(!strstr(result,"\"status\":\"error\""));
          total++;
          if(strstr(result,"\"truncated\":false"))break;
          assert(json_parse(result,strlen(result),tokens,128)>0);
          assert(json_string(result,tokens,json_member(result,tokens,0,"next_cursor"),cursor,sizeof(cursor))>0);
          snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"Res.bin\",\"limit\":1,\"cursor\":\"%s\"}",cursor);
          assert(++pages<8);
      } while(1);
      assert(total==5);
    }
    strcpy(call.name,"read_resource");
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"STR \",\"id\":128}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"format\":\"text\"")&&strstr(result,"\"text\":\"Hello\""));
    assert(strstr(result,"\"resource_bytes\":6")&&strstr(result,"\"content_bytes\":5")&&strstr(result,"\"truncated\":false"));
    /* Pascal length is one unsigned byte, including the high-bit range. */
    { unsigned char long_string[256]; int n;
      for(n=127;n<=255;n++) {
        JsonToken tokens[128]; char decoded[300];
        long_string[0]=(unsigned char)n;memset(long_string+1,'A',(size_t)n);
        long_string[1]='B';long_string[n]='Z';
        int r=add_resource(file,'STR ',130,"long",long_string,n+1);
        strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"STR \",\"id\":130,\"max_bytes\":256}");
        tools_execute(&call,result,sizeof(result));
        assert(json_parse(result,strlen(result),tokens,128)>0);
        assert(json_string(result,tokens,json_member(result,tokens,0,"text"),decoded,sizeof(decoded))==n);
        assert(decoded[0]=='B'&&decoded[n-1]=='Z');
        assert(strstr(result,"\"truncated\":false"));
        if(n==255) {
          strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"STR \",\"id\":130,\"start_byte\":128,\"max_bytes\":128}");
          tools_execute(&call,result,sizeof(result));
          assert(strstr(result,"\"content_bytes\":255")&&strstr(result,"\"next_byte\":255"));
          assert(json_parse(result,strlen(result),tokens,128)>0);
          assert(json_string(result,tokens,json_member(result,tokens,0,"text"),decoded,sizeof(decoded))==127);
          assert(decoded[126]=='Z');
        }
        resources[r].used=0;
      }
      { unsigned char malformed[]={5,'A'};
        int r=add_resource(file,'STR ',130,"",malformed,sizeof(malformed));
        strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"STR \",\"id\":130}");
        tools_execute(&call,result,sizeof(result));assert(strstr(result,"MALFORMED"));
        resources[r].used=0;
        r=add_resource(file,'STR ',130,"",NULL,0);
        tools_execute(&call,result,sizeof(result));assert(strstr(result,"MALFORMED"));
        resources[r].used=0;
        long_string[0]=0;r=add_resource(file,'STR ',130,"",long_string,1);
        tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"content_bytes\":0"));
        resources[r].used=0;
      }
    }
    { unsigned char version[]={0x12,0x23,0x20,0,0,0,1,'x',1,'y'};
      const unsigned char stages[]={0x20,0x40,0x60,0x80};
      const char *labels[]={"development","alpha","beta","release"};int k;
      for(k=0;k<4;k++) {
        char expected[64];int r;
        version[2]=stages[k];r=add_resource(file,'vers',2,"",version,sizeof(version));
        strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"vers\",\"id\":2}");
        tools_execute(&call,result,sizeof(result));
        assert(strstr(result,"\"version\":\"12.2.3\""));
        snprintf(expected,sizeof(expected),"\"stage\":\"%s\"",labels[k]);assert(strstr(result,expected));
        resources[r].used=0;
      }
    }
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"TEST\",\"id\":-2}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"format\":\"hex\"")&&strstr(result,"\"hex\":\"01007FFF\""));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"0x54455354\",\"id\":-2}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"format\":\"hex\"")&&strstr(result,"\"type\":\"TEST\""));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"TEXT\",\"id\":129}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"format\":\"text\"")&&strstr(result,"line one")&&strstr(result,"line two"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":0,\"max_bytes\":256}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"truncated\":true")&&strstr(result,"\"next_byte\":256"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":256,\"max_bytes\":256}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"truncated\":false")&&strstr(result,"\"next_byte\":300"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"vers\",\"id\":1}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"format\":\"vers\"")&&strstr(result,"\"version\":\"1.0.2\"")&&strstr(result,"\"stage\":\"release\""));
    assert(strstr(result,"\"short\":\"1.0.2 test\"")&&strstr(result,"\"long\":\"built for checks\""));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"STR \",\"id\":999}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"NOT_FOUND"));
    strcpy(call.arguments,"{\"path\":\"NoFork.c\",\"type\":\"STR \",\"id\":128}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"NO_RESOURCE_FORK"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"TOOLONG\",\"id\":1}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"ARGUMENTS"));
    strcpy(call.arguments,"{\"path\":\"Res.bin\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":301}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"RANGE"));
    puts("PASS read-only inspection: Finder identity, process paging, font metrics, alias targets, resource maps and bounded reads");
}
/* view_image: gating on the model's vision flag, argument and file
 * validation, bounded stepped reads that honor Stop, a deadline and
 * change detection, and the single-image hand-off to the agent. */
static void png_file(int index, long size, unsigned long width, unsigned long height)
{
    static const unsigned char signature[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    long i; unsigned char *b=(unsigned char *)files[index].bytes;
    assert(size<=(long)sizeof(files[index].bytes));
    for(i=0;i<size;i++) b[i]=(unsigned char)(i*31+7);
    memcpy(b,signature,8); b[8]=b[9]=b[10]=0; b[11]=13; memcpy(b+12,"IHDR",4);
    b[16]=(unsigned char)(width>>24);b[17]=(unsigned char)(width>>16);b[18]=(unsigned char)(width>>8);b[19]=(unsigned char)width;
    b[20]=(unsigned char)(height>>24);b[21]=(unsigned char)(height>>16);b[22]=(unsigned char)(height>>8);b[23]=(unsigned char)height;
    memcpy(b+size-12,"\0\0\0\0IEND\xAE\x42\x60\x82",12);
    files[index].size=size; files[index].info.fdType='PNGf';
}
static int view_begin(const char *arguments)
{
    AgentCall view; memset(&view,0,sizeof(view)); strcpy(view.id,"v1"); strcpy(view.name,"view_image");
    snprintf(view.arguments,sizeof(view.arguments),"%s",arguments);
    return view_image_begin(&view,result,sizeof(result),NULL,NULL,100);
}
static void view_checks(void)
{
    int i, steps;
    const AgentImage *image;
    const char *bad[]={"{}","{\"path\":\"s.png\",\"path\":\"s.png\"}","{\"path\":\"s.png\",\"more\":1}","{\"path\":7}","{\"path\":\"\"}","not json"};
    reset(); i=add(10,"shot.png",0); png_file(i,20000,390,150);
    view_image_reset();
    /* Fail closed: no flag and a "no" flag read nothing and attach nothing. */
    view_image_set_vision(AGENT_VISION_UNKNOWN);
    assert(!view_begin("{\"path\":\"shot.png\"}") && strstr(result,"VISION_UNKNOWN") && !view_image_take() && !view_image_held());
    view_image_set_vision(AGENT_VISION_NO);
    assert(!view_begin("{\"path\":\"shot.png\"}") && strstr(result,"VISION_UNSUPPORTED") && !view_image_take() && !opens[i]);
    view_image_set_vision(AGENT_VISION_YES);
    for(steps=0;steps<(int)(sizeof(bad)/sizeof(*bad));steps++) assert(!view_begin(bad[steps]) && strstr(result,"ARGUMENTS") && !view_image_held());
    /* A good PNG is read in two bounded steps and handed over exactly once. */
    assert(view_begin("{\"path\":\"shot.png\"}")==2 && !view_image_take());
    assert(view_image_step(result,sizeof(result),101,0)==2 && !view_image_take() && !opens[i]);
    assert(view_image_step(result,sizeof(result),102,0)==0 && strlen(result)<AGENT_RESULT_CAP);
    assert(strstr(result,"\"status\":\"ok\"") && strstr(result,"\"path\":\"shot.png\"") && strstr(result,"\"width\":390") &&
           strstr(result,"\"height\":150") && strstr(result,"\"bytes\":20000") && !strstr(result,"base64") && !opens[i]);
    image=view_image_take();
    assert(image && image->length==20000 && image->width==390 && image->height==150 && !strcmp(image->path,"shot.png"));
    assert(!memcmp(image->data,files[i].bytes,20000) && !view_image_take() && view_image_held()==image);
    /* A second look in the same round is refused rather than clobbering the first. */
    assert(view_begin("{\"path\":\"shot.png\"}")==2);
    assert(view_image_step(result,sizeof(result),101,0)==2 && view_image_step(result,sizeof(result),102,0)==0);
    assert(view_begin("{\"path\":\"shot.png\"}")==0 && strstr(result,"ONE_IMAGE_PER_ROUND") && view_image_held());
    assert(view_image_take());
    /* Unreadable inputs: nothing is held after any of them. */
    assert(!view_begin("{\"path\":\"missing.png\"}") && strstr(result,"\"code\":\"FILE\"") && !view_image_held());
    assert(!view_begin("{\"path\":\":shot.png\"}") && strstr(result,"\"code\":\"FILE\""));
    add(10,"Folder",1); assert(!view_begin("{\"path\":\"Folder\"}") && strstr(result,"NOT_PNG"));
    i=add(10,"alias.png",0); png_file(i,200,4,4); files[i].info.fdFlags=0x8000;
    assert(!view_begin("{\"path\":\"alias.png\"}") && strstr(result,"NOT_PNG"));
    i=add(10,"tiny.png",0); png_file(i,40,4,4);
    assert(!view_begin("{\"path\":\"tiny.png\"}") && strstr(result,"NOT_PNG") && strstr(result,"too short"));
    i=add(10,"text.png",0); png_file(i,100,4,4); files[i].bytes[1]='X';
    assert(!view_begin("{\"path\":\"text.png\"}") && strstr(result,"NOT_PNG") && strstr(result,"signature"));
    i=add(10,"zero.png",0); png_file(i,100,0,4);
    assert(!view_begin("{\"path\":\"zero.png\"}") && strstr(result,"NOT_PNG") && strstr(result,"dimensions"));
    i=add(10,"huge.png",0); png_file(i,100,8193,4);
    assert(!view_begin("{\"path\":\"huge.png\"}") && strstr(result,"NOT_PNG") && strstr(result,"dimensions"));
    i=add(10,"wide.png",0); png_file(i,100,8192,8192);
    assert(view_begin("{\"path\":\"wide.png\"}")==2 && view_image_step(result,sizeof(result),100,1)==0 && strstr(result,"STOPPED"));
    /* Size: one byte over the cap is refused before any read; the cap itself is
     * accepted, and Stop abandons it without attaching. */
    i=add(10,"big.png",0); png_file(i,100,4,4); files[i].size=AGENT_IMAGE_CAP+1;
    assert(!view_begin("{\"path\":\"big.png\"}") && strstr(result,"TOO_LARGE") && strstr(result,"exceeds 131072 bytes") && !view_image_held());
    files[i].size=AGENT_IMAGE_CAP;
    assert(view_begin("{\"path\":\"big.png\"}")==2 && view_image_held() && view_image_held()->length==(size_t)AGENT_IMAGE_CAP);
    assert(view_image_step(result,sizeof(result),101,1)==0 && strstr(result,"STOPPED") && !view_image_held() && !view_image_take());
    assert(view_image_step(result,sizeof(result),102,0)==0 && strstr(result,"NO_ACTIVE_VIEW"));
    /* Deadline, mid-read change and an unfinished PNG all abandon the image. */
    i=leaf("shot.png");
    assert(view_begin("{\"path\":\"shot.png\"}")==2);
    assert(view_image_step(result,sizeof(result),100+60*60,0)==0 && strstr(result,"TIMEOUT") && !view_image_held());
    assert(view_begin("{\"path\":\"shot.png\"}")==2 && view_image_step(result,sizeof(result),101,0)==2);
    files[i].mddat++;
    assert(view_image_step(result,sizeof(result),102,0)==0 && strstr(result,"CHANGED") && !view_image_take() && !view_image_held());
    files[i].mddat--; files[i].bytes[19999]^=1;
    assert(view_begin("{\"path\":\"shot.png\"}")==2 && view_image_step(result,sizeof(result),101,0)==2);
    assert(view_image_step(result,sizeof(result),102,0)==0 && strstr(result,"NOT_PNG") && strstr(result,"IEND") && !view_image_take());
    puts("PASS view_image: vision gating, argument and file checks, bounded stepped reads, Stop, deadline, change detection and single hand-off");
}
int main(void)
{
    project_checks();
    search_checks();
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
    folder_checks();
    puts("PASS native executor: create/read/collision, encoding, bounds, journal barriers, I/O faults, rename races and uncertain outcomes");
    edit_checks();
    inspect_checks();
    view_checks();
    return 0;
}
