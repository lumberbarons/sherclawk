/* Native bounded tools. Mutations stage verified TEXT files and journal every
 * publication boundary. All catalog/text work is bounded; aliases, binary
 * files, resource forks and parent traversal are never followed as text. */
#include "tools.h"
#include "inspect.h"
#include "json.h"
#include "text.h"
#include "config.h"
#include "build/project-template.h"
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <Memory.h>
#include <Gestalt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
extern unsigned long TickCount(void);

/* Runtime workspace root; the app replaces the compiled default from the saved
 * preferences before any tool runs. Tests keep the compiled default. */
static char gWorkspace[256] = SHERCLAWK_WORKSPACE;
const char *tools_workspace(void) { return gWorkspace; }
void tools_set_workspace(const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof(gWorkspace)) return;
    memcpy(gWorkspace, path, n + 1);
}

static int fail(char *out, size_t cap, const char *code, const char *message, int native)
{
    char q[512];
    if (json_quote(message, q, sizeof(q)) < 0) strcpy(q, "\"Tool failed\"");
    snprintf(out, cap, "{\"status\":\"error\",\"code\":\"%s\",\"message\":%s,\"os_error\":%d}", code, q, native);
    return -1;
}
static int append(char *out, size_t cap, size_t *at, const char *s)
{
    size_t n = strlen(s);
    if (*at >= cap || n >= cap - *at) return -1;
    memcpy(out + *at, s, n + 1); *at += n; return 0;
}
static int quote(char *out, size_t cap, size_t *at, const char *s)
{
    int n = json_quote(s, out + *at, cap - *at);
    if (n < 0) return -1;
    *at += (size_t)n; return 0;
}
/* The wire contract is relative paths only. This also rejects volume changes,
 * absolute paths, empty interior components and HFS parent syntax. */
int tools_validate_path(const char *path, int folder)
{
    size_t at, component = 0;
    if (!*path) return folder ? 0 : -1;
    if (*path == ':' || strlen(path) > 180) return -1;
    for (at = 0; path[at]; at++) {
        unsigned char c = (unsigned char)path[at];
        if (c < 32 || c == 127 || c == '/' || c == '\\') return -1;
        if (c == ':') { if (!component || component > 31) return -1; component = 0; }
        else component++;
    }
    if (component > 31 || (!component && !folder)) return -1;
    return 0;
}
static int spec_for(const char *path, int folder, FSSpec *spec)
{
    char local[256], full[256];
    Str255 p;
    size_t n;
    if (text_to_macroman_strict(path, local, sizeof(local)) < 0 || tools_validate_path(local, folder)) return paramErr;
    if (snprintf(full, sizeof(full), "%s%s", tools_workspace(), local) >= (int)sizeof(full)) return paramErr;
    n = strlen(full); if (n > 255) return paramErr;
    p[0] = (unsigned char)n; memcpy(p + 1, full, n);
    return FSMakeFSSpec(0, 0, p, spec);
}
static OSErr catalog(FSSpec *spec, CInfoPBRec *pb)
{
    memset(pb, 0, sizeof(*pb)); pb->hFileInfo.ioNamePtr = spec->name;
    pb->hFileInfo.ioVRefNum = spec->vRefNum; pb->hFileInfo.ioDirID = spec->parID;
    return PBGetCatInfoSync(pb);
}
static int string_arg(const char *s, const JsonToken *tokens, const char *name, char *out, size_t cap)
{
    return json_string(s, tokens, json_member(s, tokens, 0, name), out, cap);
}
static int int_arg(const char *s, const JsonToken *tokens, const char *name, int def, int low, int high)
{
    char value[32];
    long n;
    int i = json_member(s, tokens, 0, name), k, size;
    if (i < 0) return def;
    size = tokens[i].end - tokens[i].start;
    if (tokens[i].type != JSON_PRIMITIVE || size < 1 || size >= (int)sizeof(value)) return -1;
    for (k = 0; k < size; k++) if (s[tokens[i].start + k] < '0' || s[tokens[i].start + k] > '9') return -1;
    memcpy(value, s + tokens[i].start, (size_t)size); value[size] = 0;
    n = strtol(value, NULL, 10); return n >= low && n <= high ? (int)n : -1;
}
static int valid_keys(const char *s, const JsonToken *tokens, const char *allowed)
{
    char key[64], padded[68];
    int i, j;
    for (i = 1; i < tokens[0].next; i = tokens[i + 1].next) {
        if (json_string(s, tokens, i, key, sizeof(key)) < 0) return -1;
        snprintf(padded, sizeof(padded), "|%s|", key);
        if (!strstr(allowed, padded)) return -1;
        for (j = 1; j < i; j = tokens[j + 1].next) {
            char previous[64];
            if (json_string(s, tokens, j, previous, sizeof(previous)) < 0 || !strcmp(previous, key)) return -1;
        }
    }
    return 0;
}
static void environment(char *out, size_t cap)
{
    char root[768], q[1024];
    long system = 0; int length;
    Gestalt(gestaltSystemVersion, &system);
    if (text_to_utf8(tools_workspace(), strlen(tools_workspace()), root, sizeof(root)) < 0 ||
        json_quote(root, q, sizeof(q)) < 0) { fail(out, cap, "CONFIG", "Invalid workspace encoding.", 0); return; }
    length=snprintf(out, cap, "{\"status\":\"ok\",\"os\":\"classic Mac OS\",\"system_version_hex\":\"%04lx\","
        "\"architecture\":\"PowerPC\",\"workspace\":%s,\"paths\":\"relative colon-separated\","
        "\"encoding\":\"MacRoman data fork to UTF-8\",\"read_only\":false,\"free_heap_bytes\":%ld,"
        "\"tools\":[\"get_environment\",\"list_files\",\"read_text\",\"search_text\",\"write_text\",\"edit_text\",\"create_folder\",\"move_to_trash\",\"create_project\",\"build_project\",\"read_build_log\",\"run_application\",\"quit_application\",\"get_file_info\",\"resolve_alias\",\"list_processes\",\"list_fonts\",\"measure_text\",\"list_resources\",\"read_resource\",\"view_image\"],"
        "\"write_policy\":\"create_only_existing_parent\",\"folder_policy\":\"create_only_existing_parent\",\"write_max_bytes\":4096,"
        "\"edit_policy\":\"unique_exact_whole_revision_CR_backup\",\"edit_max_bytes\":%ld,"
        "\"edit_string_max_bytes\":%d,\"build_input_max_bytes\":%ld,\"descriptor_max_bytes\":%d,\"total_snapshot_max_bytes\":%ld,"
        "\"build_supported\":true,\"launch_supported\":true}", system, q, (long)FreeMem(), TOOLS_ACCEPTED_FILE_CAP, TOOLS_STRING_CAP, TOOLS_ACCEPTED_FILE_CAP, TOOLS_DESCRIPTOR_CAP, TOOLS_SNAPSHOT_CAP);
    if(length<0 || (size_t)length>=cap)fail(out,cap,"LIMIT","Environment report exceeds result capacity.",0);
}
static void list(const char *s, const JsonToken *tokens, char *out, size_t cap)
{
    char root[512], prefix[514], child[768], name[128], entry[1100], tail[120];
    FSSpec spec;
    CInfoPBRec pb;
    Str255 native;
    OSErr err;
    size_t at = 0;
    int cursor = int_arg(s, tokens, "cursor", 0, 0, 30000), limit = int_arg(s, tokens, "limit", 8, 1, 12);
    int count = 0, next, done = 0;
    long dir;
    if (valid_keys(s, tokens, "|root||cursor||limit|") || string_arg(s, tokens, "root", root, sizeof(root)) < 0 || cursor < 0 || limit < 0) {
        fail(out, cap, "ARGUMENTS", "Expected root, optional cursor and limit; no other fields.", 0); return;
    }
    err = spec_for(root, 1, &spec);
    if (!err) err = catalog(&spec, &pb);
    if (err || !(pb.hFileInfo.ioFlAttrib & 16)) { fail(out, cap, "FOLDER", "Cannot resolve the workspace folder.", err); return; }
    dir = pb.dirInfo.ioDrDirID;
    snprintf(prefix, sizeof(prefix), "%s%s", root, *root && root[strlen(root) - 1] != ':' ? ":" : "");
    append(out, cap, &at, "{\"status\":\"ok\",\"files\":[");
    next = cursor;
    while (count < limit) {
        size_t pos = 0;
        char kind[160];
        memset(&pb, 0, sizeof(pb)); native[0] = 0;
        pb.hFileInfo.ioNamePtr = native; pb.hFileInfo.ioVRefNum = spec.vRefNum;
        pb.hFileInfo.ioDirID = dir; pb.hFileInfo.ioFDirIndex = (short)(next + 1);
        err = PBGetCatInfoSync(&pb);
        if (err == fnfErr) { done = 1; break; }
        if (err) { fail(out, cap, "CATALOG", "Folder enumeration failed.", err); return; }
        if (text_to_utf8((char *)native + 1, native[0], name, sizeof(name)) < 0) {
            fail(out, cap, "ENCODING", "Filename conversion failed.", 0); return;
        }
        snprintf(child, sizeof(child), "%s%s%s", prefix, name, pb.hFileInfo.ioFlAttrib & 16 ? ":" : "");
        if (append(entry, sizeof(entry), &pos, "{\"path\":") || quote(entry, sizeof(entry), &pos, child)) break;
        if (pb.hFileInfo.ioFlAttrib & 16) {
            snprintf(kind, sizeof(kind), ",\"kind\":\"folder\",\"data_bytes\":0}");
        } else {
            char revision[48];
            tools_catalog_revision(&pb, revision, sizeof(revision));
            snprintf(kind, sizeof(kind), ",\"kind\":\"file\",\"data_bytes\":%ld,\"revision\":\"%s\"}",
                (long)pb.hFileInfo.ioFlLgLen, revision);
        }
        if (append(entry, sizeof(entry), &pos, kind)) break;
        if (at + pos + 150 >= cap) break;
        if (count) append(out, cap, &at, ",");
        append(out, cap, &at, entry); count++; next++;
    }
    snprintf(tail, sizeof(tail), "],\"truncated\":%s,\"next_cursor\":%d}", done ? "false" : "true", next);
    if (append(out, cap, &at, tail)) fail(out, cap, "LIMIT", "Folder result is too large.", 0);
}
static int plain_file(const FSSpec *spec, const CInfoPBRec *pb)
{
    char name[32];
    const char *dot;
    int i;
    if (pb->hFileInfo.ioFlAttrib & 16 || pb->hFileInfo.ioFlRLgLen) return 0;
    if (pb->hFileInfo.ioFlFndrInfo.fdType == 'alis' || (pb->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000)) return 0;
    if (pb->hFileInfo.ioFlFndrInfo.fdType == 'TEXT') return 1;
    if (spec->name[0] > 31) return 0;
    for (i = 0; i < spec->name[0]; i++) name[i] = (char)tolower(spec->name[i + 1]);
    name[i] = 0; dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".r") ||
        !strcmp(dot, ".txt") || !strcmp(dot, ".md") || !strcmp(dot, ".sh") ||
        !strcmp(dot, ".out") || !strcmp(dot, ".log") || !strcmp(dot, ".conf"));
}
static unsigned long hash_bytes(const char *bytes, long size)
{
    unsigned long hash = 2166136261UL;
    long i;
    for (i = 0; i < size; i++) hash = ((hash ^ (unsigned char)bytes[i]) * 16777619UL) & 0xffffffffUL;
    return hash;
}
/* Whole-file tokens include catalog identity. A bounded scan token must never
 * be accepted as a mutation guard, nor vary with the displayed page. */
static void full_revision(const CInfoPBRec *pb, const char *bytes, long size, char *out, size_t cap)
{
    snprintf(out, cap, "full-%08lx-%08lx-%08lx-%08lx", (unsigned long)pb->hFileInfo.ioDirID,
        (unsigned long)pb->hFileInfo.ioFlMdDat, (unsigned long)size, hash_bytes(bytes, size));
}
/* Catalog-only pin for destructive calls: a file that changed identity,
 * modification date or either fork size since it was listed is refused. Never
 * a content hash and never accepted as an edit revision. */
void tools_catalog_revision(const CInfoPBRec *pb, char *out, size_t cap)
{
    snprintf(out, cap, "cat-%08lx-%08lx-%08lx-%08lx", (unsigned long)pb->hFileInfo.ioDirID,
        (unsigned long)pb->hFileInfo.ioFlMdDat, (unsigned long)pb->hFileInfo.ioFlLgLen,
        (unsigned long)pb->hFileInfo.ioFlRLgLen);
}
/* Worker01:buildjobs and its children are sealed build evidence for every
 * mutating tool. Case-insensitive, whole path components only. */
