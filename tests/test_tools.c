/* Exercise the actual native executor with a File Manager model. Faults verify
 * that journal barriers, short writes, corrupt reads and rename races cannot
 * silently overwrite an existing file or claim an uncertain create succeeded. */
#include "tools.h"
#include "view_image.h"
#include "json.h"
#include "text.h"
#include "build/project-template.h"
/* One folder plus every embedded project file. */
#define PROJECT_FILES ((int)(sizeof(project_inputs)/sizeof(project_inputs[0])))
#define PROJECT_CREATES (1+PROJECT_FILES)
#include <Files.h>
#include <Folders.h>
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
#include "stepped-tools.h"
static struct File { int used, dir; long parent, id; char name[32], bytes[65538]; long size, resource; FInfo info;
    unsigned long crdat, mddat; int locked; } files[64];
static long positions[64];
static int dir_error, dir_leftover, dir_race, touch_on_read;
static int short_write, bad_read, bad_close, rename_race, rename_error, published, flush_error;
static int journals, fail_journal, creates;
static int editing, renames, fault_rename, change_after_stage, stage_bad_read, stage_short_read, busy, swapped_publish;
static int trash_can, trash_vref, trash_moves, trash_intents, trash_commits, rename_lost;
static int opens[64];
static int io_reads,io_writes,io_closes,io_flushes,io_opens;
static int fault_read,fault_write,fault_close,fault_flush,fault_open;
static long largest_transfer;
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
    if (parent && !name[0]) { /* the directory itself, like the real File Manager */
        int k; for(k=0;k<64;k++) if(files[k].used && files[k].dir && files[k].id==parent) break;
        if(k==64) return dirNFErr;
        spec->parID=files[k].parent; spec->name[0]=(unsigned char)strlen(files[k].name);
        memcpy(spec->name+1,files[k].name,spec->name[0]); return 0;
    }
    if (!parent) {
        assert(name[0]>=8 && !memcmp(name+1,"Retro68:",8));
        if (name[0]==8) { parent=1;name=(const unsigned char *)"\007Retro68"; }
        else {
            /* Walk colon-separated folders below the workspace; a missing or
             * non-folder component fails like the real File Manager. */
            const unsigned char *rest=name+9,*end=name+1+name[0],*colon;
            parent=10;
            while((colon=memchr(rest,':',(size_t)(end-rest)))) {
                unsigned char part[32]; int k;
                part[0]=(unsigned char)(colon-rest); memcpy(part+1,rest,part[0]);
                k=find(parent,part);
                if(k<0) return dirNFErr;
                if(!files[k].dir) return dirNFErr;
                parent=files[k].id; rest=colon+1;
            }
            leafname[0]=(unsigned char)(end-rest);memcpy(leafname+1,rest,leafname[0]);name=leafname;
        }
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
    pb->hFileInfo.ioFlAttrib=(unsigned char)(files[i].dir ? 16 : 0);
    if(files[i].locked) pb->hFileInfo.ioFlAttrib|=1;
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
    if(++io_opens==fault_open)return ioErr;
    if((mode==fsRdWrPerm && (busy || opens[i])) || opens[i]==fsRdWrPerm)return ioErr;
    opens[i]=mode; *ref=(short)i; positions[i]=0; return 0;
}
OSErr FSRead(short ref, long *n, void *out)
{
    if(*n>largest_transfer)largest_transfer=*n;
    if(++io_reads==fault_read)return ioErr;
    if(*n>files[ref].size-positions[ref]) *n=files[ref].size-positions[ref];
    if(stage_short_read && *n && !strncmp(files[ref].name,"Sherclawk tmp",13)) --*n;
    memcpy(out,files[ref].bytes+positions[ref],(size_t)*n); positions[ref]+=*n;
    if(touch_on_read) files[ref].mddat++;
    if((bad_read || (stage_bad_read && !strncmp(files[ref].name,"Sherclawk tmp",13))) && *n) ((char *)out)[0]^=1;
    return 0;
}
OSErr FSWrite(short ref, long *n, const void *in)
{
    if(*n>largest_transfer)largest_transfer=*n;
    if(++io_writes==fault_write)return ioErr;
    if(short_write && *n) --*n;
    assert(positions[ref]+*n<=(long)sizeof(files[ref].bytes)); memcpy(files[ref].bytes+positions[ref],in,(size_t)*n); positions[ref]+=*n; if(positions[ref]>files[ref].size)files[ref].size=positions[ref]; return 0;
}
OSErr FSClose(short ref) { opens[ref]=0; return bad_close || ++io_closes==fault_close ? ioErr : 0; }
OSErr SetFPos(short ref, short mode, long pos) { (void)mode; positions[ref]=pos; return 0; }
OSErr FlushVol(const unsigned char *name, short vol) { (void)name;(void)vol;return (published && flush_error) || ++io_flushes==fault_flush ? ioErr : 0; }
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
/* The Folder Manager model: the volume Trash (index of the folder FindFolder
 * resolves, or -1 for none; trash_vref != 1 models another volume) and the
 * Preferences folder, named by directory ID. prefs_dir defaults to an ID that
 * holds nothing in the workspace; a test points it at a workspace folder, or
 * sets prefs_error to model a lookup failure. */
static long prefs_dir; static int prefs_error, prefs_lookups;
OSErr FindFolder(short vRefNum, unsigned long folderType, Boolean createFolder, short *found_vol, long *found_dir)
{
    (void)vRefNum; (void)createFolder;
    if(folderType==(unsigned long)kPreferencesFolderType) {
        prefs_lookups++;
        if(prefs_error)return prefs_error;
        *found_vol=1; *found_dir=prefs_dir; return 0;
    }
    if(folderType!=(unsigned long)kTrashFolderType)return paramErr;
    if(trash_can<0 || !files[trash_can].used || !files[trash_can].dir)return fnfErr;
    *found_vol=(short)trash_vref;
    *found_dir=files[trash_can].id;
    return 0;
}
/* dest names the destination DIRECTORY (its own name and parent), as the real
 * call does; the moved file keeps its name. */
OSErr FSpCatMove(const FSSpec *source, const FSSpec *dest)
{
    int i=find(source->parID,source->name), d=find(dest->parID,dest->name);
    if(i<0)return fnfErr;
    if(d<0 || !files[d].dir)return fnfErr;
    renames++;
    if(find(files[d].id,source->name)>=0)return dupFNErr;
    if(rename_error && (!fault_rename || fault_rename==renames))return ioErr;
    files[i].parent=files[d].id;
    published=1; trash_moves++;
    if(rename_lost && (!fault_rename || fault_rename==renames))return ioErr;
    return 0;
}
/* ---------- Read-only inspection model: resources, aliases, processes,
 * fonts and QuickDraw text state. A test alias record is one byte: index+1
 * selects a file and 0xFE means a target outside the workspace volume. ----- */