static int evidence_path(const char *path)
{
    static const char sealed[] = "worker01:buildjobs";
    size_t i;
    for (i = 0; i < sizeof(sealed) - 1 && path[i]; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 'a' - 'A');
        if (c != (unsigned char)sealed[i]) return 0;
    }
    return i == sizeof(sealed) - 1 && (path[i] == 0 || path[i] == ':');
}
/* A real folder: a directory that is not an alias. */
static int is_plain_dir(const CInfoPBRec *pb)
{
    return (pb->hFileInfo.ioFlAttrib & 16) && !(pb->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000);
}
static int same_file(const CInfoPBRec *a, const CInfoPBRec *b)
{
    return a->hFileInfo.ioDirID == b->hFileInfo.ioDirID &&
        a->hFileInfo.ioFlMdDat == b->hFileInfo.ioFlMdDat &&
        a->hFileInfo.ioFlLgLen == b->hFileInfo.ioFlLgLen &&
        a->hFileInfo.ioFlRLgLen == b->hFileInfo.ioFlRLgLen &&
        /* Catalog attributes include transient fork-open bits. */
        (a->hFileInfo.ioFlAttrib & 0x11) == (b->hFileInfo.ioFlAttrib & 0x11) &&
        a->hFileInfo.ioFlFndrInfo.fdType == b->hFileInfo.ioFlFndrInfo.fdType &&
        a->hFileInfo.ioFlFndrInfo.fdCreator == b->hFileInfo.ioFlFndrInfo.fdCreator &&
        a->hFileInfo.ioFlFndrInfo.fdFlags == b->hFileInfo.ioFlFndrInfo.fdFlags;
}
/* Read and edit share one retained source snapshot; only one may be active. */
static char text_source[TOOLS_FILE_CAP + 1], text_edited[TOOLS_FILE_CAP + 1];
static char text_scratch[TOOLS_WORK_CHUNK];
static int text_active;
static int text_arguments(const AgentCall *call, JsonToken *tokens, char *out, size_t cap, int mutation)
{
    char path[512];
    if(cap<AGENT_RESULT_CAP || strlen(call->arguments)>=AGENT_ARGUMENT_CAP ||
       json_parse(call->arguments,strlen(call->arguments),tokens,128)<1 || tokens[0].type!=JSON_OBJECT) {
        fail(out,cap,"ARGUMENTS","Tool arguments must be a bounded JSON object.",0);return -1;
    }
    if(mutation && string_arg(call->arguments,tokens,"path",path,sizeof(path))>=0 && evidence_path(path)) {
        fail(out,cap,"EXECUTION_EVIDENCE_READ_ONLY","Worker queue and snapshots are read-only to source tools.",0);return -1;
    }
    return 0;
}
static unsigned long hash_more(unsigned long h,const char *bytes,long n)
{
    long i;for(i=0;i<n;i++)h=((h^(unsigned char)bytes[i])*16777619UL)&0xffffffffUL;
    return h;
}
static void revision_hash(const CInfoPBRec *pb,long size,unsigned long h,char *out,size_t cap)
{
    snprintf(out,cap,"full-%08lx-%08lx-%08lx-%08lx",(unsigned long)pb->hFileInfo.ioDirID,
        (unsigned long)pb->hFileInfo.ioFlMdDat,(unsigned long)size,h);
}
static struct {
    FSSpec spec; CInfoPBRec original;
    short ref; long size,offset,base,begin;
    int phase,whole,editable,start,maximum,line;
    uint32_t started; unsigned long hash;
    char revision[80],quoted[AGENT_RESULT_CAP];
    size_t quoted_size; long end; int emitted;
} text_read;
static int read_finish(char *out,size_t cap,const char *code,int native,int stop)
{
    OSErr closed=0;
    if(text_read.ref>=0) { closed=FSClose(text_read.ref);text_read.ref=-1; }
    text_active=0;
    if(code || closed)fail(out,cap,closed ? "CLOSE" : code,"Read failed; no editable revision supplied.",native ? native : closed);
    return stop || closed ? 1 : 0;
}
int read_text_begin(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *context,uint32_t now)
{
    if(cap<AGENT_RESULT_CAP) { if(cap)out[0]=0;return 1; }
    cap=AGENT_RESULT_CAP;
    JsonToken tokens[128]; char path[512]; OSErr err;
    (void)journal;(void)context;
    if(text_active) { fail(out,cap,"TEXT_BUSY","A text operation is already active.",0);return 1; }
    if(text_arguments(call,tokens,out,cap,0))return 0;
    memset(&text_read,0,sizeof(text_read));text_read.ref=-1;
    text_read.start=int_arg(call->arguments,tokens,"start_line",1,1,100000);
    text_read.maximum=int_arg(call->arguments,tokens,"max_lines",20,1,30);
    text_read.base=int_arg(call->arguments,tokens,"start_byte",0,0,2147483647);
    if(valid_keys(call->arguments,tokens,"|path||start_byte||start_line||max_lines|") ||
       string_arg(call->arguments,tokens,"path",path,sizeof(path))<0 || text_read.start<0 ||
       text_read.maximum<0 || text_read.base<0 || (text_read.base && text_read.start!=1)) {
        fail(out,cap,"ARGUMENTS","Expected path, optional start_line or start_byte, and max_lines.",0);return 0;
    }
    err=tools_resolve(path,&text_read.spec);if(!err)err=catalog(&text_read.spec,&text_read.original);
    if(err)return read_finish(out,cap,"FILE",err,0);
    if(!plain_file(&text_read.spec,&text_read.original))return read_finish(out,cap,"NOT_TEXT",0,0);
    if(text_read.base>text_read.original.hFileInfo.ioFlLgLen)return read_finish(out,cap,"RANGE",0,0);
    text_read.whole=text_read.original.hFileInfo.ioFlLgLen<=TOOLS_ACCEPTED_FILE_CAP;
    text_read.editable=text_read.whole;
    text_read.size=text_read.original.hFileInfo.ioFlLgLen-(text_read.whole ? 0 : text_read.base);
    if(!text_read.whole && text_read.size>8192)text_read.size=8192;
    err=FSpOpenDF(&text_read.spec,fsRdPerm,&text_read.ref);
    if(!err)err=SetFPos(text_read.ref,fsFromStart,text_read.whole ? 0 : text_read.base);
    if(err)return read_finish(out,cap,"READ",err,0);
    text_read.started=now;text_read.hash=2166136261UL;text_read.line=1;text_active=1;return 2;
}
static int read_page(char *out,size_t cap)
{
    char utf8[8],quoted[20],header[512],tail[160],line_info[100];
    long byte_base=text_read.whole ? 0 : text_read.base;
    int work=0;
    for(;;) {
        long end=text_read.end;
        int emitted=text_read.emitted,truncated,partial,n=0,q=0;
        size_t quoted_size=text_read.quoted_size;
        if(end<text_read.size && emitted<text_read.maximum) {
            n=text_source[end]==13 && end+1<text_read.size && text_source[end+1]==10 ? 2 : 1;
            if(work+n>TOOLS_WORK_CHUNK)return 2;
            if(text_to_utf8(text_source+end,(size_t)n,utf8,sizeof(utf8))<0 ||
                (q=json_quote(utf8,quoted,sizeof(quoted)))<0)return read_finish(out,cap,"ENCODING",0,0);
            q-=2;end+=n;quoted_size+=(size_t)q;
            if(text_source[end-1]==13 || text_source[end-1]==10)emitted++;
        }
        truncated=byte_base+end<text_read.original.hFileInfo.ioFlLgLen;
        partial=end>text_read.begin && text_source[end-1]!=13 && text_source[end-1]!=10 && truncated;
        if(text_read.base)strcpy(line_info,"\"start_line\":null,\"next_line\":null");
        else if(partial)snprintf(line_info,sizeof(line_info),"\"start_line\":%d,\"next_line\":null",text_read.start);
        else snprintf(line_info,sizeof(line_info),"\"start_line\":%d,\"next_line\":%d",text_read.start,
            text_read.start+emitted+(end>text_read.begin && text_source[end-1]!=13 && text_source[end-1]!=10));
        snprintf(header,sizeof(header),"{\"status\":\"ok\",\"encoding\":\"MacRoman\",\"revision\":\"%s\",\"revision_scope\":\"%s\",\"editable\":%s,%s,\"text\":\"",text_read.revision,
            text_read.whole ? "whole_file" : "scan",text_read.editable ? "true" : "false",line_info);
        snprintf(tail,sizeof(tail),"\",\"truncated\":%s,\"start_byte\":%ld,\"next_byte\":%ld,\"line_partial\":%s}",
            truncated ? "true" : "false",byte_base+text_read.begin,byte_base+end,partial ? "true" : "false");
        if(strlen(header)+quoted_size+strlen(tail)>=cap) {
            if(!text_read.quoted_size)return read_finish(out,cap,"LIMIT",0,0);
            /* Reformat the already accepted prefix without adding a byte. */
            text_read.maximum=text_read.emitted;
            continue;
        }
        if(n) {
            memcpy(text_read.quoted+text_read.quoted_size,quoted+1,(size_t)q);
            text_read.quoted_size=quoted_size;text_read.quoted[quoted_size]=0;
            text_read.end=end;text_read.emitted=emitted;work+=n;
        } else {
            size_t at=0;
            if(append(out,cap,&at,header) || append(out,cap,&at,text_read.quoted) || append(out,cap,&at,tail))
                return read_finish(out,cap,"LIMIT",0,0);
            text_active=0;return 0;
        }
    }
}
int read_text_step(char *out,size_t cap,uint32_t now,int stop)
{

    long n,got,i; OSErr err; CInfoPBRec after;
    if(text_active!=1) { fail(out,cap,"NO_ACTIVE_READ","No active text read.",0);return 1; }
    if(cap<AGENT_RESULT_CAP)return read_finish(out,cap,"LIMIT",0,1);
    cap=AGENT_RESULT_CAP;
    if(stop || (uint32_t)(now-text_read.started)>=60UL*60UL)return read_finish(out,cap,stop ? "STOPPED" : "DEADLINE",0,1);
    n=text_read.size-text_read.offset;if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
    if(text_read.phase<2) {
        got=n;
        err=FSRead(text_read.ref,&got,text_read.phase ? text_scratch : text_source+text_read.offset);
        if(err || got!=n)return read_finish(out,cap,"READ",err ? err : ioErr,0);
        if(text_read.phase) {
            if(memcmp(text_scratch,text_source+text_read.offset,(size_t)n))return read_finish(out,cap,"CHANGED",0,0);
        } else {
            for(i=0;i<n;i++) {
                unsigned char c=(unsigned char)text_source[text_read.offset+i];
                if(!c || c==127 || (c<32 && c!=9 && c!=10 && c!=13))return read_finish(out,cap,"NOT_TEXT",0,0);
                if(c==10)text_read.editable=0;
            }
            text_read.hash=hash_more(text_read.hash,text_source+text_read.offset,n);
        }
        text_read.offset+=n;
        if(text_read.offset<text_read.size)return 2;
        if(text_read.whole && !text_read.phase) {
            text_read.offset=0;text_read.phase=1;
            err=SetFPos(text_read.ref,fsFromStart,0);
            if(err)return read_finish(out,cap,"READ",err,0);
            return 2;
        }
        err=catalog(&text_read.spec,&after);
        if(err || !same_file(&text_read.original,&after))return read_finish(out,cap,"CHANGED",err,0);
        if(read_finish(out,cap,NULL,0,0))return 1;
        if(text_read.whole)revision_hash(&after,text_read.size,text_read.hash,text_read.revision,sizeof(text_read.revision));
        else snprintf(text_read.revision,sizeof(text_read.revision),"scan-%08lx-%08lx-%08lx",(unsigned long)after.hFileInfo.ioFlMdDat,(unsigned long)after.hFileInfo.ioFlLgLen,text_read.hash);
        text_read.begin=text_read.whole ? text_read.base : 0;
        text_read.phase=2;text_active=1;return 2;
    }
    if(text_read.phase==3)return read_page(out,cap);
    /* Line navigation over the verified snapshot is cooperative as well. */
    for(i=0;i<TOOLS_WORK_CHUNK && text_read.begin<text_read.size && text_read.line<text_read.start;i++) {
        long b=text_read.begin++;
        if(text_source[b]==13 || (text_source[b]==10 && (!b || text_source[b-1]!=13)))text_read.line++;
    }
    if(text_read.line<text_read.start) {
        if(text_read.begin<text_read.size)return 2;
        return read_finish(out,cap,"RANGE",0,0);
    }
    if(text_read.begin && text_read.begin<text_read.size && text_source[text_read.begin]==10 && text_source[text_read.begin-1]==13)text_read.begin++;
    text_read.end=text_read.begin;text_read.phase=3;return 2;
}
/* AGENTS.md: one bounded whole-file read of <folder>:AGENTS.md with the same
 * plain-text rules as read_text. Absence is ordinary and silent; anything
 * present but unusable is reported so the app can say it was skipped. */
int tools_read_instructions(const char *folder, char *out, size_t cap, unsigned long *hash)
{
    static char bytes[AGENT_INSTRUCTIONS_CAP + 1], utf8[AGENT_INSTRUCTIONS_CAP * 3 + 1];
    char path[256], marker[64];
    FSSpec spec;
    CInfoPBRec pb, after;
    short ref;
    OSErr err, closed;
    long size, wanted;
    size_t i, limit;
    int n, markerlen, cut = 0;
    unsigned long sum;
    if (cap) out[0] = 0;
    if (hash) *hash = 0;
    if (cap < AGENT_INSTRUCTIONS_CAP + 1) return TOOLS_INSTRUCTIONS_UNUSABLE;
    if (snprintf(path, sizeof(path), "%s%sAGENTS.md", folder, *folder ? ":" : "") >= (int)sizeof(path)) return TOOLS_INSTRUCTIONS_ABSENT;
    if (spec_for(path, 0, &spec)) return TOOLS_INSTRUCTIONS_ABSENT;
    if (catalog(&spec, &pb)) return TOOLS_INSTRUCTIONS_ABSENT;
    if (!plain_file(&spec, &pb)) return TOOLS_INSTRUCTIONS_UNUSABLE;
    if (!pb.hFileInfo.ioFlLgLen) return TOOLS_INSTRUCTIONS_ABSENT;
    size = pb.hFileInfo.ioFlLgLen;
    if (size > AGENT_INSTRUCTIONS_CAP) { size = AGENT_INSTRUCTIONS_CAP; cut = 1; }
    wanted = size;
    if (FSpOpenDF(&spec, fsRdPerm, &ref)) return TOOLS_INSTRUCTIONS_UNUSABLE;
    err = FSRead(ref, &size, bytes); closed = FSClose(ref);
    if (err || closed || size != wanted) return TOOLS_INSTRUCTIONS_UNUSABLE;
    sum = hash_bytes(bytes, size);
    for (i = 0; i < (size_t)size; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (!c || c == 127 || (c < 32 && c != 9 && c != 10 && c != 13)) return TOOLS_INSTRUCTIONS_UNUSABLE;
    }
    if (catalog(&spec, &after) || !same_file(&pb, &after)) return TOOLS_INSTRUCTIONS_UNUSABLE;
    n = text_to_utf8(bytes, (size_t)size, utf8, sizeof(utf8));
    if (n < 0) return TOOLS_INSTRUCTIONS_UNUSABLE;
    if (hash) *hash = sum;
    if (n > AGENT_INSTRUCTIONS_CAP) cut = 1;
    if (!cut) { memcpy(out, utf8, (size_t)n + 1); return TOOLS_INSTRUCTIONS_LOADED; }
    /* Keep whole lines and whole characters, and say what was left out. */
    markerlen = snprintf(marker, sizeof(marker), "\n[AGENTS.md truncated at %d bytes]", AGENT_INSTRUCTIONS_CAP);
    limit = AGENT_INSTRUCTIONS_CAP - (size_t)markerlen;
    if (limit > (size_t)n) limit = (size_t)n;
    while (limit && ((unsigned char)utf8[limit] & 0xC0) == 0x80) limit--;
    if (limit < (size_t)n ? utf8[limit] != '\n' : utf8[n - 1] != '\n') {
        size_t line = limit;
        while (line && utf8[line - 1] != '\n') line--;
        if (line) limit = line;
    }
    while (limit && utf8[limit - 1] == '\n') limit--;
    memcpy(out, utf8, limit); memcpy(out + limit, marker, (size_t)markerlen + 1);
    return TOOLS_INSTRUCTIONS_TRUNCATED;
}
int tools_call_project(const AgentCall *call, char *name, size_t cap)
{
    JsonToken tokens[128];
    char value[512];
    size_t n;
    int key;
    if (cap) name[0] = 0;
    if (json_parse(call->arguments, strlen(call->arguments), tokens, 128) < 1 || tokens[0].type != JSON_OBJECT) return 0;
    key = json_member(call->arguments, tokens, 0, "path");
    if (key < 0) key = json_member(call->arguments, tokens, 0, "root");
    if (key < 0 || json_string(call->arguments, tokens, key, value, sizeof(value)) < 0) return 0;
    for (n = 0; value[n] && value[n] != ':'; n++) {
        unsigned char c = (unsigned char)value[n];
        if (c < 32 || c == 127 || c == '/' || c == '\\') return 0;
    }
    if (!n || n >= cap) return 0;
    memcpy(name, value, n); name[n] = 0; return 1;
}
/* The configured workspace root as a non-alias folder. App-internal bootstrap
 * operations that create fixed children (the build queue) start here. */
OSErr tools_workspace_root(FSSpec *spec)
{
    CInfoPBRec pb;
    OSErr err = spec_for("", 1, spec);
    if (!err) err = catalog(spec, &pb);
    if (!err && (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))) err = paramErr;
    return err;
}
/* Resolve each existing ancestor by directory ID, never through an alias.
 * Only fnfErr for the final leaf is a valid create destination. */
OSErr tools_resolve(const char *path, FSSpec *spec)
{
    char local[256], *part, *colon;
    FSSpec parent;
    CInfoPBRec pb;
    Str255 name;
    OSErr err;
    long dir;
    if (text_to_macroman_strict(path, local, sizeof(local)) < 0 || tools_validate_path(local, 0)) return paramErr;
    err = spec_for("", 1, &parent);
    if (!err) err = catalog(&parent, &pb);
    if (err) return err == fnfErr ? dirNFErr : err;
    if (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000)) return paramErr;
    dir = pb.dirInfo.ioDrDirID;
    part = local;
    while ((colon = strchr(part, ':')) != NULL) {
        size_t n = (size_t)(colon - part);
        name[0] = (unsigned char)n; memcpy(name + 1, part, n);
        err = FSMakeFSSpec(parent.vRefNum, dir, name, spec);
        if (!err) err = catalog(spec, &pb);
        if (err) return err == fnfErr ? dirNFErr : err;
        if (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000)) return paramErr;
        dir = pb.dirInfo.ioDrDirID; part = colon + 1;
    }
    name[0] = (unsigned char)strlen(part); memcpy(name + 1, part, name[0]);
    return FSMakeFSSpec(parent.vRefNum, dir, name, spec);
}
/* Bounded depth-first catalog walk for a directory ID, filling its relative
 * MacRoman path with a trailing colon. 512 entries or depth 8, then reports
 * incompleteness rather than guessing. Shared with alias resolution. */
int tools_find_dir(const FSSpec *spec, long dir, const char *prefix, long wanted,
                   char *found, size_t cap, int depth, int *budget, int *complete)
{
    int index;
    for (index = 1; *budget > 0; index++) {
        CInfoPBRec pb;
        Str255 name;
        OSErr err;
        char child[768];
        size_t plen = strlen(prefix);
        memset(&pb, 0, sizeof(pb)); name[0] = 0;
        pb.hFileInfo.ioNamePtr = name; pb.hFileInfo.ioVRefNum = spec->vRefNum;
        pb.hFileInfo.ioDirID = dir; pb.hFileInfo.ioFDirIndex = (short)index;
        err = PBGetCatInfoSync(&pb);
        if (err == fnfErr) return 0;
        if (err) { *complete = 0; return 0; }
        (*budget)--;
        if (!(pb.hFileInfo.ioFlAttrib & 16)) continue;
        if (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) continue;
        if (name[0] > 31 || plen + (size_t)name[0] + 2 > sizeof(child)) { *complete = 0; continue; }
        memcpy(child, prefix, plen); memcpy(child + plen, name + 1, name[0]);
        child[plen + name[0]] = ':'; child[plen + name[0] + 1] = 0;
        if (pb.dirInfo.ioDrDirID == wanted) { snprintf(found, cap, "%s", child); return 1; }
        if (depth < 8 && tools_find_dir(spec, pb.dirInfo.ioDrDirID, child, wanted, found, cap, depth + 1, budget, complete))
            return 1;
    }
    *complete = 0;
    return 0;
}
/* Stateless depth-first continuation. Catalog order must stay unchanged between
 * pages. Each call examines at most 64 entries and reads at most 8192 bytes;
 * the agent services Stop before dispatching the next bounded call. */
static void search_text(const char *s, const JsonToken *tokens, char *out, size_t cap)
{
    static char bytes[4224];
    char root[512], local[256], query[512], needle[129], cursor[256] = "";
    char paths[9][256], path[768], utf8[400], entry[1250], tail[400];
    long dirs[9], indices[9] = {1}, offset = 0, line = 1, prevcr = 0;
    int depth = 0, recursive = 0, limit, count = 0, steps = 0, budget = 8192, done = 0, skipped = 0;
    size_t at = 0, qlen;
    FSSpec spec;
    CInfoPBRec pb;
    OSErr err;
    int member = json_member(s, tokens, 0, "recursive");
    limit = int_arg(s,tokens,"limit",4,1,8);
    if (member >= 0) {
        int n = tokens[member].end - tokens[member].start;
        if (tokens[member].type != JSON_PRIMITIVE ||
            !((n == 4 && !memcmp(s+tokens[member].start,"true",4)) ||
              (n == 5 && !memcmp(s+tokens[member].start,"false",5)))) goto arguments;
        recursive = n == 4;
    }
    if (valid_keys(s,tokens,"|root||query||recursive||limit||cursor|") || limit < 0 ||
        string_arg(s,tokens,"root",root,sizeof(root)) < 0 ||
        string_arg(s,tokens,"query",query,sizeof(query)) < 0 ||
        text_to_macroman_strict(root,local,sizeof(local)) < 0 || tools_validate_path(local,1) ||
        text_to_macroman_strict(query,needle,sizeof(needle)) <= 0) goto arguments;
    qlen = strlen(needle);
    { size_t i; for(i=0;i<qlen;i++) if((unsigned char)needle[i]<32 || needle[i]==127) goto arguments; }
    if (json_member(s,tokens,0,"cursor") >= 0 && string_arg(s,tokens,"cursor",cursor,sizeof(cursor))<0) goto arguments;
    if (*cursor) {
        char *p, *end;
        long values[14]; int n=0;
        p=cursor;
        while(*p && n<14) {
            if(*p<'0' || *p>'9') goto arguments;
            values[n]=0;end=p;
            while(*end>='0' && *end<='9') {
                int digit=*end-'0';
                if(values[n]>(2147483647L-digit)/10)goto arguments;
                values[n]=values[n]*10+digit;end++;
            }
            n++;
            if(!*end) {p=end;break;}
            if(*end!=':') goto arguments;
            p=end+1;if(!*p)goto arguments;
        }
        if(*p || n<5 || values[0]>8 || n!=values[0]+5) goto arguments;
        depth=(int)values[0]; if(depth && !recursive) goto arguments;
        {int i;for(i=0;i<=depth;i++) {indices[i]=values[i+1];if(indices[i]<1 || indices[i]>30001)goto arguments;}}
        offset=values[depth+2];line=values[depth+3];prevcr=values[depth+4];
        if(line<1 || prevcr>1) goto arguments;
    }
    /* Resolve each root ancestor by ID, refusing alias folders. */
    if(*local && local[strlen(local)-1]==':') local[strlen(local)-1]=0;
    if(*root && root[strlen(root)-1]==':') root[strlen(root)-1]=0;
    if(*local) err=tools_resolve(root,&spec);
    else err=spec_for("",1,&spec);
    if(!err)err=catalog(&spec,&pb);
    if(err || !(pb.hFileInfo.ioFlAttrib&16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags&0x8000)) {
        fail(out,cap,"FOLDER","Cannot resolve a non-alias workspace folder.",err);return;
    }
    dirs[0]=pb.dirInfo.ioDrDirID;
    snprintf(paths[0],sizeof(paths[0]),"%s%s",local,*local ? ":" : "");
    {int d;for(d=0;d<depth;d++) {
        Str255 name;
        memset(&pb,0,sizeof(pb));pb.hFileInfo.ioNamePtr=name;pb.hFileInfo.ioVRefNum=spec.vRefNum;
        pb.hFileInfo.ioDirID=dirs[d];pb.hFileInfo.ioFDirIndex=(short)indices[d];
        err=PBGetCatInfoSync(&pb);
        if(err || !(pb.hFileInfo.ioFlAttrib&16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags&0x8000) ||
            name[0]>31 || strlen(paths[d])+name[0]+1>180) {fail(out,cap,"CURSOR","Search tree changed or cursor is invalid.",err);return;}
        memcpy(paths[d+1],paths[d],strlen(paths[d]));
        memcpy(paths[d+1]+strlen(paths[d]),name+1,name[0]);
        paths[d+1][strlen(paths[d])+name[0]]=':';paths[d+1][strlen(paths[d])+name[0]+1]=0;
        dirs[d+1]=pb.dirInfo.ioDrDirID;
    }}
    append(out,cap,&at,"{\"status\":\"ok\",\"matches\":[");
    while(steps<64 && budget>128 && count<limit) {
        Str255 name;
        FSSpec file;
        long size, wanted, process, i;
        short ref;
        int binary=0;
        size_t plen;
        if(indices[depth]>30000){fail(out,cap,"LIMIT","Folder exceeds 30000 catalog entries; narrow the search root.",0);return;}
        memset(&pb,0,sizeof(pb));pb.hFileInfo.ioNamePtr=name;pb.hFileInfo.ioVRefNum=spec.vRefNum;
        pb.hFileInfo.ioDirID=dirs[depth];pb.hFileInfo.ioFDirIndex=(short)indices[depth];steps++;
        err=PBGetCatInfoSync(&pb);
        if(err==fnfErr) {
            if(!depth){done=1;break;} depth--;indices[depth]++;offset=0;line=1;prevcr=0;continue;
        }
        if(err){fail(out,cap,"CATALOG","Search enumeration failed.",err);return;}
        plen=strlen(paths[depth]);
        if(name[0]>31 || plen+name[0]>179 || (pb.hFileInfo.ioFlFndrInfo.fdFlags&0x8000)) goto skip;
        memcpy(local,paths[depth],plen);memcpy(local+plen,name+1,name[0]);local[plen+name[0]]=0;
        if(tools_validate_path(local,(pb.hFileInfo.ioFlAttrib&16)!=0))goto skip;
        if(pb.hFileInfo.ioFlAttrib&16) {
            if(recursive) {
                if(depth==8) goto skip;
                memcpy(paths[depth+1],paths[depth],plen);memcpy(paths[depth+1]+plen,name+1,name[0]);
                paths[depth+1][plen+name[0]]=':';paths[depth+1][plen+name[0]+1]=0;
                dirs[depth+1]=pb.dirInfo.ioDrDirID;depth++;indices[depth]=1;continue;
            }
            indices[depth]++;continue;
        }
        file.vRefNum=spec.vRefNum;file.parID=dirs[depth];memcpy(file.name,name,(size_t)name[0]+1);
        if(!plain_file(&file,&pb)) goto skip;
        if(offset>pb.hFileInfo.ioFlLgLen){fail(out,cap,"CURSOR","File shortened since search page.",0);return;}
        process=budget-(long)qlen+1;if(process>4096)process=4096;
        wanted=pb.hFileInfo.ioFlLgLen-offset;if(wanted>process+(long)qlen-1)wanted=process+(long)qlen-1;
        size=wanted;
        err=FSpOpenDF(&file,fsRdPerm,&ref);
        if(err){fail(out,cap,"READ","Cannot open search file.",err);return;}
        err=SetFPos(ref,fsFromStart,offset);if(!err && size)err=FSRead(ref,&size,bytes);
        {OSErr closed=FSClose(ref);if(!err)err=closed;}
        if(err || size!=wanted){fail(out,cap,"READ","Search read failed or was short.",err);return;}
        {CInfoPBRec after;err=catalog(&file,&after);if(err || !same_file(&pb,&after)){fail(out,cap,"CHANGED","File changed during search; restart search.",err);return;}}
        budget-=(int)size;
        for(i=0;i<size;i++){unsigned char c=(unsigned char)bytes[i];if(!c || c==127 || (c<32 && c!=9 && c!=10 && c!=13))binary=1;}
        if(binary) goto skip;
        if(process>size)process=size;
        for(i=0;i<process;i++) {
            if(i+(long)qlen<=size && !memcmp(bytes+i,needle,qlen)) {
                size_t pos=0;long end=i;char meta[160];
                while(end<size && end-i<32 && bytes[end]!=13 && bytes[end]!=10)end++;
                memcpy(local,paths[depth],plen);memcpy(local+plen,name+1,name[0]);local[plen+name[0]]=0;
                if(text_to_utf8(local,strlen(local),path,sizeof(path))<0 || text_to_utf8(bytes+i,(size_t)(end-i),utf8,sizeof(utf8))<0)goto arguments;
                if(append(entry,sizeof(entry),&pos,"{\"path\":") || quote(entry,sizeof(entry),&pos,path)) {fail(out,cap,"LIMIT","Match path exceeds result capacity.",0);return;}
                snprintf(meta,sizeof(meta),",\"line\":%ld,\"byte\":%ld,\"excerpt\":",line,offset+i);
                if(append(entry,sizeof(entry),&pos,meta) || quote(entry,sizeof(entry),&pos,utf8) || append(entry,sizeof(entry),&pos,"}")) {fail(out,cap,"LIMIT","Match exceeds result capacity.",0);return;}
                if(at+pos+280>=cap) {if(!count){fail(out,cap,"LIMIT","Match exceeds result capacity.",0);return;}break;}
                if(count)append(out,cap,&at,",");
                append(out,cap,&at,entry);count++;
            }
            if(bytes[i]==13 || (bytes[i]==10 && !prevcr)) {
                if(line>=2147483647L){fail(out,cap,"LIMIT","Line count exceeds search capacity.",0);return;}
                line++;
            }
            prevcr=bytes[i]==13;
            if(count==limit){i++;break;}
        }
        offset+=i;
        if(offset>=pb.hFileInfo.ioFlLgLen){indices[depth]++;offset=0;line=1;prevcr=0;}
        if(i<process)break;
        continue;
skip:
        skipped++;indices[depth]++;offset=0;line=1;prevcr=0;
    }
    cursor[0]=0;
    if(!done) {
        size_t pos=0;int d;
        pos+=(size_t)snprintf(cursor,sizeof(cursor),"%d",depth);
        for(d=0;d<=depth;d++)pos+=(size_t)snprintf(cursor+pos,sizeof(cursor)-pos,":%ld",indices[d]);
        snprintf(cursor+pos,sizeof(cursor)-pos,":%ld:%ld:%ld",offset,line,prevcr);
    }
    snprintf(tail,sizeof(tail),"],\"truncated\":%s,\"next_cursor\":%s%s%s,\"skipped\":%d,\"scanned_bytes\":%d}",
        done ? "false" : "true",done ? "" : "\"",done ? "null" : cursor,done ? "" : "\"",skipped,8192-budget);
    if(append(out,cap,&at,tail))fail(out,cap,"LIMIT","Search result exceeds capacity.",0);
    return;
arguments:
    fail(out,cap,"ARGUMENTS","Expected root, nonempty single-line MacRoman query (128 bytes), optional recursive, limit (1-8), and returned cursor.",0);
}