struct Resource { int used, file; ResType type; short id; unsigned char rname[256]; long size; unsigned char data[1024]; };
static struct Resource resources[16];
static short res_ref, res_error;
static struct TestProcess { unsigned long hi, lo; const char *name, *app; } processes[260];
static int process_count, process_self, process_front, process_enumeration_error;
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
    if(process_enumeration_error)return ioErr;
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
               journals==2 ? !strcmp(event,"mutation_staged") && creates==PROJECT_CREATES && !published :
               journals==3 && !strcmp(event,"mutation_committed") && published);
        return fail_journal==journals ? -1 : 0;
    }
    if(!strcmp(call.name,"create_folder")) {
        assert(journals==1 ? !strcmp(event,"mutation_intent") && !creates : !strcmp(event,"mutation_committed") && creates);
        return fail_journal==journals ? -1 : 0;
    }
    if(!strcmp(call.name,"move_to_trash")) {
        char kind[24], status[16];
        int mutation=json_member(json,tokens,0,"mutation");
        int ki=json_member(json,tokens,mutation,"kind"), si=json_member(json,tokens,mutation,"status");
        assert(mutation>=0 && ki>=0 && si>=0);
        assert(json_string(json,tokens,ki,kind,sizeof(kind))>=0);
        assert(json_string(json,tokens,si,status,sizeof(status))>=0);
        if(!strcmp(kind,"trash_folder")) {
            assert(!strcmp(event,!strcmp(status,"pending") ? "mutation_intent" : "mutation_committed"));
            assert(trash_moves==0);
        } else {
            assert(!strcmp(kind,"move_to_trash"));
            if(!strcmp(event,"mutation_intent")) {
                assert(!strcmp(status,"pending") && trash_intents==trash_commits);
                trash_intents++;
            } else {
                assert(!strcmp(event,"mutation_committed") && !strcmp(status,"ok") && trash_intents==trash_commits+1);
                trash_commits++;
            }
        }
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
    dir_error=dir_leftover=dir_race=touch_on_read=0;
    short_write=bad_read=bad_close=rename_race=rename_error=published=flush_error=0;
    journals=fail_journal=creates=0;longest_temporary=0;
    editing=renames=fault_rename=change_after_stage=stage_bad_read=stage_short_read=busy=swapped_publish=0;
    prefs_dir=5000;prefs_error=0;prefs_lookups=0;
    trash_can=-1;trash_vref=1;trash_moves=trash_intents=trash_commits=0;rename_lost=0;
    io_reads=io_writes=io_closes=io_flushes=io_opens=0;
    fault_read=fault_write=fault_close=fault_flush=fault_open=0;largest_transfer=0;
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
static void page_checks(void)
{
    static char decoded[AGENT_RESULT_CAP], rebuilt[16385],expected[16385];
    const unsigned char kinds[] = {'x', '"', '\\', '\t', '\r', 0xdb};
    int k;
    for (k=0;k<(int)sizeof(kinds);k++) {
        long cursor=0; int pages=0, f;size_t reconstructed=0;
        reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';
        memset(files[f].bytes,kinds[k],4096);files[f].size=4096;
        if(k==0)for(int line_end=127;line_end<4096;line_end+=128)files[f].bytes[line_end]='\r';
        while(cursor<4096) {
            JsonToken t[128];int n;long next;
            strcpy(call.name,"read_text");
            snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"hello.c\",\"start_byte\":%ld,\"max_lines\":30}",cursor);
            tools_execute(&call,result,sizeof(result));
            assert(strlen(result)<AGENT_RESULT_CAP && json_parse(result,strlen(result),t,128)>0);
            n=json_string(result,t,json_member(result,t,0,"text"),decoded,sizeof(decoded));assert(n>0);
            next=strtol(result+t[json_member(result,t,0,"next_byte")].start,NULL,10);
            assert(next>cursor && next<=4096);
            if(k==0)assert(n==next-cursor);
            assert(reconstructed+(size_t)n<sizeof(rebuilt));memcpy(rebuilt+reconstructed,decoded,(size_t)n);reconstructed+=(size_t)n;
            cursor=next;assert(++pages<200);
        }
        if(k==0)assert(pages==2);
        assert(text_to_utf8(files[f].bytes,4096,expected,sizeof(expected))==(int)reconstructed);
        assert(!memcmp(rebuilt,expected,reconstructed));
    }
    /* Page endings never divide CRLF, including when max_lines is reached. */
    reset();k=add(10,"hello.c",0);files[k].info.fdType='TEXT';
    strcpy(files[k].bytes,"a\r\nb\r\n");files[k].size=6;
    strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"max_lines\":1}");
    tools_execute(&call,result,sizeof(result));field("text",decoded,sizeof(decoded));
    assert(!strcmp(decoded,"a\n\n"));assert(strstr(result,"\"next_byte\":3"));
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
    char backup[768], revision[80], updated[80], saved[AGENT_ARGUMENT_CAP];
    static char large[65538];
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
        if(j<3)assert(leaf("hello.c")==i);
        else{field("backup_path",backup,sizeof(backup));assert(leaf(backup)==i && !strcmp(files[i].bytes,"one\r"));}
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
    memset(large,'x',65536);large[65535]='z';large[65536]=0;
    i=edit_setup(large,"z","y");field("revision",revision,sizeof(revision));strcpy(saved,call.arguments);
    strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"start_byte\":3500}");tools_execute(&call,result,sizeof(result));assert(strstr(result,revision));
    strcpy(call.name,"edit_text");strcpy(call.arguments,saved);assert(!run() && files[leaf("hello.c")].size==65536 && files[leaf("hello.c")].bytes[65535]=='y');
    edit_setup(large,"z","yy");assert(!run() && strstr(result,"LIMIT") && !creates);
    i=edit_setup(large,"z","y");files[i].bytes[65536]='z';files[i].size=65537;assert(!run() && strstr(result,"LIMIT") && !creates);
    /* Guard includes bytes outside the displayed page, even at same size/date. */
    large[65536]=0;i=edit_setup(large,"z","y");files[i].bytes[3500]='a';assert(!run() && strstr(result,"REVISION_MISMATCH") && !creates);
    edit_setup("one\r","one","two");strcpy(call.arguments,"{\"path\":\"hello.c\",\"expected_revision\":\"scan-stale\",\"old_text\":\"one\",\"new_text\":\"two\"}");assert(!run() && strstr(result,"ARGUMENTS") && !creates);
    edit_setup("one\r","one","two");strcpy(call.arguments,"{\"path\":\"hello.c\",\"expected_revision\":\"full-x\",\"old_text\":\"one\",\"old_text\":\"two\",\"new_text\":\"x\"}");assert(!run() && strstr(result,"ARGUMENTS") && !creates);
    /* All three recovery paths must fit a result before creating any file. */
    i=edit_setup("one\r","one","two");field("revision",revision,sizeof(revision));
    /* The longest legal path (UTF-8 argument bound) no longer overflows the recovery
       record, which repeats it, now that a result holds AGENT_RESULT_CAP bytes. */
    { char path[512]="", name[32], quoted[1100];long parent=10;int d,k;
      for(d=0;d<6;d++) {
          int n=d==5 ? 7 : 31;
          memset(name,0xdb,(size_t)n);name[n]=0;k=add(parent,name,1);parent=files[k].id;
          for(k=0;k<n;k++)strcat(path,"\xe2\x82\xac");
          strcat(path,":");
      }
      files[i].parent=parent;strcat(path,"hello.c");assert(json_quote(path,quoted,sizeof(quoted))>0);
      snprintf(call.arguments,sizeof(call.arguments),"{\"path\":%s,\"expected_revision\":\"%s\",\"old_text\":\"one\",\"new_text\":\"two\"}",quoted,revision);
      assert(!run() && strstr(result,"\"status\":\"ok\"") && !strstr(result,"LIMIT"));
      { unsigned char hello[8]={7,'h','e','l','l','o','.','c'};int e=find(parent,hello);
        assert(e>=0 && !strcmp(files[e].bytes,"two\r")); }
    }
    puts("PASS exact edit: whole-file/page guards, unique matches, backups, locks, encoding, limits, journal barriers and publication faults");
}
static void handles_closed(void)
{ int k;for(k=0;k<64;k++)assert(!opens[k]); }
static void large_text_checks(void)
{
    static char source[65538],pattern[4097];
    const long sizes[]={0,4096,4097,16384,65536,65537};
    int k,f,r,step,limit;char revision[80];
    for(k=0;k<6;k++) {
        reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=sizes[k];
        memset(files[f].bytes,'x',(size_t)sizes[k]);
        strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");
        r=read_text_begin(&call,result,sizeof(result),NULL,NULL,0xfffffff0U);assert(r==2);
        { int busy_result=edit_text_begin(&call,result,sizeof(result),journal,NULL,0xfffffff0U);
          assert(busy_result==1 && strstr(result,"TEXT_BUSY")); }
        for(step=0;r==2;step++) { assert(step<200);r=read_text_step(result,sizeof(result),16U,0); }
        assert(!r && strstr(result,sizes[k]<=65536 ? "whole_file" : "scan"));
        handles_closed();
    }
    /* A change outside the page with unchanged size/date during verification. */
    reset();f=add(10,"hello.c",0);files[f].size=16384;files[f].info.fdType='TEXT';memset(files[f].bytes,'x',16384);
    strcpy(call.arguments,"{\"path\":\"hello.c\"}");
    assert(read_text_begin(&call,result,sizeof(result),NULL,NULL,1)==2);
    for(k=0;k<16;k++)assert(read_text_step(result,sizeof(result),2,0)==2);
    files[f].bytes[15000]='y';r=2;
    while(r==2)r=read_text_step(result,sizeof(result),2,0);
    assert(!r && strstr(result,"CHANGED"));handles_closed();
    for(k=1;k<=32;k++) {
        reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=16384;memset(files[f].bytes,'x',16384);
        fault_read=k;strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");
        assert(!run() && strstr(result,"\"code\":\"READ\""));handles_closed();
    }
    reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=16384;memset(files[f].bytes,'x',16384);
    fault_close=1;strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\"}");
    assert(run()==1 && strstr(result,"CLOSE"));handles_closed();
    /* Whole-snapshot line navigation beyond the old 8192-byte scan. */
    memset(source,'x',16384);source[12000]='\r';source[16383]='z';source[16384]=0;
    f=edit_setup(source,"z","y");strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"start_line\":2}");
    tools_execute(&call,result,sizeof(result));assert(strstr(result,"\"start_byte\":12001"));
    /* A maximum pattern and long repetitive fallback chains. */
    memset(pattern,'a',4096);pattern[4095]='b';pattern[4096]=0;
    memset(source,'a',16384);source[16383]='b';source[16384]=0;
    f=edit_setup(source,pattern,"z");assert(!run() && strstr(result,"EDITED"));
    assert(files[leaf("hello.c")].size==12289 && files[leaf("hello.c")].bytes[12288]=='z');handles_closed();
    /* Match straddles both the input and output chunk boundaries. */
    memset(source,'x',4097);memcpy(source+1022,"unique",6);source[4097]=0;
    f=edit_setup(source,"unique","");assert(!run() && files[leaf("hello.c")].size==4091);
    assert(!memcmp(files[f].bytes,source,4097));handles_closed();
    memset(source,'x',65536);memcpy(source,"unique",6);memcpy(source+65000,"unique",6);source[65536]=0;
    edit_setup(source,"unique","y");assert(!run() && strstr(result,"AMBIGUOUS_MATCH") && !creates);
    /* Stop and deadline at every observable phase, including tick wrap. */
    memset(source,'x',4097);source[1023]='z';source[4097]=0;
    edit_setup(source,"z","y");r=edit_text_begin(&call,result,sizeof(result),journal,NULL,0xfffffff0U);assert(r==2);
    r=2;limit=0;while(r==2) { r=edit_text_step(result,sizeof(result),16U,0);assert(++limit<100); }
    assert(!r);handles_closed();
    for(k=0;k<limit;k++)for(int timeout=0;timeout<2;timeout++) {
        f=edit_setup(source,"z","y");
        r=edit_text_begin(&call,result,sizeof(result),journal,NULL,0xfffffff0U);assert(r==2);
        for(step=0;step<k;step++)assert(edit_text_step(result,sizeof(result),16U,0)==2);
        assert(edit_text_step(result,sizeof(result),timeout ? 0xfffffff0U+3600U : 16U,!timeout)==1);
        assert(strstr(result,renames ? "uncertain" : "error"));
        assert(!memcmp(files[f].bytes,source,4097));
        if(!renames)assert(leaf("hello.c")==f);
        handles_closed();assert(edit_text_step(result,sizeof(result),16U,1)==1);
    }
    for(k=0;k<35;k++) {
        reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=16384;memset(files[f].bytes,'x',16384);
        strcpy(call.arguments,"{\"path\":\"hello.c\"}");assert(read_text_begin(&call,result,sizeof(result),NULL,NULL,42)==2);
        for(step=0;step<k;step++)assert(read_text_step(result,sizeof(result),43,0)==2);
        assert(read_text_step(result,sizeof(result),3642,0)==1);handles_closed();
    }
    /* Pending entry points themselves enforce the execution-evidence guard. */
    reset();strcpy(call.arguments,"{\"path\":\"wOrKeR01:bUiLdJoBs:fake\"}");
    r=edit_text_begin(&call,result,sizeof(result),journal,NULL,42);
    assert(!r && strstr(result,"EXECUTION_EVIDENCE_READ_ONLY"));
    /* Fail every transfer/open/close/flush, including later chunks. */
    edit_setup(source,"z","y");io_reads=io_writes=io_closes=io_flushes=io_opens=0;largest_transfer=0;
    assert(!run() && largest_transfer<=TOOLS_WORK_CHUNK);
    { int counts[]={io_reads,io_writes,io_closes,io_flushes,io_opens};
      for(int operation=0;operation<5;operation++)for(int failure=1;failure<=counts[operation];failure++) {
          f=edit_setup(source,"z","y");io_reads=io_writes=io_closes=io_flushes=io_opens=0;
          if(operation==0)fault_read=failure;
          if(operation==1)fault_write=failure;
          if(operation==2)fault_close=failure;
          if(operation==3)fault_flush=failure;
          if(operation==4)fault_open=failure;
          r=run();assert(!strstr(result,"\"status\":\"ok\""));
          if(renames)assert(r==1 && strstr(result,"uncertain"));
          assert(!memcmp(files[f].bytes,source,4097));handles_closed();
      }
    }
    /* Search continuations remain observational beyond the former 8 KiB
     * prefix and at a scan boundary. */
    reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=65536;
    memset(files[f].bytes,'x',65536);memcpy(files[f].bytes+8190,"needle",6);memcpy(files[f].bytes+50000,"needle",6);
    { char cursor[160]="";int seen=0,pages=0;
      do {
          strcpy(call.name,"search_text");snprintf(call.arguments,sizeof(call.arguments),"{\"root\":\"\",\"query\":\"needle\",\"cursor\":\"%s\"}",cursor);
          tools_execute(&call,result,sizeof(result));
          assert(!strstr(result,"revision"));
          if(strstr(result,"\"byte\":8190"))seen|=1;
          if(strstr(result,"\"byte\":50000"))seen|=2;
          assert(++pages<20);
          if(strstr(result,"\"truncated\":false"))break;
          field("next_cursor",cursor,sizeof(cursor));
      } while(1);
      assert(seen==3);
    }
    /* Invalid result capacity still closes an owned pending handle. */
    reset();f=add(10,"hello.c",0);files[f].info.fdType='TEXT';files[f].size=1;files[f].bytes[0]='x';
    strcpy(call.arguments,"{\"path\":\"hello.c\"}");assert(read_text_begin(&call,result,sizeof(result),NULL,NULL,42)==2);
    assert(read_text_step(result,1,43,0)==1);handles_closed();
    edit_setup("one\r","one","two");r=edit_text_begin(&call,result,sizeof(result),journal,NULL,42);assert(r==2);
    assert(edit_text_step(result,1,43,0)==1);handles_closed();
    /* Fresh readback tokens remain independent of a byte cursor. */
    edit_setup("one\r","one","two");field("revision",revision,sizeof(revision));
    assert(!run());strcpy(call.name,"read_text");strcpy(call.arguments,"{\"path\":\"hello.c\",\"start_byte\":2}");
    tools_execute(&call,result,sizeof(result));assert(!strstr(result,revision));handles_closed();
    puts("PASS large text: caps, verified snapshots, whole-file navigation, KMP boundaries, cancellation phases, wraparound and recovery handles");
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
    { const struct { const char *arguments, *code; } bad[]={
        {"{\"path\":\"missing:inner\"}","PATH"},{"{\"path\":\":escape\"}","PATH"},
        {"{\"path\":\"src:\"}","PATH"},{"{\"path\":\"\"}","PATH"},
        {"{}","ARGUMENTS"},{"{\"path\":\"a\",\"path\":\"b\"}","ARGUMENTS"},
        {"{\"path\":\"a\",\"mode\":\"x\"}","ARGUMENTS"},{"{\"path\":\"\\ud83e\\udd80\"}","PATH"}};
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){char wanted[48];
        reset();strcpy(call.name,"create_folder");strcpy(call.arguments,bad[i].arguments);
        snprintf(wanted,sizeof(wanted),"\"code\":\"%s\"",bad[i].code);
        assert(!run() && strstr(result,"\"status\":\"error\"") && strstr(result,wanted) && !creates && !journals);} }
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
    assert(!run() && strstr(result,"CREATED_PROJECT") && journals==3 && creates==PROJECT_CREATES);
    parent=leaf("Project");assert(parent>=0 && files[parent].dir);
    for(i=0;i<PROJECT_FILES;i++) {
        unsigned char name[32];name[0]=(unsigned char)strlen(project_inputs[i].name);
        memcpy(name+1,project_inputs[i].name,name[0]);j=find(files[parent].id,name);
        assert(j>=0 && files[j].info.fdType=='TEXT' && files[j].info.fdCreator=='ttxt' && !files[j].resource);
        assert(files[j].size==(long)strlen(project_inputs[i].bytes) && !memcmp(files[j].bytes,project_inputs[i].bytes,(size_t)files[j].size));
        assert(!memchr(files[j].bytes,10,(size_t)files[j].size));
    }
    assert(!run() && strstr(result,"EXISTS") && creates==PROJECT_CREATES && journals==3);
    { const struct { const char *arguments, *code; } bad[]={
        {"{}","ARGUMENTS"},{"{\"path\":\"Project\",\"template\":\"other\"}","ARGUMENTS"},
        {"{\"path\":\"a\",\"path\":\"b\"}","ARGUMENTS"},{"{\"path\":\"missing:Project\"}","PATH"},
        {"{\"path\":\":escape\"}","PATH"},{"{\"path\":\"Project:\"}","PATH"},{"{\"path\":\"\"}","PATH"}};
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){char wanted[48];
        reset();strcpy(call.name,"create_project");strcpy(call.arguments,bad[i].arguments);
        snprintf(wanted,sizeof(wanted),"\"code\":\"%s\"",bad[i].code);
        assert(!run() && strstr(result,"\"status\":\"error\"") && strstr(result,wanted) && !creates && !journals);} }
    reset();i=add(10,"alias",1);files[i].info.fdFlags=0x8000;strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"alias:Project\"}");assert(!run() && !creates);
    for(i=1;i<=3;i++) {
        reset();strcpy(call.name,"create_project");strcpy(call.arguments,"{\"path\":\"Project\"}");fail_journal=i;
        assert(run());assert(i==3 ? leaf("Project")>=0 && strstr(result,"uncertain") : leaf("Project")<0);
        assert(i==1 ? !creates : creates==PROJECT_CREATES);
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

/* ---------- AGENTS.md loading: the file named <folder>:AGENTS.md, MacRoman/CR
 * TEXT like every workspace text file, capped and truncated visibly. ------- */
static char instructions[AGENT_INSTRUCTIONS_CAP+1];
static int put_file(long parent, const char *name, const char *bytes, long size)
{
    int i=add(parent,name,0);
    memcpy(files[i].bytes,bytes,(size_t)size); files[i].size=size; files[i].info.fdType='TEXT';
    return i;
}
static int load(const char *folder, unsigned long *hash)
{
    memset(instructions,'#',sizeof(instructions)); instructions[sizeof(instructions)-1]=0;
    return tools_read_instructions(folder,instructions,sizeof(instructions),hash);
}
static void project_name_checks(void)
{
    static const struct { const char *tool, *arguments, *expected; } cases[] = {
        {"read_text","{\"path\":\"Putt:main.c\"}","Putt"},
        {"write_text","{\"path\":\"Putt:src:util.c\",\"text\":\"x\"}","Putt"},
        {"edit_text","{\"path\":\"Putt:main.c\",\"expected_revision\":\"r\",\"old_text\":\"a\",\"new_text\":\"b\"}","Putt"},
        {"list_files","{\"root\":\"Putt:src\"}","Putt"},
        {"search_text","{\"root\":\"Putt\",\"query\":\"x\"}","Putt"},
        {"create_folder","{\"path\":\"Putt:assets\"}","Putt"},
        {"create_project","{\"path\":\"Putt\"}","Putt"},
        {"build_project","{\"path\":\"Putt\"}","Putt"},
        {"view_image","{\"path\":\"caf\\u00e9:shot.png\"}","caf\xc3\xa9"},
        {"read_text","{\"path\":\"notes.txt\"}","notes.txt"},
        /* No project: the workspace root, no path argument, a leading colon or a bad document. */
        {"list_files","{\"root\":\"\"}",NULL},
        {"list_files","{}",NULL},
        {"get_environment","{}",NULL},
        {"run_application","{\"build_id\":\"b1\"}",NULL},
        {"read_text","{\"path\":\":Putt:main.c\"}",NULL},
        {"read_text","{\"path\":7}",NULL},
        {"read_text","{\"path\":\"Putt:main.c\"",NULL},
        {"read_text","{\"path\":\"a/b:c\"}",NULL},
    };
    char name[32];
    size_t i;
    memset(&call,0,sizeof(call));
    for(i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        strcpy(call.name,cases[i].tool); strcpy(call.arguments,cases[i].arguments);
        memset(name,'?',sizeof(name));
        if(cases[i].expected) assert(tools_call_project(&call,name,sizeof(name))==1 && !strcmp(name,cases[i].expected));
        else assert(tools_call_project(&call,name,sizeof(name))==0 && !name[0]);
    }
    /* A name that does not fit the caller's buffer is no project, never a cut one. */
    strcpy(call.arguments,"{\"path\":\"Putt:main.c\"}");
    assert(tools_call_project(&call,name,4)==0 && !name[0]);
    puts("PASS AGENTS.md project names: first path component of path or root, nothing else");
}
static void instruction_checks(void)
{
    unsigned long hash,other;
    static char big[20000];
    char marker[80];
    int i,n;
    snprintf(marker,sizeof(marker),"[AGENTS.md truncated at %d bytes]",AGENT_INSTRUCTIONS_CAP);
    /* Absent is the ordinary case: no file, no folder, a file where a folder
     * would be, or an empty file all report nothing and leave out empty. */
    reset();
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_ABSENT && !instructions[0]);
    assert(load("Putt",&hash)==TOOLS_INSTRUCTIONS_ABSENT && !instructions[0]);
    put_file(10,"hello.c","x",1);
    assert(load("hello.c",&hash)==TOOLS_INSTRUCTIONS_ABSENT && !instructions[0]);
    put_file(10,"AGENTS.md","",0);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_ABSENT && !instructions[0]);
    /* The root file: MacRoman and CR become UTF-8 and LF; the hash names the bytes. */
    reset();
    put_file(10,"AGENTS.md","Use caf\x8e\rTwo spaces.\r",21);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_LOADED && !strcmp(instructions,"Use caf\xc3\xa9\nTwo spaces.\n") && hash);
    files[1].bytes[0]='X';
    assert(load("",&other)==TOOLS_INSTRUCTIONS_LOADED && other!=hash && instructions[0]=='X');
    /* A project file lives one folder down and is a different file. */
    reset();
    put_file(10,"AGENTS.md","root\r",5);
    i=add(10,"Putt",1); put_file(files[i].id,"AGENTS.md","project\r",8);
    assert(load("Putt",&hash)==TOOLS_INSTRUCTIONS_LOADED && !strcmp(instructions,"project\n"));
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_LOADED && !strcmp(instructions,"root\n"));
    /* Plain TEXT only: binary bytes, aliases, folders and resource forks are
     * refused, reported as unusable rather than silently absent. */
    reset(); put_file(10,"AGENTS.md","ab\0cd",5);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    reset(); put_file(10,"AGENTS.md","ab\x01" "cd",5);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    reset(); i=put_file(10,"AGENTS.md","rules\r",6); files[i].info.fdFlags=0x8000;
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    reset(); i=put_file(10,"AGENTS.md","rules\r",6); files[i].resource=10;
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    reset(); add(10,"AGENTS.md",1);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    /* A file that changes while it is read is not trusted. */
    reset(); put_file(10,"AGENTS.md","rules\r",6); touch_on_read=1;
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_UNUSABLE && !instructions[0]);
    touch_on_read=0;
    /* An output buffer that cannot hold a full file plus its terminator is refused. */
    reset(); put_file(10,"AGENTS.md","rules\r",6);
    assert(tools_read_instructions("",instructions,AGENT_INSTRUCTIONS_CAP,&hash)==TOOLS_INSTRUCTIONS_UNUSABLE);
    /* The cap itself fits whole; one byte more is cut with a visible marker. */
    reset(); memset(big,'x',AGENT_INSTRUCTIONS_CAP); put_file(10,"AGENTS.md",big,AGENT_INSTRUCTIONS_CAP);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_LOADED && strlen(instructions)==AGENT_INSTRUCTIONS_CAP && !strstr(instructions,"truncated"));
    reset(); memset(big,'x',AGENT_INSTRUCTIONS_CAP+1); put_file(10,"AGENTS.md",big,AGENT_INSTRUCTIONS_CAP+1);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_TRUNCATED && strlen(instructions)<=AGENT_INSTRUCTIONS_CAP);
    n=(int)strlen(instructions); assert(n>(int)strlen(marker) && !strcmp(instructions+n-(int)strlen(marker),marker));
    /* A long file is cut on a line boundary, so no rule is left half-written. */
    reset(); n=0; for(i=0;i<1000;i++) n+=snprintf(big+n,sizeof(big)-(size_t)n,"Rule number %03d.\r",i);
    put_file(10,"AGENTS.md",big,n);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_TRUNCATED && strlen(instructions)<=AGENT_INSTRUCTIONS_CAP);
    { char *end=strstr(instructions,"\n[AGENTS.md truncated"); assert(end && end[-1]=='.' && !strncmp(instructions,"Rule number 000.\n",17)); }
    /* Three-byte characters expand past the cap before it is read in full: the
     * cut never lands inside a character and the text stays valid UTF-8. */
    reset(); memset(big,0xdb,AGENT_INSTRUCTIONS_CAP); put_file(10,"AGENTS.md",big,AGENT_INSTRUCTIONS_CAP);
    assert(load("",&hash)==TOOLS_INSTRUCTIONS_TRUNCATED && strlen(instructions)<=AGENT_INSTRUCTIONS_CAP);
    { const unsigned char *u=(const unsigned char *)instructions; size_t k=0;
      while(u[k] && !strncmp((const char *)u+k,"\xe2\x82\xac",3)) k+=3;
      assert(k>=3000 && !strncmp((const char *)u+k,"\n[AGENTS.md truncated",21)); }
    puts("PASS AGENTS.md loader: absent, MacRoman/CR, project folders, refusals, change detection, cap and visible truncation");
    project_name_checks();
}
/* ---------- Reversible delete: pinned batches into the volume Trash ------- */
static int trash_file(long parent, const char *name, const char *bytes)
{
    int i=add(parent,name,0);
    memcpy(files[i].bytes,bytes,strlen(bytes)); files[i].size=(long)strlen(bytes);
    files[i].info.fdType='TEXT'; files[i].info.fdCreator='ttxt';
    return i;
}
static int list_revision(const char *leaf_name, char *out, size_t cap)
{
    JsonToken t[128]; int i, files_token, found=0;
    strcpy(call.name,"list_files"); strcpy(call.arguments,"{\"root\":\"\",\"limit\":12}");
    tools_execute(&call,result,sizeof(result));
    assert(json_parse(result,strlen(result),t,128)>0);
    files_token=json_member(result,t,0,"files"); assert(files_token>=0 && t[files_token].type==JSON_ARRAY);
    for(i=files_token+1;i<t[files_token].next;i=t[i].next) {
        char path[256];
        if(json_string(result,t,json_member(result,t,i,"path"),path,sizeof(path))<0)continue;
        if(strcmp(path,leaf_name))continue;
        assert(json_string(result,t,json_member(result,t,i,"revision"),out,cap)>=0);
        found=1;
    }
    return found;
}
static void info_revision(const char *path, char *out, size_t cap)
{
    strcpy(call.name,"get_file_info");
    snprintf(call.arguments,sizeof(call.arguments),"{\"path\":\"%s\"}",path);
    tools_execute(&call,result,sizeof(result));
    field("revision",out,cap);
}
static int trash_can_model(void)
{
    int ntf=add(10,"Network Trash Folder",1);
    int can=add(files[ntf].id,"Trash Can #2",1);
    trash_can=can; return can;
}
static int move_one(const char *path, const char *revision)
{
    strcpy(call.name,"move_to_trash");
    snprintf(call.arguments,sizeof(call.arguments),"{\"files\":[{\"path\":\"%s\",\"revision\":\"%s\"}]}",path,revision);
    return run();
}
static int result_file_field(const char *name, char *out, size_t cap)
{
    JsonToken t[128]; int files_token, first;
    assert(json_parse(result,strlen(result),t,128)>0);
    files_token=json_member(result,t,0,"files"); assert(files_token>=0 && t[files_token].type==JSON_ARRAY);
    first=files_token+1; assert(first<t[files_token].next);
    return json_string(result,t,json_member(result,t,first,name),out,cap)>=0;
}
static void trash_checks(void)
{
    char rev[48], rev2[48], text[256];
    int i, j, n, can, folder;
    /* Both listings emit the same cat- pin; folders emit none. */
    reset(); trash_file(10,"hello.c","one");
    assert(list_revision("hello.c",rev,sizeof(rev)) && !strncmp(rev,"cat-",4));
    info_revision("hello.c",rev2,sizeof(rev2));
    assert(!strcmp(rev,rev2));
    reset(); add(10,"sub",1);
    strcpy(call.name,"get_file_info"); strcpy(call.arguments,"{\"path\":\"sub\"}");
    tools_execute(&call,result,sizeof(result));
    assert(strstr(result,"\"kind\":\"folder\"") && !strstr(result,"revision"));

    /* One pinned file renamed into the network Trash. */
    reset(); i=trash_file(10,"hello.c","one");
    assert(list_revision("hello.c",rev,sizeof(rev)));
    can=trash_can_model();
    assert(!move_one("hello.c",rev) && strstr(result,"TRASHED") && strstr(result,"\"moved\":1") &&
        journals==2 && trash_moves==1 && trash_intents==1 && trash_commits==1);
    field("trash",text,sizeof(text)); assert(!strcmp(text,"Network Trash Folder:Trash Can #2:"));
    assert(result_file_field("moved_as",text,sizeof(text)) && !strcmp(text,"hello.c"));
    assert(files[i].parent==files[can].id && !strcmp(files[i].name,"hello.c") && !opens[i]);

    /* A name already in the Trash is refused; a move never renames. */
    reset(); i=trash_file(10,"hello.c","one");
    assert(list_revision("hello.c",rev,sizeof(rev)));
    can=trash_can_model(); trash_file(files[can].id,"hello.c","old");
    assert(!move_one("hello.c",rev) && strstr(result,"EXISTS") && !trash_moves && files[i].parent==10);

    /* No volume Trash (or one on another volume): one journaled workspace
     * fallback, reused by later batches. */
    reset(); i=trash_file(10,"hello.c","one");
    assert(list_revision("hello.c",rev,sizeof(rev)));
    assert(!move_one("hello.c",rev) && strstr(result,"TRASHED") && creates==1 && journals==4);
    field("trash",text,sizeof(text)); assert(!strcmp(text,"Sherclawk Trash:"));
    folder=leaf("Sherclawk Trash"); assert(folder>=0 && files[folder].dir && files[i].parent==files[folder].id);
    j=trash_file(10,"two.c","two");
    assert(list_revision("two.c",rev2,sizeof(rev2)));
    assert(!move_one("two.c",rev2) && strstr(result,"TRASHED") && creates==1 && journals==6);
    assert(files[j].parent==files[folder].id);
    reset(); i=trash_file(10,"hello.c","one");
    assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); trash_vref=2;
    assert(!move_one("hello.c",rev) && strstr(result,"TRASHED") && leaf("Sherclawk Trash")>=0);
    assert(files[i].parent==files[leaf("Sherclawk Trash")].id);

    /* Refusals: nothing moves, no journal record is written. */
    reset(); add(10,"dir",1);
    assert(!move_one("dir","cat-0-0-0-0") && strstr(result,"NOT_FILE") && journals==0 && !trash_moves);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    files[i].locked=1;
    assert(!move_one("hello.c",rev) && strstr(result,"LOCKED") && journals==0 && !trash_moves && files[i].parent==10);
    files[i].locked=0;
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    files[i].info.fdFlags=0x8000;
    assert(!move_one("hello.c",rev) && strstr(result,"ALIAS") && !trash_moves);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    files[i].mddat++;
    assert(!move_one("hello.c",rev) && strstr(result,"REVISION_MISMATCH") && journals==0 && !trash_moves && files[i].parent==10);
    reset(); { int w=add(10,"Worker01",1), b=add(files[w].id,"buildjobs",1); trash_file(files[b].id,"log","x"); }
    assert(!move_one("Worker01:buildjobs:log","cat-0-0-0-0") && strstr(result,"REFUSED") && journals==0);
    reset(); { int t=add(10,"Sherclawk Trash",1); trash_file(files[t].id,"old.c","x"); }
    assert(!move_one("Sherclawk Trash:old.c","cat-0-0-0-0") && strstr(result,"REFUSED") && !trash_moves);
    { const char *bad[]={
        "{}","{\"files\":[]}","{\"files\":\"x\"}",
        "{\"files\":[{\"path\":\"hello.c\"}]}",
        "{\"files\":[{\"revision\":\"cat-0-0-0-0\"}]}",
        "{\"files\":[{\"path\":\"hello.c\",\"revision\":\"full-x\"}]}",
        "{\"files\":[{\"path\":\"hello.c\",\"revision\":\"cat-0-0-0-0\",\"extra\":1}]}",
        "{\"files\":[{\"path\":\"a\",\"path\":\"b\",\"revision\":\"cat-0-0-0-0\"}]}",
        "{\"files\":[{\"path\":\"hello.c\",\"revision\":\"cat-0-0-0-0\"}],\"mode\":\"x\"}" };
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){reset();strcpy(call.name,"move_to_trash");strcpy(call.arguments,bad[i]);
        assert(!run() && strstr(result,"ARGUMENTS") && journals==0 && !trash_moves);} }
    reset(); trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    { char many[AGENT_ARGUMENT_CAP]; size_t at=0;
      at+=(size_t)snprintf(many,sizeof(many),"{\"files\":[");
      for(n=0;n<9;n++)at+=(size_t)snprintf(many+at,sizeof(many)-at,"%s{\"path\":\"hello.c\",\"revision\":\"%s\"}",n?",":"",rev);
      snprintf(many+at,sizeof(many)-at,"]}");
      strcpy(call.name,"move_to_trash"); strcpy(call.arguments,many);
      assert(!run() && strstr(result,"ARGUMENTS") && journals==0); }

    /* A batch moves every pinned item; one stale pin refuses the whole batch. */
    reset(); i=trash_file(10,"one.c","1"); j=trash_file(10,"two.c","2");
    assert(list_revision("one.c",rev,sizeof(rev)) && list_revision("two.c",rev2,sizeof(rev2)));
    can=trash_can_model();
    strcpy(call.name,"move_to_trash");
    snprintf(call.arguments,sizeof(call.arguments),"{\"files\":[{\"path\":\"one.c\",\"revision\":\"%s\"},{\"path\":\"two.c\",\"revision\":\"%s\"}]}",rev,rev2);
    assert(!run() && strstr(result,"TRASHED") && strstr(result,"\"moved\":2") && journals==4 && trash_moves==2);
    assert(files[i].parent==files[can].id && files[j].parent==files[can].id);
    reset(); i=trash_file(10,"one.c","1"); j=trash_file(10,"two.c","2");
    assert(list_revision("one.c",rev,sizeof(rev)) && list_revision("two.c",rev2,sizeof(rev2)));
    can=trash_can_model(); files[j].mddat++;
    strcpy(call.name,"move_to_trash");
    snprintf(call.arguments,sizeof(call.arguments),"{\"files\":[{\"path\":\"one.c\",\"revision\":\"%s\"},{\"path\":\"two.c\",\"revision\":\"%s\"}]}",rev,rev2);
    assert(!run() && strstr(result,"REVISION_MISMATCH") && strstr(result,"\"moved\":0") && journals==0 && !trash_moves);
    assert(files[i].parent==10 && files[j].parent==10);

    /* A mid-batch execution failure stops there and reports what moved. */
    reset(); i=trash_file(10,"one.c","1"); j=trash_file(10,"two.c","2");
    assert(list_revision("one.c",rev,sizeof(rev)) && list_revision("two.c",rev2,sizeof(rev2)));
    can=trash_can_model(); rename_error=1; fault_rename=2;
    strcpy(call.name,"move_to_trash");
    snprintf(call.arguments,sizeof(call.arguments),"{\"files\":[{\"path\":\"one.c\",\"revision\":\"%s\"},{\"path\":\"two.c\",\"revision\":\"%s\"}]}",rev,rev2);
    assert(!run() && strstr(result,"MOVE_FAILED") && strstr(result,"\"moved\":1") && journals==3 && trash_moves==1);
    assert(files[i].parent==files[can].id && files[j].parent==10);
    field("failed",text,sizeof(text)); assert(!strcmp(text,"two.c"));

    /* Journal barriers: intent stops before the rename, a failed commit is uncertain. */
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); fail_journal=1;
    assert(move_one("hello.c",rev)==1 && strstr(result,"JOURNAL") && !trash_moves && files[i].parent==10 && journals==1);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); fail_journal=2;
    assert(move_one("hello.c",rev)==1 && strstr(result,"uncertain") && strstr(result,"JOURNAL_AFTER_MOVE") && trash_moves==1 && journals==2);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    fail_journal=1;
    assert(move_one("hello.c",rev)==1 && strstr(result,"JOURNAL") && leaf("Sherclawk Trash")<0 && journals==1);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    fail_journal=2;
    assert(move_one("hello.c",rev)==1 && strstr(result,"uncertain") && strstr(result,"trash_folder") &&
        leaf("Sherclawk Trash")>=0 && !trash_moves && journals==2);

    /* Move errors and post-move verification failures. */
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); rename_error=1;
    assert(!move_one("hello.c",rev) && strstr(result,"MOVE_FAILED") && !trash_moves && files[i].parent==10 && journals==1);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); flush_error=1;
    assert(move_one("hello.c",rev)==1 && strstr(result,"uncertain") && strstr(result,"MOVE_UNCERTAIN") && trash_moves==1 && journals==1);
    /* A lost rename reply is verified like any other move, never claimed failed. */
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    can=trash_can_model(); rename_lost=1;
    assert(!move_one("hello.c",rev) && strstr(result,"TRASHED") && trash_moves==1 && journals==2 &&
        files[i].parent==files[can].id);
    reset(); i=trash_file(10,"hello.c","one"); assert(list_revision("hello.c",rev,sizeof(rev)));
    trash_can_model(); strcpy(call.name,"move_to_trash");
    snprintf(call.arguments,sizeof(call.arguments),"{\"files\":[{\"path\":\"hello.c\",\"revision\":\"%s\"}]}",rev);
    assert(tools_execute_recorded(&call,result,sizeof(result),NULL,NULL)==1 && strstr(result,"JOURNAL") && !trash_moves);

    /* A report that cannot fit the result budget is refused untouched. */
    reset();
    { char longpath[520]="", leafname[32], p2[8][520], rb[8][48], args2[AGENT_ARGUMENT_CAP];
      size_t at=0; long parent=10; int d,e;
      for(d=0;d<5;d++){ memset(leafname,0xdb,31); leafname[31]=0; i=add(parent,leafname,1); parent=files[i].id; for(e=0;e<31;e++)strcat(longpath,"\xe2\x82\xac"); strcat(longpath,":"); }
      for(n=0;n<8;n++){ snprintf(leafname,sizeof(leafname),"f%d.c",n); trash_file(parent,leafname,"x");
        snprintf(p2[n],sizeof(p2[n]),"%s%s",longpath,leafname); info_revision(p2[n],rb[n],sizeof(rb[n])); }
      (void)trash_can_model();
      at+=(size_t)snprintf(args2,sizeof(args2),"{\"files\":[");
      for(n=0;n<8;n++)at+=(size_t)snprintf(args2+at,sizeof(args2)-at,"%s{\"path\":\"%s\",\"revision\":\"%s\"}",n?",":"",p2[n],rb[n]);
      snprintf(args2+at,sizeof(args2)-at,"]}");
      strcpy(call.name,"move_to_trash"); strcpy(call.arguments,args2);
      assert(!run() && strstr(result,"LIMIT") && journals==0 && !trash_moves); }
    /* The pre-rename capacity check is a true bound: whatever the batch size, the
       call either finishes with every file moved or refuses before any rename
       (never a partial move that then cannot report). */
    { int k, finished=0, refused=0;
      for(k=1;k<=8;k++) {
        char longpath[520]="", leafname[32], p2[8][520], rb[8][48], args2[AGENT_ARGUMENT_CAP];
        size_t at=0; long parent=10; int d,e;
        reset();
        for(d=0;d<5;d++){ memset(leafname,0xdb,31); leafname[31]=0; i=add(parent,leafname,1); parent=files[i].id; for(e=0;e<31;e++)strcat(longpath,"\xe2\x82\xac"); strcat(longpath,":"); }
        trash_can_model();
        for(n=0;n<k;n++){ snprintf(leafname,sizeof(leafname),"f%d.c",n); trash_file(parent,leafname,"x");
          snprintf(p2[n],sizeof(p2[n]),"%s%s",longpath,leafname); info_revision(p2[n],rb[n],sizeof(rb[n])); }
        at+=(size_t)snprintf(args2,sizeof(args2),"{\"files\":[");
        for(n=0;n<k;n++)at+=(size_t)snprintf(args2+at,sizeof(args2)-at,"%s{\"path\":\"%s\",\"revision\":\"%s\"}",n?",":"",p2[n],rb[n]);
        snprintf(args2+at,sizeof(args2)-at,"]}");
        strcpy(call.name,"move_to_trash"); strcpy(call.arguments,args2);
        if(!run() && strstr(result,"TRASHED") && trash_moves==k) finished++;
        else { assert(strstr(result,"LIMIT") && journals==0 && !trash_moves); refused++; }
      }
      assert(finished>0 && refused>0); }
    puts("PASS move_to_trash: pinned batches, network Trash and workspace fallback, collisions, refusals, journal barriers, partial failure and result caps");
}
/* The MCP configuration and its staging/backup siblings hold credentials. They
 * are protected by catalog identity (Preferences folder ID + leaf name): every
 * model-facing tool must refuse them, wherever the workspace puts Preferences. */