static int mutation_result(char *out, size_t cap, const char *status, const char *code,
                           const char *path, const char *temporary, long bytes,
                           const char *revision, int native)
{
    char qp[1100], qt[1100];
    int n;
    if (json_quote(path, qp, sizeof(qp)) < 0 || json_quote(temporary, qt, sizeof(qt)) < 0) return -1;
    n = snprintf(out, cap, "{\"status\":\"%s\",\"code\":\"%s\",\"path\":%s,\"temporary_path\":%s,"
        "\"data_bytes\":%ld,\"encoding\":\"MacRoman\",\"line_endings\":\"CR\",\"finder_type\":\"TEXT\","
        "\"revision\":\"%s\",\"os_error\":%d}", status, code, qp, qt, bytes, revision, native);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
/* The one place a mutation record is wrapped in its call envelope and handed to
 * the session journal. Nonzero means the record was not durably accepted,
 * including an envelope that would not fit. */
static int journal_mutation(AgentJournal journal, void *context, const char *event,
                            const char *call_id, const char *record)
{
    static char envelope[AGENT_RESULT_CAP + 1000];
    int n = snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (n < 0 || (size_t)n >= sizeof(envelope)) return 1;
    return journal(context, event, envelope);
}
static int write_text(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                      AgentJournal journal, void *context)
{
    static char utf8[AGENT_ARGUMENT_CAP], bytes[TOOLS_STRING_CAP+1], observed[4097];
    static char record[AGENT_RESULT_CAP]; /* static: the application stack is small */
    char path[512], temporary[768], tempname[32], revision[80] = "";
    char call_id[800];
    FSSpec target, stage;
    CInfoPBRec pb;
    OSErr err, closed;
    short ref;
    int length, attempt;
    long count;
    size_t i, prefix;
    unsigned long hash = 2166136261UL;
    if (valid_keys(call->arguments, tokens, "|path||text|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0 ||
        string_arg(call->arguments, tokens, "text", utf8, sizeof(utf8)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected only path and text strings.", 0); return 0;
    }
    length = text_to_macroman_strict(utf8, bytes, sizeof(bytes));
    if (length < 0) { fail(out, cap, "ENCODING_LIMIT", "Text must be representable in MacRoman and at most 4096 encoded bytes.", 0); return 0; }
    for (i = 0; i < (size_t)length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if ((c < 32 && c != 9 && c != 13) || c == 127) { fail(out, cap, "NOT_TEXT", "Binary control bytes are refused.", 0); return 0; }
        hash = ((hash ^ c) * 16777619UL) & 0xffffffffUL;
    }
    err = tools_resolve(path, &target);
    if (!err) { fail(out, cap, "EXISTS", "Destination exists; write_text never overwrites.", 0); return 0; }
    if (err != fnfErr) { fail(out, cap, "PATH", "Use a relative workspace file path with existing non-alias parent folders.", err); return 0; }
    if (!journal) { fail(out, cap, "JOURNAL", "Creating text requires a durable session journal.", 0); return 1; }
    if (json_quote(call->id, call_id, sizeof(call_id)) < 0) { fail(out, cap, "JOURNAL", "Cannot encode call identity.", 0); return 1; }
    prefix = strrchr(path, ':') ? (size_t)(strrchr(path, ':') - path + 1) : 0;
    for (attempt = 0; attempt < 100; attempt++) {
        extern unsigned long TickCount(void);
        snprintf(tempname, sizeof(tempname), "Sherclawk tmp %08lx %02x", (unsigned long)TickCount() & 0xffffffffUL, attempt);
        stage = target; stage.name[0] = (unsigned char)strlen(tempname); memcpy(stage.name + 1, tempname, stage.name[0]);
        err = catalog(&stage, &pb);
        if (!err) continue;
        if (err != fnfErr) { fail(out, cap, "STAGE", "Cannot inspect temporary destination.", err); return 0; }
        snprintf(temporary, sizeof(temporary), "%.*s%s", (int)prefix, path, tempname);
        snprintf(revision, sizeof(revision), "00000000-%08lx-%08lx", (unsigned long)length, hash);
        if (mutation_result(record, sizeof(record), "pending", "CREATE_ONLY", path, temporary, length, revision, 0)) {
            fail(out, cap, "LIMIT", "Cannot encode recovery paths; no file created.", 0); return 0;
        }
        if (journal_mutation(journal, context, "mutation_intent", call_id, record)) { fail(out, cap, "JOURNAL", "Cannot record mutation intent; no file created.", 0); return 1; }
        err = FSpCreate(&stage, 'ttxt', 'TEXT', smSystemScript);
        if (err == dupFNErr) continue;
        break;
    }
    if (attempt == 100) { fail(out, cap, "STAGE", "Temporary name collisions; no destination created.", 0); return 0; }
    if (err) goto failed;
    err = FSpOpenDF(&stage, fsWrPerm, &ref);
    if (err) goto failed;
    count = length; err = FSWrite(ref, &count, bytes); closed = FSClose(ref);
    if (!err && count != length) err = ioErr;
    if (!err) err = closed;
    if (err) goto failed;
    err = FlushVol(NULL, stage.vRefNum); if (err) goto failed;
    err = catalog(&stage, &pb);
    if (!err && (pb.hFileInfo.ioFlLgLen != length || pb.hFileInfo.ioFlRLgLen || pb.hFileInfo.ioFlFndrInfo.fdType != 'TEXT')) err = ioErr;
    if (err) goto failed;
    err = FSpOpenDF(&stage, fsRdPerm, &ref); if (err) goto failed;
    count = length; err = FSRead(ref, &count, observed); closed = FSClose(ref);
    if (!err && (count != length || memcmp(bytes, observed, (size_t)length))) err = ioErr;
    if (!err) err = closed;
    if (err) goto failed;
    full_revision(&pb, bytes, length, revision, sizeof(revision));
    mutation_result(record, sizeof(record), "staged", "CREATE_ONLY", path, temporary, length, revision, 0);
    if (journal_mutation(journal, context, "mutation_staged", call_id, record)) {
        mutation_result(out, cap, "error", "JOURNAL_STAGE_RETAINED", path, temporary, length, revision, 0); return 1;
    }
    /* HFS/AFP rename refuses a colliding name, including a late race. */
    err = FSpRename(&stage, target.name);
    if (err == dupFNErr) {
        mutation_result(out, cap, "error", "EXISTS_STAGE_RETAINED", path, temporary, length, revision, err); return 0;
    }
    if (err) goto uncertain;
    err = FlushVol(NULL, target.vRefNum); if (err) goto uncertain;
    err = catalog(&target, &pb); if (err) goto uncertain;
    full_revision(&pb, bytes, length, revision, sizeof(revision));
    mutation_result(out, cap, "ok", "CREATED", path, "", length, revision, 0);
    if (journal_mutation(journal, context, "mutation_committed", call_id, out)) {
        mutation_result(out, cap, "uncertain", "JOURNAL_AFTER_PUBLISH", path, temporary, length, revision, 0); return 1;
    }
    return 0;
failed:
    mutation_result(out, cap, "error", "STAGE_FAILED_INSPECT_TEMP", path, temporary, length, revision, err);
    return 0;
uncertain:
    mutation_result(out, cap, "uncertain", "PUBLISH_INSPECT_PATHS", path, temporary, length, revision, err);
    return 1;
}
static int edit_result(char *out, size_t cap, const char *status, const char *code,
                       const char *path, const char *temporary, const char *backup,
                       long bytes, const char *revision, const char *previous, int native)
{
    char qp[1100], qt[1100], qb[1100];
    int n;
    if (json_quote(path, qp, sizeof(qp)) < 0 || json_quote(temporary, qt, sizeof(qt)) < 0 ||
        json_quote(backup, qb, sizeof(qb)) < 0) return -1;
    n = snprintf(out, cap, "{\"status\":\"%s\",\"code\":\"%s\",\"path\":%s,\"temporary_path\":%s,"
        "\"backup_path\":%s,\"data_bytes\":%ld,\"revision\":\"%s\",\"previous_revision\":\"%s\",\"os_error\":%d}",
        status, code, qp, qt, qb, bytes, revision, previous, native);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
/* CR text only: normalize model strings, never silently convert unrelated
 * source bytes. Hold an exclusive File Manager open on the original through
 * publication. Rename is collision-safe but not a two-file transaction on AFP:
 * journal both sibling paths before moving anything, retain the original,
 * recheck its identity/bytes after rename, and stop on any uncertain outcome.
 * External POSIX writers bypass AFP locks; never edit concurrently that way. */
/* Every phase advances at most one transfer or a bounded comparison budget.
 * The original remains exclusively open through both renames. */
enum TextEditPhase { E_READ, E_REWIND, E_INITIAL_VERIFY, E_PREFIX, E_MATCH,
    E_COPY, E_NAME, E_WRITE, E_STAGE_OPEN, E_STAGE_VERIFY, E_STAGED,
    E_ORIGINAL_OPEN, E_ORIGINAL_VERIFY, E_BACKUP_RENAME, E_BACKUP_OPEN,
    E_BACKUP_VERIFY, E_BACKED_UP, E_PUBLISH, E_PUBLISHED_OPEN,
    E_PUBLISHED_VERIFY, E_COMMIT };
static struct {
    FSSpec target,stage,backup; CInfoPBRec original,staged;
    short ref,aux; enum TextEditPhase phase;
    long size,length,offset,found; int old_len,new_len,matched,matches,attempt,renamed,retained,changed;
    int prefix_at,prefix_match; unsigned short prefix[TOOLS_STRING_CAP];
    unsigned long source_hash,edited_hash; uint32_t started;
    AgentJournal journal; void *context;
    char path[512],temporary[768],backup_path[768],expected[80],previous[80],revision[80],call_id[800];
    char old[TOOLS_STRING_CAP+1],replacement[TOOLS_STRING_CAP+1];
} text_edit;
static int edit_finish(char *out,size_t cap,const char *code,int native,int stop)
{
    OSErr c;
    if(text_edit.aux>=0) { c=FSClose(text_edit.aux);text_edit.aux=-1;if(c) { native=c;stop=1; } }
    if(text_edit.ref>=0) { c=FSClose(text_edit.ref);text_edit.ref=-1;if(c) { native=c;stop=1; } }
    text_active=0;
    if(text_edit.retained || text_edit.renamed)
        edit_result(out,cap,text_edit.renamed ? "uncertain" : "error",code,text_edit.path,text_edit.temporary,
                    text_edit.backup_path,text_edit.length,text_edit.revision,text_edit.previous,native);
    else fail(out,cap,code,"Edit refused; source unchanged. Requires a current whole-file revision and one unique exact match.",native);
    return stop || text_edit.renamed ? 1 : 0;
}
static int edit_record(const char *event,const char *status)
{
    static char record[AGENT_RESULT_CAP];
    if(edit_result(record,sizeof(record),status,!strcmp(status,"ok") ? "EDITED" : "EXACT_EDIT",text_edit.path,!strcmp(status,"ok") ? "" : text_edit.temporary,
                   text_edit.backup_path,text_edit.length,text_edit.revision,text_edit.previous,0))return -1;
    return journal_mutation(text_edit.journal,text_edit.context,event,text_edit.call_id,record);
}
int edit_text_begin(const AgentCall *call,char *out,size_t cap,AgentJournal journal,void *context,uint32_t now)
{
    if(cap<AGENT_RESULT_CAP) { if(cap)out[0]=0;return 1; }
    cap=AGENT_RESULT_CAP;
    static char utf8[AGENT_ARGUMENT_CAP];JsonToken tokens[128];OSErr err;int i;
    if(text_active) { fail(out,cap,"TEXT_BUSY","A text operation is already active.",0);return 1; }
    if(text_arguments(call,tokens,out,cap,1))return 0;
    memset(&text_edit,0,sizeof(text_edit));text_edit.ref=text_edit.aux=-1;
    if(valid_keys(call->arguments,tokens,"|path||expected_revision||old_text||new_text|") ||
       string_arg(call->arguments,tokens,"path",text_edit.path,sizeof(text_edit.path))<0 ||
       string_arg(call->arguments,tokens,"expected_revision",text_edit.expected,sizeof(text_edit.expected))<0 ||
       strncmp(text_edit.expected,"full-",5) || string_arg(call->arguments,tokens,"old_text",utf8,sizeof(utf8))<0)
        return edit_finish(out,cap,"ARGUMENTS",0,0);
    text_edit.old_len=text_to_macroman_strict(utf8,text_edit.old,sizeof(text_edit.old));
    if(string_arg(call->arguments,tokens,"new_text",utf8,sizeof(utf8))<0)return edit_finish(out,cap,"ARGUMENTS",0,0);
    text_edit.new_len=text_to_macroman_strict(utf8,text_edit.replacement,sizeof(text_edit.replacement));
    if(text_edit.old_len<1 || text_edit.new_len<0)return edit_finish(out,cap,"ENCODING_LIMIT",0,0);
    for(i=0;i<text_edit.old_len+text_edit.new_len;i++) {
        unsigned char c=(unsigned char)(i<text_edit.old_len ? text_edit.old[i] : text_edit.replacement[i-text_edit.old_len]);
        if(c==127 || (c<32 && c!=9 && c!=13))return edit_finish(out,cap,"NOT_TEXT",0,0);
    }
    err=tools_resolve(text_edit.path,&text_edit.target);if(!err)err=catalog(&text_edit.target,&text_edit.original);
    if(err)return edit_finish(out,cap,"PATH",err,0);
    if(!plain_file(&text_edit.target,&text_edit.original))return edit_finish(out,cap,"NOT_TEXT",0,0);
    text_edit.size=text_edit.original.hFileInfo.ioFlLgLen;
    if(text_edit.size<0 || text_edit.size>TOOLS_ACCEPTED_FILE_CAP)return edit_finish(out,cap,"LIMIT",0,0);
    if(!journal || json_quote(call->id,text_edit.call_id,sizeof(text_edit.call_id))<0)return edit_finish(out,cap,"JOURNAL",0,1);
    err=FSpOpenDF(&text_edit.target,fsRdWrPerm,&text_edit.ref);
    if(err)return edit_finish(out,cap,"BUSY",err,0);
    text_edit.journal=journal;text_edit.context=context;text_edit.started=now;
    text_edit.source_hash=text_edit.edited_hash=2166136261UL;text_edit.prefix_at=1;
    text_active=2;return 2;
}
/* A verification phase compares exact retained bytes, never just hashes. */
static int edit_compare(short ref,const char *expected,long length)
{
    long n=length-text_edit.offset,got;OSErr err;
    if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
    got=n;err=FSRead(ref,&got,text_scratch);
    if(err || got!=n || memcmp(text_scratch,expected+text_edit.offset,(size_t)n))return err ? err : ioErr;
    text_edit.offset+=n;return 0;
}
int edit_text_step(char *out,size_t cap,uint32_t now,int stop)
{

    CInfoPBRec pb;OSErr err=0,c;long n,got,i;int budget;
    if(text_active!=2) { fail(out,cap,"NO_ACTIVE_EDIT","No active text edit.",0);return 1; }
    if(cap<AGENT_RESULT_CAP)return edit_finish(out,cap,"LIMIT",0,1);
    cap=AGENT_RESULT_CAP;
    if(stop || (uint32_t)(now-text_edit.started)>=60UL*60UL)
        return edit_finish(out,cap,stop ? "STOPPED_INSPECT_PATHS" : "DEADLINE_INSPECT_PATHS",0,1);
    switch(text_edit.phase) {
    case E_READ:
        n=text_edit.size-text_edit.offset;if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
        got=n;err=FSRead(text_edit.ref,&got,text_source+text_edit.offset);
        if(err || got!=n)return edit_finish(out,cap,"READ",err ? err : ioErr,0);
        for(i=0;i<n;i++) {
            unsigned char ch=(unsigned char)text_source[text_edit.offset+i];
            if(ch==127 || (ch<32 && ch!=9 && ch!=13))return edit_finish(out,cap,ch==10 ? "LINE_ENDINGS" : "NOT_TEXT",0,0);
        }
        text_edit.source_hash=hash_more(text_edit.source_hash,text_source+text_edit.offset,n);
        text_edit.offset+=n;if(text_edit.offset==text_edit.size)text_edit.phase=E_REWIND;
        break;
    case E_REWIND:
        err=catalog(&text_edit.target,&pb);
        if(!err && !same_file(&text_edit.original,&pb))err=ioErr;
        if(!err)err=SetFPos(text_edit.ref,fsFromStart,0);
        if(err)return edit_finish(out,cap,"CHANGED",err,0);
        text_edit.offset=0;text_edit.phase=E_INITIAL_VERIFY;break;
    case E_INITIAL_VERIFY:
        err=edit_compare(text_edit.ref,text_source,text_edit.size);
        if(err)return edit_finish(out,cap,"CHANGED",err,0);
        if(text_edit.offset<text_edit.size)break;
        err=catalog(&text_edit.target,&pb);
        if(err || !same_file(&text_edit.original,&pb))return edit_finish(out,cap,"CHANGED",err,0);
        revision_hash(&pb,text_edit.size,text_edit.source_hash,text_edit.previous,sizeof(text_edit.previous));
        if(strcmp(text_edit.previous,text_edit.expected))return edit_finish(out,cap,"REVISION_MISMATCH",0,0);
        text_edit.phase=E_PREFIX;break;
    case E_PREFIX:
        /* KMP prefix construction and matching each charge every comparison,
         * including fallback comparisons, against the same per-step budget. */
        budget=TOOLS_WORK_CHUNK;
        while(text_edit.prefix_at<text_edit.old_len && budget--) {
            int at=text_edit.prefix_at,m=text_edit.prefix_match;
            if(text_edit.old[at]==text_edit.old[m]) {
                text_edit.prefix_match=m+1;text_edit.prefix[at]=(unsigned short)(m+1);text_edit.prefix_at++;
            } else if(m)text_edit.prefix_match=text_edit.prefix[m-1];
            else { text_edit.prefix[at]=0;text_edit.prefix_at++; }
        }
        if(text_edit.prefix_at==text_edit.old_len) { text_edit.offset=0;text_edit.phase=E_MATCH; }
        break;
    case E_MATCH:
        budget=TOOLS_WORK_CHUNK;
        while(text_edit.offset<text_edit.size && budget--) {
            int m=text_edit.matched;
            if(text_source[text_edit.offset]==text_edit.old[m]) {
                text_edit.offset++;text_edit.matched++;
                if(text_edit.matched==text_edit.old_len) {
                    text_edit.found=text_edit.offset-text_edit.old_len;
                    if(++text_edit.matches==2)return edit_finish(out,cap,"AMBIGUOUS_MATCH",0,0);
                    text_edit.matched=text_edit.prefix[text_edit.matched-1];
                }
            } else if(m)text_edit.matched=text_edit.prefix[m-1];
            else text_edit.offset++;
        }
        if(text_edit.offset<text_edit.size)break;
        if(!text_edit.matches)return edit_finish(out,cap,"NO_MATCH",0,0);
        text_edit.length=text_edit.size-text_edit.old_len+text_edit.new_len;
        if(text_edit.length>TOOLS_ACCEPTED_FILE_CAP)return edit_finish(out,cap,"LIMIT",0,0);
        text_edit.changed=text_edit.length!=text_edit.size;
        text_edit.offset=0;text_edit.phase=E_COPY;break;
    case E_COPY:
        n=text_edit.length-text_edit.offset;if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
        for(i=0;i<n;i++) {
            long at=text_edit.offset+i;
            text_edited[at]=at<text_edit.found ? text_source[at] :
                at<text_edit.found+text_edit.new_len ? text_edit.replacement[at-text_edit.found] :
                text_source[at-text_edit.new_len+text_edit.old_len];
        }
        if(!text_edit.changed && memcmp(text_edited+text_edit.offset,text_source+text_edit.offset,(size_t)n))text_edit.changed=1;
        text_edit.edited_hash=hash_more(text_edit.edited_hash,text_edited+text_edit.offset,n);
        text_edit.offset+=n;if(text_edit.offset==text_edit.length) {
            if(!text_edit.changed)return edit_finish(out,cap,"NO_CHANGE",0,0);
            text_edit.phase=E_NAME;
        }
        break;
    case E_NAME: {
        char tmp[32],bak[32];size_t prefix=strrchr(text_edit.path,':') ? (size_t)(strrchr(text_edit.path,':')-text_edit.path+1) : 0;
        static char record[AGENT_RESULT_CAP];
        if(text_edit.attempt==100)return edit_finish(out,cap,"STAGE",0,0);
        snprintf(tmp,sizeof(tmp),"Sherclawk tmp %08lx %02x",(unsigned long)text_edit.started,text_edit.attempt);
        snprintf(bak,sizeof(bak),"Sherclawk bak %08lx %02x",(unsigned long)text_edit.started,text_edit.attempt++);
        text_edit.stage=text_edit.backup=text_edit.target;
        text_edit.stage.name[0]=(unsigned char)strlen(tmp);memcpy(text_edit.stage.name+1,tmp,text_edit.stage.name[0]);
        text_edit.backup.name[0]=(unsigned char)strlen(bak);memcpy(text_edit.backup.name+1,bak,text_edit.backup.name[0]);
        err=catalog(&text_edit.stage,&pb);if(!err)break;
        if(err!=fnfErr)return edit_finish(out,cap,"STAGE",err,0);
        err=catalog(&text_edit.backup,&pb);if(!err)break;
        if(err!=fnfErr)return edit_finish(out,cap,"STAGE",err,0);
        snprintf(text_edit.temporary,sizeof(text_edit.temporary),"%.*s%s",(int)prefix,text_edit.path,tmp);
        snprintf(text_edit.backup_path,sizeof(text_edit.backup_path),"%.*s%s",(int)prefix,text_edit.path,bak);
        if(edit_result(record,sizeof(record)-100,"pending","EXACT_EDIT",text_edit.path,text_edit.temporary,
            text_edit.backup_path,text_edit.length,text_edit.expected,text_edit.previous,0))return edit_finish(out,cap,"LIMIT",0,0);
        strcpy(text_edit.revision,text_edit.expected);
        if(edit_record("mutation_intent","pending"))return edit_finish(out,cap,"JOURNAL",0,1);
        err=FSpCreate(&text_edit.stage,'ttxt','TEXT',smSystemScript);
        if(err==dupFNErr)break;
        text_edit.retained=1;
        if(!err)err=FSpOpenDF(&text_edit.stage,fsWrPerm,&text_edit.aux);
        if(err)return edit_finish(out,cap,"STAGE_FAILED_INSPECT_TEMP",err,0);
        text_edit.offset=0;text_edit.phase=E_WRITE;break;
    }
    case E_WRITE:
        n=text_edit.length-text_edit.offset;if(n>TOOLS_WORK_CHUNK)n=TOOLS_WORK_CHUNK;
        got=n;err=FSWrite(text_edit.aux,&got,text_edited+text_edit.offset);
        if(err || got!=n)return edit_finish(out,cap,"STAGE_FAILED_INSPECT_TEMP",err ? err : ioErr,0);
        text_edit.offset+=n;
        if(text_edit.offset==text_edit.length) {
            c=FSClose(text_edit.aux);text_edit.aux=-1;
            if(!c)c=FlushVol(NULL,text_edit.stage.vRefNum);
            if(c)return edit_finish(out,cap,"STAGE_FAILED_INSPECT_TEMP",c,0);
            text_edit.phase=E_STAGE_OPEN;
        }
        break;
    case E_STAGE_OPEN:
        err=catalog(&text_edit.stage,&text_edit.staged);
        if(!err && (text_edit.staged.hFileInfo.ioFlLgLen!=text_edit.length || !plain_file(&text_edit.stage,&text_edit.staged) ||
            text_edit.staged.hFileInfo.ioFlFndrInfo.fdType!='TEXT' || text_edit.staged.hFileInfo.ioFlFndrInfo.fdCreator!='ttxt'))err=ioErr;
        if(!err)err=FSpOpenDF(&text_edit.stage,fsRdPerm,&text_edit.aux);
        if(err)return edit_finish(out,cap,"STAGE_FAILED_INSPECT_TEMP",err,0);
        text_edit.offset=0;text_edit.phase=E_STAGE_VERIFY;break;
    case E_STAGE_VERIFY:
    case E_PUBLISHED_VERIFY:
        err=edit_compare(text_edit.aux,text_edited,text_edit.length);
        if(err)return edit_finish(out,cap,"STAGE_FAILED_INSPECT_TEMP",err,0);
        if(text_edit.offset<text_edit.length)break;
        c=FSClose(text_edit.aux);text_edit.aux=-1;
        if(c)return edit_finish(out,cap,"CLOSE_INSPECT_PATHS",c,1);
        err=catalog(text_edit.phase==E_STAGE_VERIFY ? &text_edit.stage : &text_edit.target,&pb);
        { CInfoPBRec normalized=pb;normalized.hFileInfo.ioFlMdDat=text_edit.staged.hFileInfo.ioFlMdDat;
          if(!err && !same_file(&text_edit.staged,&normalized))err=ioErr; }
        if(err)return edit_finish(out,cap,"CHANGED_INSPECT_PATHS",err,0);
        revision_hash(&pb,text_edit.length,text_edit.edited_hash,text_edit.revision,sizeof(text_edit.revision));
        text_edit.phase=text_edit.phase==E_STAGE_VERIFY ? E_STAGED : E_COMMIT;break;
    case E_STAGED:
        if(edit_record("mutation_staged","staged"))return edit_finish(out,cap,"JOURNAL_STAGE_RETAINED",0,1);
        text_edit.phase=E_ORIGINAL_OPEN;break;
    case E_ORIGINAL_OPEN:
    case E_BACKUP_OPEN:
        err=catalog(text_edit.phase==E_ORIGINAL_OPEN ? &text_edit.target : &text_edit.backup,&pb);
        if(text_edit.phase==E_BACKUP_OPEN)pb.hFileInfo.ioFlMdDat=text_edit.original.hFileInfo.ioFlMdDat;
        if(!err && !same_file(&text_edit.original,&pb))err=ioErr;
        if(!err)err=SetFPos(text_edit.ref,fsFromStart,0);
        if(err)return edit_finish(out,cap,"CHANGED_STAGE_RETAINED",err,0);
        text_edit.offset=0;text_edit.phase=text_edit.phase==E_ORIGINAL_OPEN ? E_ORIGINAL_VERIFY : E_BACKUP_VERIFY;break;
    case E_ORIGINAL_VERIFY:
    case E_BACKUP_VERIFY:
        err=edit_compare(text_edit.ref,text_source,text_edit.size);
        if(err)return edit_finish(out,cap,"CHANGED_STAGE_RETAINED",err,0);
        if(text_edit.offset<text_edit.size)break;
        err=catalog(text_edit.phase==E_ORIGINAL_VERIFY ? &text_edit.target : &text_edit.backup,&pb);
        if(text_edit.phase==E_BACKUP_VERIFY)pb.hFileInfo.ioFlMdDat=text_edit.original.hFileInfo.ioFlMdDat;
        if(!err && !same_file(&text_edit.original,&pb))err=ioErr;
        if(err)return edit_finish(out,cap,"CHANGED_STAGE_RETAINED",err,0);
        text_edit.phase=text_edit.phase==E_ORIGINAL_VERIFY ? E_BACKUP_RENAME : E_BACKED_UP;break;
    case E_BACKUP_RENAME:
        text_edit.renamed=1;
        err=FSpRename(&text_edit.target,text_edit.backup.name);
        if(!err)err=FlushVol(NULL,text_edit.target.vRefNum);
        if(err)return edit_finish(out,cap,"PUBLISH_INSPECT_PATHS",err,1);
        text_edit.phase=E_BACKUP_OPEN;break;
    case E_BACKED_UP:
        if(edit_record("mutation_backed_up","backed_up"))return edit_finish(out,cap,"JOURNAL_BACKUP_RETAINED",0,1);
        text_edit.phase=E_PUBLISH;break;
    case E_PUBLISH:
        err=FSpRename(&text_edit.stage,text_edit.target.name);
        if(!err)err=FlushVol(NULL,text_edit.target.vRefNum);
        if(err)return edit_finish(out,cap,"PUBLISH_INSPECT_PATHS",err,1);
        text_edit.phase=E_PUBLISHED_OPEN;break;
    case E_PUBLISHED_OPEN:
        err=catalog(&text_edit.target,&pb);
        pb.hFileInfo.ioFlMdDat=text_edit.staged.hFileInfo.ioFlMdDat;
        if(!err && !same_file(&text_edit.staged,&pb))err=ioErr;
        if(!err)err=FSpOpenDF(&text_edit.target,fsRdPerm,&text_edit.aux);
        if(err)return edit_finish(out,cap,"PUBLISH_INSPECT_PATHS",err,1);
        text_edit.offset=0;text_edit.phase=E_PUBLISHED_VERIFY;break;
    case E_COMMIT:
        c=FSClose(text_edit.ref);text_edit.ref=-1;
        if(c)return edit_finish(out,cap,"CLOSE_INSPECT_PATHS",c,1);
        if(edit_record("mutation_committed","ok"))return edit_finish(out,cap,"JOURNAL_AFTER_PUBLISH",0,1);
        edit_result(out,cap,"ok","EDITED",text_edit.path,"",text_edit.backup_path,text_edit.length,text_edit.revision,text_edit.previous,0);
        text_active=0;return 0;
    }
    return 2;
}
/* Create-only folder publication: HFS creation is atomic, so one intent record
 * precedes it and one committed record follows verification. */
enum { FOLDER_CREATED, FOLDER_EXISTS, FOLDER_FAILED, FOLDER_UNCERTAIN, FOLDER_UNVERIFIED };
/* Create one folder and prove it: FSpDirCreate, FlushVol, then a catalog read
 * that must show a real folder. *os_err is the native error of the step that
 * decided the result. FOLDER_EXISTS: the name was taken, unless accept_existing
 * verifies the folder that is there instead. FOLDER_FAILED: provably nothing
 * is at the name. FOLDER_UNCERTAIN: the create errored but something exists.
 * FOLDER_UNVERIFIED: the create succeeded but the folder cannot be proven.
 * *dir is the folder's directory ID only for FOLDER_CREATED. */
static int create_verified_folder(FSSpec *target, int accept_existing, long *dir, OSErr *os_err)
{
    CInfoPBRec pb;
    long made = 0;
    OSErr err = FSpDirCreate(target, smSystemScript, &made);
    *os_err = err;
    if (err == dupFNErr && !accept_existing) return FOLDER_EXISTS;
    if (err && err != dupFNErr) return catalog(target, &pb) ? FOLDER_FAILED : FOLDER_UNCERTAIN;
    err = FlushVol(NULL, target->vRefNum);
    if (!err) err = catalog(target, &pb);
    if (!err && !is_plain_dir(&pb)) err = ioErr;
    *os_err = err;
    if (err) return FOLDER_UNVERIFIED;
    *dir = pb.dirInfo.ioDrDirID;
    return FOLDER_CREATED;
}
static int create_folder(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                         AgentJournal journal, void *context)
{
    static char record[AGENT_RESULT_CAP]; /* static: the application stack is small */
    char path[512], q[1100], call_id[800];
    FSSpec target;
    OSErr err;
    long dir = 0;
    if (valid_keys(call->arguments, tokens, "|path|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected only a path string.", 0); return 0;
    }
    err = tools_resolve(path, &target);
    if (!err) { fail(out, cap, "EXISTS", "Destination exists; create_folder never reuses a name.", 0); return 0; }
    if (err != fnfErr) { fail(out, cap, "PATH", "Use a relative workspace folder path with existing non-alias parent folders and no trailing colon.", err); return 0; }
    if (!journal) { fail(out, cap, "JOURNAL", "Creating a folder requires a durable session journal.", 0); return 1; }
    if (json_quote(call->id, call_id, sizeof(call_id)) < 0 || json_quote(path, q, sizeof(q)) < 0) {
        fail(out, cap, "JOURNAL", "Cannot encode call identity.", 0); return 1;
    }
    snprintf(record, sizeof(record), "{\"status\":\"pending\",\"code\":\"CREATE_FOLDER\",\"path\":%s,\"os_error\":0}", q);
    if (journal_mutation(journal, context, "mutation_intent", call_id, record)) { fail(out, cap, "JOURNAL", "Cannot record mutation intent; no folder created.", 0); return 1; }
    switch (create_verified_folder(&target, 0, &dir, &err)) {
    case FOLDER_EXISTS:
        fail(out, cap, "EXISTS", "Destination appeared during creation; nothing was changed by this call.", err); return 0;
    case FOLDER_FAILED:
        fail(out, cap, "CREATE_FAILED", "Folder creation failed; no folder exists at the destination.", err); return 0;
    case FOLDER_UNCERTAIN:
        /* A failed create may still have left a folder; never claim "unchanged" unless proven. */
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"CREATE_FOLDER_UNCERTAIN\",\"path\":%s,\"os_error\":%d}", q, (int)err); return 1;
    case FOLDER_UNVERIFIED:
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"CREATE_FOLDER_UNVERIFIED\",\"path\":%s,\"os_error\":%d}", q, (int)err); return 1;
    }
    snprintf(out, cap, "{\"status\":\"ok\",\"code\":\"CREATED_FOLDER\",\"path\":%s,\"os_error\":0}", q);
    if (journal_mutation(journal, context, "mutation_committed", call_id, out)) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"JOURNAL_AFTER_PUBLISH\",\"path\":%s,\"os_error\":0}", q); return 1;
    }
    return 0;
}
/* ---------- Reversible delete: a same-volume rename into a Trash folder ----
 * Sources are plain files pinned to a cat- catalog revision from list_files or
 * get_file_info. The destination is the volume's Trash when it resolves on the
 * same volume (on an AFP share, the AppleShare client's Network Trash Folder),
 * else a journaled Sherclawk Trash folder in the workspace. Nothing is ever
 * deleted outright and this tool never empties a Trash. */
#define TRASH_MAX 8
#define TRASH_FOLDER_NAME "Sherclawk Trash"
typedef struct {
    char path[512], revision[48], leaf[64], moved_as[64];
    FSSpec source;
    long identity, data_size, resource_size;
    unsigned long file_type, creator;
} TrashItem;
/* Scratch for one move_to_trash call, static because the application stack is
 * small (docs/limits.md). Calls never overlap: tools run one at a time. */
static struct {
    char call_id[800];
    char record[AGENT_RESULT_CAP];
    char quoted[1100], quoted_alt[1100], path[1100];
} trash_work;
/* Nothing inside a Trash can be trashed again. Both roots sit at the workspace
 * (volume) top level on this deployment. */
static int trash_reserved_path(const char *path)
{
    static const char *const reserved[] = { "network trash folder", "sherclawk trash" };
    size_t k;
    for (k = 0; k < sizeof(reserved) / sizeof(reserved[0]); k++) {
        size_t n = strlen(reserved[k]), i;
        for (i = 0; i < n && path[i]; i++) {
            unsigned char c = (unsigned char)path[i];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 'a' - 'A');
            if (c != (unsigned char)reserved[k][i]) break;
        }
        if (i == n && (path[i] == 0 || path[i] == ':')) return 1;
    }
    return 0;
}
static int revision_matches(const CInfoPBRec *pb, const char *expected)
{
    char actual[48];
    tools_catalog_revision(pb, actual, sizeof(actual));
    return !strcmp(actual, expected);
}
/* An item refusal names the failing path so a batch error is actionable; no
 * journal record is written and nothing moved. */