#define SECRET "Bearer SECRET-TOKEN-123"
#define CONFIG_NAME "Sherclawk MCP Servers.json"
static struct { int prefs, config, stage, backup, notes; } guarded;
static void guard_setup(void)
{
    static const char body[] = "{\"mcpServers\":{\"t\":{\"headers\":{\"Authorization\":\"" SECRET "\"}}}}";
    const char *names[3] = { CONFIG_NAME, CONFIG_NAME ".new", CONFIG_NAME ".old" };
    int *slot[3] = { &guarded.config, &guarded.stage, &guarded.backup }, k;
    reset();
    guarded.prefs = add(10, "Preferences", 1);
    prefs_dir = files[guarded.prefs].id;
    for (k = 0; k < 3; k++) {
        *slot[k] = add(files[guarded.prefs].id, names[k], 0);
        files[*slot[k]].info.fdType = 'TEXT'; files[*slot[k]].info.fdCreator = 'ttxt';
        memcpy(files[*slot[k]].bytes, body, sizeof(body) - 1); files[*slot[k]].size = (long)sizeof(body) - 1;
    }
    guarded.notes = add(files[guarded.prefs].id, "notes.txt", 0);
    files[guarded.notes].info.fdType = 'TEXT';
    strcpy(files[guarded.notes].bytes, "needle in notes\r"); files[guarded.notes].size = 16;
}
static void guard_call(const char *name, const char *arguments)
{
    strcpy(call.name, name); strcpy(call.arguments, arguments);
    tools_execute(&call, result, sizeof(result));
}
/* The call was refused by the guard and revealed nothing of the files. */
static void guard_refused(void)
{
    if (!strstr(result, "\"code\":\"PROTECTED\"")) fprintf(stderr, "guard: %s | %s\n", call.name, result);
    assert(strstr(result, "\"status\":\"error\"") && strstr(result, "\"code\":\"PROTECTED\""));
    assert(strstr(result, "\"os_error\":30001"));
    assert(!strstr(result, "SECRET") && !strstr(result, "Bearer") && !strstr(result, "mcpServers"));
}
static void guard_untouched(void)
{
    static const char body[] = "{\"mcpServers\":{\"t\":{\"headers\":{\"Authorization\":\"" SECRET "\"}}}}";
    int k, slot[3] = { guarded.config, guarded.stage, guarded.backup };
    for (k = 0; k < 3; k++)
        assert(files[slot[k]].used && files[slot[k]].size == (long)sizeof(body) - 1 &&
               !memcmp(files[slot[k]].bytes, body, sizeof(body) - 1) && !files[slot[k]].locked);
    assert(!creates && !published && !trash_moves && !renames);
}
static void protection_checks(void)
{
    const char *protected_paths[] = {
        "Preferences:" CONFIG_NAME, "Preferences:" CONFIG_NAME ".new", "Preferences:" CONFIG_NAME ".old",
        "Preferences:SHERCLAWK mcp servers.JSON", "Preferences:sherclawk MCP servers.json.OLD" };
    char arguments[AGENT_ARGUMENT_CAP];
    int k, i;

    /* Reads: file, metadata and resource tools refuse every protected name, in any case. */
    guard_setup();
    for (k = 0; k < (int)(sizeof(protected_paths) / sizeof(*protected_paths)); k++) {
        const char *tools[] = { "read_text", "get_file_info", "list_resources", "resolve_alias" };
        for (i = 0; i < (int)(sizeof(tools) / sizeof(*tools)); i++) {
            snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", protected_paths[k]);
            guard_call(tools[i], arguments);
            guard_refused();
        }
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\",\"type\":\"STR \",\"id\":128}", protected_paths[k]);
        guard_call("read_resource", arguments); guard_refused();
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", protected_paths[k]);
        view_image_reset(); view_image_set_vision(AGENT_VISION_YES);
        assert(!view_begin(arguments)); strcpy(call.name, "view_image"); guard_refused();
        assert(!view_image_held());
    }
    guard_untouched();

    /* Listing omits protected entries and still pages through the rest. */
    guard_call("list_files", "{\"root\":\"Preferences\"}");
    assert(strstr(result, "Preferences:notes.txt") && !strstr(result, "Servers"));
    assert(!strstr(result, "SECRET") && strstr(result, "\"truncated\":false"));
    { int pages = 0, seen = 0; char cursor[16] = "0";
      while (pages < 10) {
          char *at;
          snprintf(arguments, sizeof(arguments), "{\"root\":\"Preferences\",\"limit\":1,\"cursor\":%s}", cursor);
          guard_call("list_files", arguments); pages++;
          assert(!strstr(result, "Servers") && !strstr(result, "\"status\":\"error\""));
          if (strstr(result, "notes.txt")) seen++;
          if (strstr(result, "\"truncated\":false")) break;
          at = strstr(result, "\"next_cursor\":"); assert(at);
          snprintf(cursor, sizeof(cursor), "%ld", strtol(at + 14, NULL, 10));
      }
      assert(seen == 1 && pages < 10); }
    guard_call("list_files", "{\"root\":\"\"}");
    assert(strstr(result, "Preferences:"));  /* the folder itself is an ordinary entry */

    /* Search, flat and recursive, never reads or reports their bytes. */
    guard_call("search_text", "{\"root\":\"Preferences\",\"query\":\"SECRET\"}");
    assert(strstr(result, "\"matches\":[]") && !strstr(result, "SECRET\"") && strstr(result, "\"skipped\":3"));
    guard_call("search_text", "{\"root\":\"\",\"query\":\"Bearer\",\"recursive\":true,\"limit\":8}");
    assert(strstr(result, "\"matches\":[]") && !strstr(result, "Bearer\""));
    guard_call("search_text", "{\"root\":\"\",\"query\":\"mcpServers\",\"recursive\":true}");
    assert(strstr(result, "\"matches\":[]"));
    guard_call("search_text", "{\"root\":\"\",\"query\":\"needle\",\"recursive\":true}");
    assert(strstr(result, "Preferences:notes.txt") && strstr(result, "needle in notes"));
    guard_untouched();

    /* Mutation: nothing can create, replace, edit, fold or trash a protected name. */
    for (k = 0; k < (int)(sizeof(protected_paths) / sizeof(*protected_paths)); k++) {
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\",\"text\":\"{}\"}", protected_paths[k]);
        guard_call("write_text", arguments); guard_refused();
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", protected_paths[k]);
        guard_call("create_folder", arguments); guard_refused();
        guard_call("create_project", arguments); guard_refused();
        snprintf(arguments, sizeof(arguments),
            "{\"path\":\"%s\",\"expected_revision\":\"full-00000000-00000000-00000000-00000000\",\"old_text\":\"t\",\"new_text\":\"u\"}",
            protected_paths[k]);
        strcpy(call.name, "edit_text"); strcpy(call.arguments, arguments);
        assert(!run() && !journals);
        guard_refused();
        snprintf(arguments, sizeof(arguments),
            "{\"files\":[{\"path\":\"%s\",\"revision\":\"cat-0000000b-000004d2-00000040-00000000\"}]}", protected_paths[k]);
        strcpy(call.name, "move_to_trash"); strcpy(call.arguments, arguments);
        assert(!run() && !journals);
        guard_refused();
        assert(strstr(result, "\"moved\":0"));
    }
    guard_untouched();
    assert(!journals);

    /* An alias whose target is a protected file is refused and reveals nothing. */
    { int alias = add(10, "Config Alias", 0);
      unsigned char record;
      files[alias].info.fdType = 'alis'; files[alias].info.fdFlags = 0x8000;
      for (k = 0; k < 3; k++) {
          record = (unsigned char)((k == 0 ? guarded.config : k == 1 ? guarded.stage : guarded.backup) + 1);
          memset(resources, 0, sizeof(resources));
          add_resource(alias, 'alis', 0, "", &record, 1); files[alias].resource = 1;
          guard_call("resolve_alias", "{\"path\":\"Config Alias\"}");
          assert(strstr(result, "\"status\":\"error\"") && strstr(result, "\"os_error\":30001"));
          assert(!strstr(result, "Servers") && !strstr(result, "target_name"));
      }
      record = (unsigned char)(guarded.notes + 1);
      memset(resources, 0, sizeof(resources));
      add_resource(alias, 'alis', 0, "", &record, 1);
      guard_call("resolve_alias", "{\"path\":\"Config Alias\"}");
      assert(strstr(result, "\"relative_path\":\"Preferences:notes.txt\"")); }

    /* Ordinary operations in the same folder, and elsewhere, keep working. */
    guard_call("read_text", "{\"path\":\"Preferences:notes.txt\"}");
    assert(strstr(result, "needle in notes") && !strstr(result, "\"status\":\"error\""));
    guard_call("get_file_info", "{\"path\":\"Preferences:notes.txt\"}");
    assert(strstr(result, "\"kind\":\"file\""));
    { char revision[80];
      guard_call("read_text", "{\"path\":\"Preferences:notes.txt\"}"); field("revision", revision, sizeof(revision));
      snprintf(arguments, sizeof(arguments),
          "{\"path\":\"Preferences:notes.txt\",\"expected_revision\":\"%s\",\"old_text\":\"needle\",\"new_text\":\"thread\"}", revision);
      strcpy(call.name, "edit_text"); strcpy(call.arguments, arguments);
      editing = 1; journals = creates = published = renames = 0;
      assert(!run() && strstr(result, "EDITED"));
      editing = 0; journals = creates = published = renames = 0;
      guard_call("read_text", "{\"path\":\"Preferences:notes.txt\"}");
      assert(strstr(result, "thread in notes")); }
    strcpy(call.name, "write_text"); strcpy(call.arguments, "{\"path\":\"Preferences:other.txt\",\"text\":\"ok\"}");
    assert(!run() && strstr(result, "CREATED"));
    journals = creates = published = renames = 0;
    strcpy(call.name, "create_folder"); strcpy(call.arguments, "{\"path\":\"Preferences:Sub\"}");
    assert(!run() && strstr(result, "\"status\":\"ok\""));
    journals = creates = published = renames = 0;
    strcpy(call.name, "write_text"); strcpy(call.arguments, "{\"path\":\"hello.c\",\"text\":\"ok\"}");
    assert(!run() && strstr(result, "CREATED"));
    { int k, slot[3] = { guarded.config, guarded.stage, guarded.backup };
      for (k = 0; k < 3; k++) assert(files[slot[k]].used && strstr(files[slot[k]].bytes, SECRET)); }

    /* Identity, not spelling: the same leaf name elsewhere is an ordinary file. */
    reset();
    i = add(10, CONFIG_NAME, 0); files[i].info.fdType = 'TEXT'; strcpy(files[i].bytes, "plain"); files[i].size = 5;
    guard_call("read_text", "{\"path\":\"" CONFIG_NAME "\"}");
    assert(strstr(result, "plain") && !strstr(result, "PROTECTED"));
    guard_call("search_text", "{\"root\":\"\",\"query\":\"plain\"}");
    assert(strstr(result, CONFIG_NAME));

    /* Fail closed: an unlocatable Preferences folder protects everything, and a
     * lookup failure is never confused with "missing" so create tools stay out. */
    reset(); prefs_error = fnfErr;
    i = add(10, "hello.c", 0); files[i].info.fdType = 'TEXT'; strcpy(files[i].bytes, "hi"); files[i].size = 2;
    guard_call("read_text", "{\"path\":\"hello.c\"}"); guard_refused();
    guard_call("list_files", "{\"root\":\"\"}"); guard_refused();
    guard_call("search_text", "{\"root\":\"\",\"query\":\"hi\"}"); guard_refused();
    strcpy(call.name, "write_text"); strcpy(call.arguments, "{\"path\":\"new.c\",\"text\":\"x\"}");
    assert(!run() && leaf("new.c") < 0 && !creates && !journals); guard_refused();
    assert(prefs_lookups > 0);
    reset(); prefs_error = paramErr;
    strcpy(call.name, "write_text"); strcpy(call.arguments, "{\"path\":\"new.c\",\"text\":\"x\"}");
    assert(!run() && leaf("new.c") < 0 && !creates); guard_refused();
    puts("PASS MCP configuration protection: identity guard across reads, listing, search, aliases, mutation and lookup failure");
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
    { const struct { const char *arguments, *code; } bad[]={
        {"{\"path\":\":escape\",\"text\":\"x\"}","PATH"},{"{\"path\":\"missing:hello.c\",\"text\":\"x\"}","PATH"},
        {"{\"path\":\"hello.c\",\"text\":\"\\ud83e\\udd80\"}","ENCODING_LIMIT"},{"{\"path\":\"hello.c\",\"text\":\"\\u0001\"}","NOT_TEXT"},
        {"{\"path\":\"hello.c\",\"text\":\"x\",\"text\":\"y\"}","ARGUMENTS"},{"{\"path\":\"hello.c\",\"text\":\"x\",\"mode\":\"overwrite\"}","ARGUMENTS"},
        {"{\"path\":\"hello.c\"}","ARGUMENTS"},{"{\"path\":\"hello.c\",\"text\":\"\\u0000\"}","ARGUMENTS"},
        {"{\"path\":\"hello.c\",\"text\":\"\\u007f\"}","NOT_TEXT"}};
      for(i=0;i<(int)(sizeof(bad)/sizeof(*bad));i++){char wanted[48];
        reset();strcpy(call.arguments,bad[i].arguments);
        snprintf(wanted,sizeof(wanted),"\"code\":\"%s\"",bad[i].code);
        assert(!run() && strstr(result,"\"status\":\"error\"") && strstr(result,wanted) && !creates && !journals);} }
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
    page_checks();
    edit_checks();
    large_text_checks();
    inspect_checks();
    protection_checks();
    view_checks();
    instruction_checks();
    trash_checks();
    return 0;
}