static int trash_refuse(char *out, size_t cap, const char *code, const char *message,
                        const char *path, int native)
{
    if (json_quote(path, trash_work.quoted, sizeof(trash_work.quoted)) < 0 ||
        json_quote(message, trash_work.quoted_alt, sizeof(trash_work.quoted_alt)) < 0)
        return fail(out, cap, "LIMIT", "Cannot encode the refusal.", native);
    snprintf(out, cap, "{\"status\":\"error\",\"code\":\"%s\",\"moved\":0,\"failed\":%s,\"message\":%s,\"os_error\":%d}",
        code, trash_work.quoted, trash_work.quoted_alt, native);
    return -1;
}
/* Validate one item without changing anything. 1 valid, 0 refused (out set). */
static int trash_validate(TrashItem *item, char *out, size_t cap)
{
    CInfoPBRec pb;
    OSErr err;
    if (evidence_path(item->path) || trash_reserved_path(item->path)) {
        trash_refuse(out, cap, "REFUSED", "Build evidence and Trash contents cannot be trashed.", item->path, 0); return 0;
    }
    err = tools_resolve(item->path, &item->source);
    if (err) { trash_refuse(out, cap, "FILE", "Cannot resolve the workspace path.", item->path, err); return 0; }
    err = catalog(&item->source, &pb);
    if (err) { trash_refuse(out, cap, "FILE", "Cannot inspect the source file.", item->path, err); return 0; }
    if (pb.hFileInfo.ioFlAttrib & 16) {
        trash_refuse(out, cap, "NOT_FILE", "Only files can be trashed; folders are never moved.", item->path, 0); return 0;
    }
    if (pb.hFileInfo.ioFlFndrInfo.fdType == 'alis' || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000)) {
        trash_refuse(out, cap, "ALIAS", "Aliases are refused; trash the file itself, not an alias.", item->path, 0); return 0;
    }
    if (pb.hFileInfo.ioFlAttrib & 1) {
        trash_refuse(out, cap, "LOCKED", "Locked files are refused; unlock the file first.", item->path, 0); return 0;
    }
    if (!revision_matches(&pb, item->revision)) {
        trash_refuse(out, cap, "REVISION_MISMATCH", "The file changed since it was listed; list it again and retry.", item->path, 0); return 0;
    }
    item->identity = pb.hFileInfo.ioDirID;
    item->data_size = pb.hFileInfo.ioFlLgLen;
    item->resource_size = pb.hFileInfo.ioFlRLgLen;
    item->file_type = pb.hFileInfo.ioFlFndrInfo.fdType;
    item->creator = pb.hFileInfo.ioFlFndrInfo.fdCreator;
    if (text_to_utf8((char *)item->source.name + 1, item->source.name[0], item->leaf, sizeof(item->leaf)) < 0) {
        trash_refuse(out, cap, "ENCODING", "Filename conversion failed.", item->path, 0); return 0;
    }
    return 1;
}
/* The volume Trash for the source, accepted only when FindFolder reports it on
 * the same volume; never a cross-volume copy. The classic Folder Manager API
 * returns the folder's volume and directory ID, not a named FSSpec. */
static OSErr trash_destination(const FSSpec *source, short *vref, long *dir)
{
    short found_vol = 0;
    long found_dir = 0;
    OSErr err = FindFolder(source->vRefNum, kTrashFolderType, 0, &found_vol, &found_dir);
    if (err) return err;
    if (found_vol != source->vRefNum || !found_dir) return paramErr;
    *vref = found_vol; *dir = found_dir;
    return 0;
}
/* The workspace fallback Trash, created once with its own journal records.
 * 0 usable, 1 stop the run, -1 error with nothing moved. */
static int trash_fallback(short *vref, long *dir, AgentJournal journal, void *context,
                          char *out, size_t cap)
{
    FSSpec trash;
    CInfoPBRec pb;
    OSErr err;
    long made = 0;
    char q[128];
    int status;
    err = tools_resolve(TRASH_FOLDER_NAME, &trash);
    if (err && err != fnfErr) { fail(out, cap, "TRASH", "Cannot resolve the workspace trash folder.", err); return -1; }
    err = catalog(&trash, &pb);
    if (!err) {
        if (!is_plain_dir(&pb)) {
            fail(out, cap, "TRASH", "The workspace trash name is not a folder.", 0); return -1;
        }
        *vref = trash.vRefNum; *dir = pb.dirInfo.ioDrDirID; return 0;
    }
    if (err != fnfErr) { fail(out, cap, "TRASH", "Cannot inspect the workspace trash folder.", err); return -1; }
    if (json_quote(TRASH_FOLDER_NAME, q, sizeof(q)) < 0) { fail(out, cap, "JOURNAL", "Cannot encode the trash path.", 0); return 1; }
    snprintf(trash_work.record, sizeof(trash_work.record),
        "{\"status\":\"pending\",\"code\":\"CREATE_FOLDER\",\"kind\":\"trash_folder\",\"path\":%s,\"os_error\":0}", q);
    if (journal_mutation(journal, context, "mutation_intent", trash_work.call_id, trash_work.record)) {
        fail(out, cap, "JOURNAL", "Cannot record trash folder intent; nothing moved.", 0); return 1;
    }
    status = create_verified_folder(&trash, 1, &made, &err);
    if (status == FOLDER_FAILED) { fail(out, cap, "TRASH_FAILED", "Trash folder creation failed; nothing moved.", err); return -1; }
    if (status != FOLDER_CREATED) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"%s\",\"kind\":\"trash_folder\",\"path\":%s,\"os_error\":%d}",
            status == FOLDER_UNCERTAIN ? "TRASH_FOLDER_UNCERTAIN" : "TRASH_FOLDER_UNVERIFIED", q, (int)err);
        return 1;
    }
    snprintf(trash_work.record, sizeof(trash_work.record),
        "{\"status\":\"ok\",\"code\":\"CREATED_FOLDER\",\"kind\":\"trash_folder\",\"path\":%s,\"os_error\":0}", q);
    *vref = trash.vRefNum; *dir = made;
    if (journal_mutation(journal, context, "mutation_committed", trash_work.call_id, trash_work.record)) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"JOURNAL_AFTER_PUBLISH\",\"kind\":\"trash_folder\",\"path\":%s,\"os_error\":0}", q);
        return 1;
    }
    return 0;
}
/* FSpCatMove keeps the leaf and never renames, so a Trash that already holds
 * the name is a refusal (dupFNErr) rather than a suffixed move. */
static OSErr trash_leaf_free(short vref, long dir, const unsigned char *leaf)
{
    FSSpec probe;
    OSErr err = FSMakeFSSpec(vref, dir, leaf, &probe);
    if (!err) return dupFNErr;
    return err == fnfErr ? 0 : err;
}
/* Workspace-relative path of the trash folder, or a known fallback leaf when
 * the bounded walk cannot place it inside the workspace. UTF-8 for results. */
static void trash_folder_text(long dir, const char *fallback_leaf, char *out, size_t cap)
{
    FSSpec workspace;
    CInfoPBRec pb;
    char found[768];
    int budget = 512, complete = 1;
    if (!tools_workspace_root(&workspace) && !catalog(&workspace, &pb) &&
        tools_find_dir(&workspace, pb.dirInfo.ioDrDirID, "", dir, found, sizeof(found), 0, &budget, &complete) &&
        text_to_utf8(found, strlen(found), out, cap) >= 0) return;
    if (fallback_leaf && text_to_utf8(fallback_leaf, strlen(fallback_leaf), out, cap) >= 0) return;
    out[0] = 0;
}
/* Format the batch report. -1 when it does not fit, leaving out unspecified. */
static int trash_format(char *out, size_t cap, const char *status, const char *code,
                        const char *folder, const TrashItem *items, int moved,
                        const char *failed, int native)
{
    size_t at = 0;
    char head[80], tail[64];
    int i;
    snprintf(head, sizeof(head), "{\"status\":\"%s\",\"code\":\"%s\",\"moved\":%d,\"trash\":", status, code, moved);
    if (append(out, cap, &at, head) || quote(out, cap, &at, folder)) return -1;
    if (failed) {
        if (append(out, cap, &at, ",\"failed\":") || quote(out, cap, &at, failed)) return -1;
    }
    if (append(out, cap, &at, ",\"files\":[")) return -1;
    for (i = 0; i < moved; i++) {
        if (i && append(out, cap, &at, ",")) return -1;
        if (append(out, cap, &at, "{\"path\":") || quote(out, cap, &at, items[i].path) ||
            append(out, cap, &at, ",\"moved_as\":") || quote(out, cap, &at, items[i].moved_as) ||
            append(out, cap, &at, "}")) return -1;
    }
    snprintf(tail, sizeof(tail), "],\"os_error\":%d}", native);
    return append(out, cap, &at, tail) ? -1 : 0;
}
/* Publish the report. When it cannot be formatted after files already moved,
 * still say how many moved rather than a bare LIMIT. Nonzero: stop the run. */
static int trash_report(char *out, size_t cap, const char *status, const char *code,
                        const char *folder, const TrashItem *items, int moved,
                        const char *failed, int native)
{
    if (!trash_format(out, cap, status, code, folder, items, moved, failed, native)) return 0;
    if (moved > 0) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"REPORT_LIMIT\",\"moved\":%d,\"os_error\":%d}", moved, native);
    } else {
        fail(out, cap, "LIMIT", "Trash report exceeds the result capacity.", 0);
    }
    return 1;
}
/* Refuse before any rename when any report this batch can produce would not
 * fit. The largest report formats every item with its longest possible trashed
 * name (the source name, which a move keeps) under the longest
 * status and code. */
static int trash_report_fits(char *out, size_t cap, const char *folder, TrashItem *items, int count)
{
    int i;
    for (i = 0; i < count; i++)
        snprintf(items[i].moved_as, sizeof(items[i].moved_as), "%s", items[i].leaf);
    if (trash_format(out, cap, "uncertain", "MOVE_UNCERTAIN_INSPECT_PATHS", folder, items, count, NULL, 0)) {
        fail(out, cap, "LIMIT", "Batch report cannot fit the result budget; send fewer files.", 0);
        return 0;
    }
    return 1;
}
/* Why a single move stopped. counted: the file is in the Trash anyway. */
typedef struct {
    const char *status, *code;
    int native, stop, counted;
} TrashOutcome;
static int trash_stopped(TrashOutcome *o, const char *status, const char *code, int native, int stop, int counted)
{
    o->status = status; o->code = code; o->native = native; o->stop = stop; o->counted = counted;
    return 0;
}
/* Move one validated file with one journaled same-volume rename. 1 moved and
 * committed; 0 stopped, described by *o. */
static int trash_commit_one(TrashItem *item, short trash_vref, long trash_dir, const char *folder,
                            AgentJournal journal, void *context, TrashOutcome *o)
{
    FSSpec target, trash_spec;
    CInfoPBRec pb, gone;
    static const unsigned char no_name[1] = { 0 };
    OSErr err;
    int rn;
    err = catalog(&item->source, &pb);
    if (err) return trash_stopped(o, "error", "FILE", err, 0, 0);
    if (!revision_matches(&pb, item->revision) || pb.hFileInfo.ioDirID != item->identity)
        return trash_stopped(o, "error", "REVISION_MISMATCH", 0, 0, 0);
    err = trash_leaf_free(trash_vref, trash_dir, item->source.name);
    if (err) return trash_stopped(o, "error", "EXISTS", err, 0, 0);
    /* FSpCatMove takes the destination directory; an empty name with the
       directory ID makes the Trash folder's own spec. */
    err = FSMakeFSSpec(trash_vref, trash_dir, no_name, &trash_spec);
    if (err) return trash_stopped(o, "error", "TRASH", err, 0, 0);
    target = item->source;
    target.vRefNum = trash_vref; target.parID = trash_dir;
    if (text_to_utf8((char *)item->source.name + 1, item->source.name[0], item->moved_as, sizeof(item->moved_as)) < 0)
        return trash_stopped(o, "error", "ENCODING", 0, 0, 0);
    if (json_quote(item->path, trash_work.quoted, sizeof(trash_work.quoted)) < 0 ||
        snprintf(trash_work.path, sizeof(trash_work.path), "%s%s", folder, item->moved_as) >= (int)sizeof(trash_work.path) ||
        json_quote(trash_work.path, trash_work.quoted_alt, sizeof(trash_work.quoted_alt)) < 0)
        return trash_stopped(o, "error", "LIMIT", 0, 0, 0);
    rn = snprintf(trash_work.record, sizeof(trash_work.record),
        "{\"status\":\"pending\",\"code\":\"MOVE_TO_TRASH\",\"kind\":\"move_to_trash\","
        "\"path\":%s,\"trash_path\":%s,\"revision\":\"%s\",\"os_error\":0}",
        trash_work.quoted, trash_work.quoted_alt, item->revision);
    if (rn < 0 || rn >= (int)sizeof(trash_work.record)) return trash_stopped(o, "error", "LIMIT", 0, 0, 0);
    if (journal_mutation(journal, context, "mutation_intent", trash_work.call_id, trash_work.record))
        return trash_stopped(o, "error", "JOURNAL", 0, 1, 0);
    /* One same-volume rename is the whole mutation. A rename error is only
       certain when the same file is still at the source path; otherwise the
       reply may have been lost, so verify the destination below instead of
       claiming failure. */
    err = FSpCatMove(&item->source, &trash_spec);
    if (err && !catalog(&item->source, &gone) && gone.hFileInfo.ioDirID == item->identity)
        return trash_stopped(o, "error", err == dupFNErr ? "EXISTS" : "MOVE_FAILED", err, 0, 0);
    err = FlushVol(NULL, target.vRefNum);
    if (!err) err = catalog(&target, &pb);
    if (!err && (pb.hFileInfo.ioFlLgLen != item->data_size || pb.hFileInfo.ioFlRLgLen != item->resource_size ||
        pb.hFileInfo.ioFlFndrInfo.fdType != item->file_type || pb.hFileInfo.ioFlFndrInfo.fdCreator != item->creator)) err = ioErr;
    if (!err && !catalog(&item->source, &gone)) err = ioErr;
    if (err) return trash_stopped(o, "uncertain", "MOVE_UNCERTAIN_INSPECT_PATHS", err, 1, 1);
    snprintf(trash_work.record, sizeof(trash_work.record),
        "{\"status\":\"ok\",\"code\":\"TRASHED\",\"kind\":\"move_to_trash\","
        "\"path\":%s,\"trash_path\":%s,\"revision\":\"%s\",\"os_error\":0}",
        trash_work.quoted, trash_work.quoted_alt, item->revision);
    if (journal_mutation(journal, context, "mutation_committed", trash_work.call_id, trash_work.record))
        return trash_stopped(o, "uncertain", "JOURNAL_AFTER_MOVE", 0, 1, 1);
    return 1;
}
/* One {path, revision} object into *item. 1 parsed, 0 malformed. */
static int trash_parse_item(const char *arguments, const JsonToken *tokens, int object, TrashItem *item)
{
    int k, keys = 0, path_token = -1, revision_token = -1;
    memset(item, 0, sizeof(*item));
    for (k = object + 1; k < tokens[object].next; k = tokens[k + 1].next) {
        char key[32];
        if (json_string(arguments, tokens, k, key, sizeof(key)) < 0) return 0;
        keys++;
        if (!strcmp(key, "path") && path_token < 0) path_token = k + 1;
        else if (!strcmp(key, "revision") && revision_token < 0) revision_token = k + 1;
        else return 0;
    }
    return keys == 2 && path_token >= 0 && revision_token >= 0 &&
        json_string(arguments, tokens, path_token, item->path, sizeof(item->path)) >= 0 &&
        json_string(arguments, tokens, revision_token, item->revision, sizeof(item->revision)) >= 0 &&
        !strncmp(item->revision, "cat-", 4);
}
/* The files array into items. Count on success; 0 refused with out set. */
static int trash_parse(const AgentCall *call, const JsonToken *tokens, TrashItem items[TRASH_MAX],
                       char *out, size_t cap)
{
    int count = 0, i, files_token;
    if (valid_keys(call->arguments, tokens, "|files|")) {
        fail(out, cap, "ARGUMENTS", "Expected only a files array of {path, revision} objects.", 0); return 0;
    }
    files_token = json_member(call->arguments, tokens, 0, "files");
    if (files_token < 0 || tokens[files_token].type != JSON_ARRAY || files_token + 1 == tokens[files_token].next) {
        fail(out, cap, "ARGUMENTS", "files must be a non-empty array of {path, revision} objects.", 0); return 0;
    }
    for (i = files_token + 1; i < tokens[files_token].next; i = tokens[i].next) {
        if (tokens[i].type != JSON_OBJECT || count == TRASH_MAX) {
            fail(out, cap, "ARGUMENTS", "At most 8 {path, revision} items are accepted, with no other fields.", 0); return 0;
        }
        if (!trash_parse_item(call->arguments, tokens, i, &items[count])) {
            fail(out, cap, "ARGUMENTS", "Each item must be exactly {path, revision} with a cat- revision.", 0); return 0;
        }
        count++;
    }
    return count;
}
static int move_to_trash(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                         AgentJournal journal, void *context)
{
    static TrashItem items[TRASH_MAX];
    char folder[256];
    short trash_vref = 0;
    long trash_dir = 0;
    int count, i, moved = 0, r, fallback = 0;
    OSErr err;
    TrashOutcome o;
    count = trash_parse(call, tokens, items, out, cap);
    if (!count) return 0;
    if (!journal) { fail(out, cap, "JOURNAL", "Moving files requires a durable session journal.", 0); return 1; }
    if (json_quote(call->id, trash_work.call_id, sizeof(trash_work.call_id)) < 0) {
        fail(out, cap, "JOURNAL", "Cannot encode call identity.", 0); return 1;
    }
    for (i = 0; i < count; i++) {
        if (!trash_validate(&items[i], out, cap)) return 0;
        if (items[i].source.vRefNum != items[0].source.vRefNum) {
            fail(out, cap, "CROSS_VOLUME", "All files in one batch must be on the same volume.", 0); return 0;
        }
    }
    err = trash_destination(&items[0].source, &trash_vref, &trash_dir);
    if (err) {
        r = trash_fallback(&trash_vref, &trash_dir, journal, context, out, cap);
        if (r > 0) return 1;
        if (r < 0) return 0;
        fallback = 1;
    }
    trash_folder_text(trash_dir, fallback ? TRASH_FOLDER_NAME : NULL, folder, sizeof(folder));
    if (!trash_report_fits(out, cap, folder, items, count)) return 0;
    for (i = 0; i < count; i++) {
        if (trash_commit_one(&items[i], trash_vref, trash_dir, folder, journal, context, &o)) { moved++; continue; }
        moved += o.counted;
        return trash_report(out, cap, o.status, o.code, folder, items, moved,
                            o.counted ? NULL : items[i].path, o.native) || o.stop;
    }
    return trash_report(out, cap, "ok", "TRASHED", folder, items, moved, NULL, 0);
}
/* Publish a complete, fixed template by one collision-safe folder rename.
 * Failed stages stay journaled for inspection; never reuse or roll them back. */
static int project_result(char *out, size_t cap, const char *status, const char *code,
                          const char *path, const char *temporary, int native)
{
    char head[120], tail[256];
    size_t at = 0;
    snprintf(head, sizeof(head), "{\"status\":\"%s\",\"code\":\"%s\",\"path\":", status, code);
    snprintf(tail, sizeof(tail), ",\"template\":\"ppc-toolbox-v1\",\"files\":[\"main.c\",\"app.r\",\"project.json\"],"
        "\"build_supported\":true,\"launch_supported\":true,\"os_error\":%d}", native);
    return append(out, cap, &at, head) || quote(out, cap, &at, path) ||
        append(out, cap, &at, ",\"temporary_path\":") || quote(out, cap, &at, temporary) ||
        append(out, cap, &at, tail) ? -1 : 0;
}
static OSErr project_file(FSSpec *file, const char *bytes, int create)
{
    char observed[4096];
    CInfoPBRec pb;
    OSErr err, closed;
    short ref;
    long length = (long)strlen(bytes), count;
    if (length > (long)sizeof(observed)) return paramErr;
    if (create) {
        err = FSpCreate(file, 'ttxt', 'TEXT', smSystemScript);
        if (err) return err;
        err = FSpOpenDF(file, fsWrPerm, &ref);
        if (err) return err;
        count = length; err = FSWrite(ref, &count, bytes); closed = FSClose(ref);
        if (!err && count != length) err = ioErr;
        if (!err) err = closed;
        if (err) return err;
        err = FlushVol(NULL, file->vRefNum); if (err) return err;
    }
    err = catalog(file, &pb);
    if (!err && (!plain_file(file, &pb) || pb.hFileInfo.ioFlLgLen != length ||
        pb.hFileInfo.ioFlFndrInfo.fdType != 'TEXT' || pb.hFileInfo.ioFlFndrInfo.fdCreator != 'ttxt')) err = ioErr;
    if (err) return err;
    err = FSpOpenDF(file, fsRdPerm, &ref); if (err) return err;
    count = length; err = FSRead(ref, &count, observed); closed = FSClose(ref);
    if (!err && (count != length || memcmp(bytes, observed, (size_t)length))) err = ioErr;
    if (!err) err = closed;
    return err;
}
static int create_project(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                          AgentJournal journal, void *context)
{
    static char record[AGENT_RESULT_CAP]; /* static: the application stack is small */
    char path[512], temporary[768], local[256], child[800], tempname[32];
    char call_id[800];
    FSSpec target, stage, file;
    CInfoPBRec pb;
    OSErr err;
    long dir = 0;
    int attempt, i;
    size_t prefix;
    const char *status = "error", *code = "PROJECT_STAGE_RETAINED";
    if (valid_keys(call->arguments, tokens, "|path|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected only a path string for a new ppc-toolbox-v1 project.", 0); return 0;
    }
    err = tools_resolve(path, &target);
    if (!err) { fail(out, cap, "EXISTS", "Project destination already exists; never reused.", 0); return 0; }
    if (err != fnfErr) { fail(out, cap, "PATH", "Expected a relative new folder in an existing non-alias parent, without trailing colon.", err); return 0; }
    prefix = strrchr(path, ':') ? (size_t)(strrchr(path, ':') - path + 1) : 0;
    /* Validate every generated path before touching the journal or volume. */
    snprintf(temporary, sizeof(temporary), "%.*sSherclawk project 00000000 00", (int)prefix, path);
    for (i = 0; i < (int)(sizeof(project_inputs)/sizeof(project_inputs[0])); i++) {
        snprintf(child, sizeof(child), "%s:%s", path, project_inputs[i].name);
        if (text_to_macroman_strict(child, local, sizeof(local)) < 0 || tools_validate_path(local, 0) || strlen(tools_workspace()) + strlen(local) > 255) goto limit;
        snprintf(child, sizeof(child), "%s:%s", temporary, project_inputs[i].name);
        if (text_to_macroman_strict(child, local, sizeof(local)) < 0 || tools_validate_path(local, 0) || strlen(tools_workspace()) + strlen(local) > 255) goto limit;
    }
    if (project_result(record, sizeof(record), "uncertain", "JOURNAL_AFTER_PUBLISH", path, temporary, -32768)) goto limit;
    if (!journal || json_quote(call->id, call_id, sizeof(call_id)) < 0) {
        fail(out, cap, "JOURNAL", "Project creation requires a durable session journal.", 0); return 1;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        extern unsigned long TickCount(void);
        snprintf(tempname, sizeof(tempname), "Sherclawk project %08lx %02x", (unsigned long)TickCount() & 0xffffffffUL, attempt);
        stage = target; stage.name[0] = (unsigned char)strlen(tempname); memcpy(stage.name + 1, tempname, stage.name[0]);
        err = catalog(&stage, &pb);
        if (!err) continue;
        if (err != fnfErr) { fail(out, cap, "STAGE", "Cannot inspect project staging name.", err); return 0; }
        snprintf(temporary, sizeof(temporary), "%.*s%s", (int)prefix, path, tempname);
        project_result(record, sizeof(record), "pending", "CREATE_PROJECT", path, temporary, 0);
        if (journal_mutation(journal, context, "mutation_intent", call_id, record)) { fail(out, cap, "JOURNAL", "No project created: intent recording failed.", 0); return 1; }
        err = FSpDirCreate(&stage, smSystemScript, &dir);
        if (err == dupFNErr) continue;
        break;
    }
    if (attempt == 100) { fail(out, cap, "STAGE", "Project staging names exhausted.", 0); return 0; }
    if (err) goto retained;
    err = FlushVol(NULL, stage.vRefNum);
    if (!err) err = catalog(&stage, &pb);
    if (!err && (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || pb.dirInfo.ioDrDirID != dir)) err = ioErr;
    if (err) goto retained;
    for (i = 0; i < (int)(sizeof(project_inputs)/sizeof(project_inputs[0])); i++) {
        file = stage; file.parID = dir; file.name[0] = (unsigned char)strlen(project_inputs[i].name);
        memcpy(file.name + 1, project_inputs[i].name, file.name[0]);
        err = project_file(&file, project_inputs[i].bytes, 1); if (err) goto retained;
    }
    project_result(record, sizeof(record), "staged", "CREATE_PROJECT", path, temporary, 0);
    if (journal_mutation(journal, context, "mutation_staged", call_id, record)) { err = 0; code = "JOURNAL_STAGE_RETAINED"; goto retained; }
    err = FSpRename(&stage, target.name);
    if (err == dupFNErr) { code = "EXISTS_STAGE_RETAINED"; goto retained; }
    status = "uncertain"; code = "PROJECT_PUBLISH_UNCERTAIN";
    if (err) goto retained;
    err = FlushVol(NULL, target.vRefNum);
    if (!err) err = catalog(&target, &pb);
    if (!err && (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) || pb.dirInfo.ioDrDirID != dir)) err = ioErr;
    if (err) goto retained;
    for (i = 0; i < (int)(sizeof(project_inputs)/sizeof(project_inputs[0])); i++) {
        file = target; file.parID = dir; file.name[0] = (unsigned char)strlen(project_inputs[i].name);
        memcpy(file.name + 1, project_inputs[i].name, file.name[0]);
        err = project_file(&file, project_inputs[i].bytes, 0); if (err) goto retained;
    }
    project_result(out, cap, "ok", "CREATED_PROJECT", path, "", 0);
    if (journal_mutation(journal, context, "mutation_committed", call_id, out)) { code = "JOURNAL_AFTER_PUBLISH"; goto retained; }
    return 0;
retained:
    project_result(out, cap, status, code, path, temporary, err);
    return 1;
limit:
    fail(out, cap, "LIMIT", "Project child or recovery paths exceed bounds; no project created.", 0); return 0;
}

int tools_execute_recorded(const AgentCall *call, char *out, size_t cap, AgentJournal journal, void *context)
{
    JsonToken tokens[128];
    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return 1; }
    if(text_arguments(call,tokens,out,cap,!strcmp(call->name,"write_text") || !strcmp(call->name,"edit_text") ||
        !strcmp(call->name,"create_folder") || !strcmp(call->name,"create_project")))return 0;
    if (!strcmp(call->name, "get_environment")) {
        if (tokens[0].next != 1) fail(out, cap, "ARGUMENTS", "get_environment takes no arguments.", 0);
        else environment(out, cap);
    } else if (!strcmp(call->name, "list_files")) list(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "read_text")) return read_text_begin(call,out,cap,journal,context,(uint32_t)TickCount());
    else if (!strcmp(call->name, "search_text")) search_text(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "write_text")) return write_text(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "edit_text")) return edit_text_begin(call,out,cap,journal,context,(uint32_t)TickCount());
    else if (!strcmp(call->name, "create_project")) return create_project(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "create_folder")) return create_folder(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "move_to_trash")) return move_to_trash(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "get_file_info") || !strcmp(call->name, "resolve_alias") ||
             !strcmp(call->name, "list_processes") || !strcmp(call->name, "list_fonts") ||
             !strcmp(call->name, "measure_text") || !strcmp(call->name, "list_resources") ||
             !strcmp(call->name, "read_resource")) inspect_execute(call, out, cap);
    else fail(out, cap, "UNKNOWN_TOOL", "This tool is not installed.", 0);
    return 0;
}
int tools_execute(const AgentCall *call, char *out, size_t cap)
{
    return tools_execute_recorded(call,out,cap,NULL,NULL);
}
int tools_text_step(char *out,size_t cap,uint32_t now,int stop)
{
    if(text_active==1)return read_text_step(out,cap,now,stop);
    if(text_active==2)return edit_text_step(out,cap,now,stop);
    fail(out,cap,"NO_ACTIVE_TEXT","No active text operation.",0);return 1;
}

const char *tools_text_phase(void)
{
    static const char *const reads[]={"read","read_verify","line_navigation","page"};
    static const char *const edits[]={"original_read","original_rewind","initial_verify","KMP_prefix","KMP_match",
        "splice","intent_stage","stage_write","stage_open","stage_verify","staged_journal","original_recheck_open",
        "original_recheck","backup_rename","backup_open","backup_verify","backed_up_journal","publication_rename",
        "publication_open","publication_verify","committed_journal"};
    if(text_active==1)return reads[text_read.phase];
    if(text_active==2)return edits[text_edit.phase];
    return "idle";
}
