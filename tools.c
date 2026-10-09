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
#include <Script.h>
#include <Memory.h>
#include <Gestalt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    long system = 0;
    Gestalt(gestaltSystemVersion, &system);
    if (text_to_utf8(tools_workspace(), strlen(tools_workspace()), root, sizeof(root)) < 0 ||
        json_quote(root, q, sizeof(q)) < 0) { fail(out, cap, "CONFIG", "Invalid workspace encoding.", 0); return; }
    snprintf(out, cap, "{\"status\":\"ok\",\"os\":\"classic Mac OS\",\"system_version_hex\":\"%04lx\","
        "\"architecture\":\"PowerPC\",\"workspace\":%s,\"paths\":\"relative colon-separated\","
        "\"encoding\":\"MacRoman data fork to UTF-8\",\"read_only\":false,\"free_heap_bytes\":%ld,"
        "\"tools\":[\"get_environment\",\"list_files\",\"read_text\",\"search_text\",\"write_text\",\"edit_text\",\"create_folder\",\"create_project\",\"build_project\",\"read_build_log\",\"run_application\",\"quit_application\",\"get_file_info\",\"resolve_alias\",\"list_processes\",\"list_fonts\",\"measure_text\",\"list_resources\",\"read_resource\"],"
        "\"write_policy\":\"create_only_existing_parent\",\"folder_policy\":\"create_only_existing_parent\",\"write_max_bytes\":4096,"
        "\"edit_policy\":\"unique_exact_whole_revision_CR_backup\",\"edit_max_bytes\":4096,"
        "\"build_supported\":true,\"launch_supported\":true}", system, q, (long)FreeMem());
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
        char kind[100];
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
        snprintf(kind, sizeof(kind), ",\"kind\":\"%s\",\"data_bytes\":%ld}",
            pb.hFileInfo.ioFlAttrib & 16 ? "folder" : "file", pb.hFileInfo.ioFlAttrib & 16 ? 0L : (long)pb.hFileInfo.ioFlLgLen);
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
static void read(const char *s, const JsonToken *tokens, char *out, size_t cap)
{
    static char bytes[8193], utf8[2200];
    char path[512], header[512], revision[80], quoted[1100], line_info[100];
    FSSpec spec;
    CInfoPBRec pb;
    short ref;
    OSErr err, closed;
    long size, wanted;
    size_t begin = 0, end = 0, at = 0, i;
    int line = 1, start = int_arg(s, tokens, "start_line", 1, 1, 100000), maximum = int_arg(s, tokens, "max_lines", 20, 1, 30);
    int emitted = 0, truncated, whole, editable, base = int_arg(s, tokens, "start_byte", 0, 0, 2147483647);
    unsigned long hash = 2166136261UL;
    if (valid_keys(s, tokens, "|path||start_byte||start_line||max_lines|") || string_arg(s, tokens, "path", path, sizeof(path)) < 0 || start < 0 || maximum < 0 || base < 0 || (base && start != 1)) {
        fail(out, cap, "ARGUMENTS", "Expected path, optional start_line or start_byte, and max_lines.", 0); return;
    }
    err = spec_for(path, 0, &spec); if (!err) err = catalog(&spec, &pb);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace file.", err); return; }
    if (!plain_file(&spec, &pb)) { fail(out, cap, "NOT_TEXT", "Only plain data-fork text is supported; binary files, aliases and resource forks are refused.", 0); return; }
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err) { fail(out, cap, "READ", "Cannot open text file.", err); return; }
    whole = pb.hFileInfo.ioFlLgLen <= 4096;
    editable = whole;
    if ((long)base > pb.hFileInfo.ioFlLgLen || SetFPos(ref, fsFromStart, whole ? 0L : (long)base)) {
        FSClose(ref); fail(out, cap, "RANGE", "Byte cursor is outside the file.", 0); return;
    }
    size = pb.hFileInfo.ioFlLgLen - (whole ? 0L : (long)base); if (size > 8192) size = 8192;
    wanted = size;
    err = FSRead(ref, &size, bytes); closed = FSClose(ref);
    if (err || closed || size != wanted) { fail(out, cap, "READ", "Text read failed or was short.", err ? err : closed); return; }
    bytes[size] = 0;
    for (i = 0; i < (size_t)size; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (!c || c == 127 || (c < 32 && c != 9 && c != 10 && c != 13)) { fail(out, cap, "NOT_TEXT", "The data fork contains binary control bytes.", 0); return; }
        if (c == 10) editable = 0;
        hash = ((hash ^ c) * 16777619UL) & 0xffffffffUL;
    }
    if (whole) {
        CInfoPBRec after;
        err = catalog(&spec, &after);
        if (err || !same_file(&pb, &after)) { fail(out, cap, "CHANGED", "File changed during read; read it again.", err); return; }
        full_revision(&pb, bytes, size, revision, sizeof(revision));
        memmove(bytes, bytes + base, (size_t)(size - base)); size -= base; bytes[size] = 0;
    } else snprintf(revision, sizeof(revision), "scan-%08lx-%08lx-%08lx", (unsigned long)pb.hFileInfo.ioFlMdDat,
        (unsigned long)pb.hFileInfo.ioFlLgLen, hash);
    while (begin < (size_t)size && line < start) {
        if (bytes[begin] == 13 || (bytes[begin] == 10 && (!begin || bytes[begin - 1] != 13))) line++;
        begin++;
    }
    if (line != start) { fail(out, cap, "RANGE", "Requested line is beyond the bounded 8192-byte prefix. Read an earlier line.", 0); return; }
    if (begin && begin < (size_t)size && bytes[begin] == 10 && bytes[begin - 1] == 13) begin++;
    end = begin;
    /* Keep results compact enough for guaranteed tool-result history reserve. */
    while (end < (size_t)size && emitted < maximum && end - begin < 300) {
        char c = bytes[end++];
        if (c == 13 || (c == 10 && (end < 2 || bytes[end - 2] != 13))) emitted++;
    }
    while (end > begin) {
        if (text_to_utf8(bytes + begin, end - begin, utf8, sizeof(utf8)) >= 0 &&
            json_quote(utf8, quoted, sizeof(quoted)) >= 0) break;
        end--;
    }
    if (end == begin) strcpy(quoted, "\"\"");
    emitted = 0;
    for (i = begin; i < end; i++) if (bytes[i] == 13 || (bytes[i] == 10 && (!i || bytes[i - 1] != 13))) emitted++;
    if (end > begin && bytes[end - 1] != 13 && bytes[end - 1] != 10) emitted++;
    truncated = (long)end + base < pb.hFileInfo.ioFlLgLen;
    if (base) strcpy(line_info, "\"start_line\":null,\"next_line\":null");
    else if (end > begin && bytes[end - 1] != 10 && bytes[end - 1] != 13 && truncated)
        snprintf(line_info, sizeof(line_info), "\"start_line\":%d,\"next_line\":null", start);
    else snprintf(line_info, sizeof(line_info), "\"start_line\":%d,\"next_line\":%d", start, start + emitted);
    snprintf(header, sizeof(header), "{\"status\":\"ok\",\"encoding\":\"MacRoman\",\"revision\":\"%s\",\"revision_scope\":\"%s\",\"editable\":%s,%s,\"text\":", revision, whole ? "whole_file" : "scan", editable ? "true" : "false", line_info);
    if (append(out, cap, &at, header) || append(out, cap, &at, quoted)) { fail(out, cap, "LIMIT", "Text result exceeds output capacity.", 0); return; }
    snprintf(header, sizeof(header), ",\"truncated\":%s,\"start_byte\":%ld,\"next_byte\":%ld,\"line_partial\":%s}",
        truncated ? "true" : "false", base + (long)begin, base + (long)end,
        end > begin && bytes[end - 1] != 10 && bytes[end - 1] != 13 && truncated ? "true" : "false");
    if (append(out, cap, &at, header)) fail(out, cap, "LIMIT", "Text result exceeds output capacity.", 0);

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
static int write_text(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                      AgentJournal journal, void *context)
{
    static char utf8[AGENT_ARGUMENT_CAP], bytes[4097], observed[4097];
    char path[512], temporary[768], tempname[32], revision[80] = "", record[AGENT_RESULT_CAP];
    char call_id[800], envelope[AGENT_RESULT_CAP + 900];
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
        snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
        if (journal(context, "mutation_intent", envelope)) { fail(out, cap, "JOURNAL", "Cannot record mutation intent; no file created.", 0); return 1; }
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
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (journal(context, "mutation_staged", envelope)) {
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
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, out);
    if (journal(context, "mutation_committed", envelope)) {
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
static int edit_text(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                     AgentJournal journal, void *context)
{
    static char utf8[AGENT_ARGUMENT_CAP], old[4097], replacement[4097], source[4097],
        bytes[4097], observed[4097];
    char path[512], temporary[768], backup_path[768], tempname[32], backupname[32];
    char expected[80], revision[80] = "", previous[80], call_id[800];
    char record[AGENT_RESULT_CAP], envelope[AGENT_RESULT_CAP + 900];
    FSSpec target, stage, backup;
    CInfoPBRec original, staged, pb;
    OSErr err, closed;
    short ref = -1, stage_ref;
    long size, count;
    int old_len, new_len, length, attempt, matches = 0, found = 0, stop = 0;
    size_t prefix, i;
    const char *code = "STAGE_FAILED_INSPECT_TEMP", *status = "error";
    if (valid_keys(call->arguments, tokens, "|path||expected_revision||old_text||new_text|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0 ||
        string_arg(call->arguments, tokens, "expected_revision", expected, sizeof(expected)) < 0 ||
        strncmp(expected, "full-", 5) ||
        string_arg(call->arguments, tokens, "old_text", utf8, sizeof(utf8)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected path, whole-file expected_revision, nonempty old_text and new_text.", 0); return 0;
    }
    old_len = text_to_macroman_strict(utf8, old, sizeof(old));
    if (string_arg(call->arguments, tokens, "new_text", utf8, sizeof(utf8)) < 0) {
        fail(out, cap, "ARGUMENTS", "new_text must be a string.", 0); return 0;
    }
    new_len = text_to_macroman_strict(utf8, replacement, sizeof(replacement));
    if (old_len < 1 || new_len < 0) {
        fail(out, cap, "ENCODING_LIMIT", "old_text must be nonempty; both strings must fit 4096 MacRoman bytes.", 0); return 0;
    }
    for (i = 0; i < (size_t)(old_len + new_len); i++) {
        unsigned char c = (unsigned char)(i < (size_t)old_len ? old[i] : replacement[i - old_len]);
        if ((c < 32 && c != 9 && c != 13) || c == 127) {
            fail(out, cap, "NOT_TEXT", "Binary controls are refused.", 0); return 0;
        }
    }
    err = tools_resolve(path, &target); if (!err) err = catalog(&target, &original);
    if (err) { fail(out, cap, "PATH", "Expected an existing workspace file with non-alias parents.", err); return 0; }
    if (!plain_file(&target, &original)) { fail(out, cap, "NOT_TEXT", "Only plain data-fork text can be edited.", 0); return 0; }
    size = original.hFileInfo.ioFlLgLen;
    if (size < 0 || size > 4096) { fail(out, cap, "LIMIT", "Editable source must be at most 4096 bytes.", 0); return 0; }
    err = FSpOpenDF(&target, fsRdWrPerm, &ref);
    if (err) { fail(out, cap, "BUSY", "Cannot exclusively open source; close other users of the file.", err); return 0; }
    count = size; err = FSRead(ref, &count, source);
    if (!err && count != size) err = ioErr;
    if (!err) err = catalog(&target, &pb);
    if (!err && !same_file(&original, &pb)) err = ioErr;
    if (err) { code = "READ"; goto before_stage; }
    source[size] = 0;
    for (i = 0; i < (size_t)size; i++) {
        unsigned char c = (unsigned char)source[i];
        if ((c < 32 && c != 9 && c != 13) || c == 127) { code = c == 10 ? "LINE_ENDINGS" : "NOT_TEXT"; goto before_stage; }
    }
    full_revision(&original, source, size, previous, sizeof(previous));
    if (strcmp(expected, previous)) { code = "REVISION_MISMATCH"; goto before_stage; }
    /* Count overlapping matches as ambiguous too. */
    for (i = 0; i + (size_t)old_len <= (size_t)size; i++)
        if (!memcmp(source + i, old, (size_t)old_len)) { matches++; found = (int)i; }
    if (matches != 1) { code = matches ? "AMBIGUOUS_MATCH" : "NO_MATCH"; goto before_stage; }
    length = (int)size - old_len + new_len;
    if (length > 4096) { code = "LIMIT"; goto before_stage; }
    memcpy(bytes, source, (size_t)found);
    memcpy(bytes + found, replacement, (size_t)new_len);
    memcpy(bytes + found + new_len, source + found + old_len, (size_t)(size - found - old_len));
    bytes[length] = 0;
    if (length == size && !memcmp(bytes, source, (size_t)size)) { code = "NO_CHANGE"; goto before_stage; }
    if (!journal || json_quote(call->id, call_id, sizeof(call_id)) < 0) { code = "JOURNAL"; stop = 1; goto before_stage; }
    prefix = strrchr(path, ':') ? (size_t)(strrchr(path, ':') - path + 1) : 0;
    for (attempt = 0; attempt < 100; attempt++) {
        extern unsigned long TickCount(void);
        unsigned long tick = (unsigned long)TickCount() & 0xffffffffUL;
        snprintf(tempname, sizeof(tempname), "Sherclawk tmp %08lx %02x", tick, attempt);
        snprintf(backupname, sizeof(backupname), "Sherclawk bak %08lx %02x", tick, attempt);
        stage = backup = target;
        stage.name[0] = (unsigned char)strlen(tempname); memcpy(stage.name + 1, tempname, stage.name[0]);
        backup.name[0] = (unsigned char)strlen(backupname); memcpy(backup.name + 1, backupname, backup.name[0]);
        err = catalog(&stage, &pb); if (!err) continue;
        if (err != fnfErr) { code = "STAGE"; goto before_stage; }
        err = catalog(&backup, &pb); if (!err) continue;
        if (err != fnfErr) { code = "STAGE"; goto before_stage; }
        snprintf(temporary, sizeof(temporary), "%.*s%s", (int)prefix, path, tempname);
        snprintf(backup_path, sizeof(backup_path), "%.*s%s", (int)prefix, path, backupname);
        /* Reserve room for the longer error codes before creating a stage. */
        if (edit_result(record, sizeof(record) - 100, "pending", "EXACT_EDIT", path, temporary,
                        backup_path, length, expected, previous, 0)) { code = "LIMIT"; goto before_stage; }
        snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
        if (journal(context, "mutation_intent", envelope)) { code = "JOURNAL"; stop = 1; goto before_stage; }
        err = FSpCreate(&stage, 'ttxt', 'TEXT', smSystemScript);
        if (err == dupFNErr) continue;
        break;
    }
    if (attempt == 100) { code = "STAGE"; goto before_stage; }
    if (err) goto staged_error;
    err = FSpOpenDF(&stage, fsWrPerm, &stage_ref); if (err) goto staged_error;
    count = length; err = FSWrite(stage_ref, &count, bytes); closed = FSClose(stage_ref);
    if (!err && count != length) err = ioErr;
    if (!err) err = closed;
    if (err) goto staged_error;
    err = FlushVol(NULL, stage.vRefNum); if (err) goto staged_error;
    err = catalog(&stage, &pb);
    if (!err && (pb.hFileInfo.ioFlLgLen != length || !plain_file(&stage, &pb) || pb.hFileInfo.ioFlFndrInfo.fdType != 'TEXT')) err = ioErr;
    if (err) goto staged_error;
    err = FSpOpenDF(&stage, fsRdPerm, &stage_ref); if (err) goto staged_error;
    count = length; err = FSRead(stage_ref, &count, observed); closed = FSClose(stage_ref);
    if (!err && (count != length || memcmp(bytes, observed, (size_t)length))) err = ioErr;
    if (!err) err = closed;
    if (err) goto staged_error;
    full_revision(&pb, bytes, length, revision, sizeof(revision));
    staged = pb;
    edit_result(record, sizeof(record), "staged", "EXACT_EDIT", path, temporary, backup_path, length, revision, previous, 0);
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (journal(context, "mutation_staged", envelope)) { code = "JOURNAL_STAGE_RETAINED"; stop = 1; goto staged_error; }
    err = catalog(&target, &pb);
    if (!err && !same_file(&original, &pb)) err = ioErr;
    if (!err) err = SetFPos(ref, fsFromStart, 0);
    count = size; if (!err) err = FSRead(ref, &count, observed);
    if (!err && (count != size || memcmp(source, observed, (size_t)size))) err = ioErr;
    if (err) { code = "CHANGED_STAGE_RETAINED"; goto staged_error; }
    /* From here every failure is uncertain: no automatic rollback/retry. */
    stop = 1; status = "uncertain"; code = "PUBLISH_INSPECT_PATHS";
    err = FSpRename(&target, backup.name); if (err) goto staged_error;
    err = FlushVol(NULL, target.vRefNum); if (err) goto staged_error;
    err = catalog(&backup, &pb);
    /* Some filesystems update modification time on rename; identity, content,
     * size and original Finder metadata must still match. */
    pb.hFileInfo.ioFlMdDat = original.hFileInfo.ioFlMdDat;
    if (!err && !same_file(&original, &pb)) err = ioErr;
    if (!err) err = SetFPos(ref, fsFromStart, 0);
    count = size; if (!err) err = FSRead(ref, &count, observed);
    if (!err && (count != size || memcmp(source, observed, (size_t)size))) err = ioErr;
    if (err) goto staged_error;
    edit_result(record, sizeof(record), "backed_up", "EXACT_EDIT", path, temporary, backup_path, length, revision, previous, 0);
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (journal(context, "mutation_backed_up", envelope)) { code = "JOURNAL_BACKUP_RETAINED"; goto staged_error; }
    err = FSpRename(&stage, target.name); if (err) goto staged_error;
    err = FlushVol(NULL, target.vRefNum); if (err) goto staged_error;
    err = catalog(&target, &pb);
    if (!err) {
        CInfoPBRec renamed = pb;
        renamed.hFileInfo.ioFlMdDat = staged.hFileInfo.ioFlMdDat;
        if (!same_file(&staged, &renamed) || !plain_file(&target, &pb)) err = ioErr;
    }
    if (err) goto staged_error;
    /* Verify the published bytes too; a successful rename alone is not proof. */
    err = FSpOpenDF(&target, fsRdPerm, &stage_ref); if (err) goto staged_error;
    count = length; err = FSRead(stage_ref, &count, observed); closed = FSClose(stage_ref);
    if (!err && (count != length || memcmp(bytes, observed, (size_t)length))) err = ioErr;
    if (!err) err = closed;
    if (err) goto staged_error;
    full_revision(&pb, bytes, length, revision, sizeof(revision));
    err = FSClose(ref); ref = -1; if (err) goto staged_error;
    edit_result(out, cap, "ok", "EDITED", path, "", backup_path, length, revision, previous, 0);
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, out);
    if (journal(context, "mutation_committed", envelope)) {
        edit_result(out, cap, "uncertain", "JOURNAL_AFTER_PUBLISH", path, temporary, backup_path, length, revision, previous, 0); return 1;
    }
    return 0;
before_stage:
    closed = FSClose(ref);
    if (closed) { stop = 1; err = closed; code = "CLOSE"; }
    fail(out, cap, code, "Edit refused; source unchanged. Requires one exact match, current whole-file revision, CR text and durable journal.", err);
    return stop;
staged_error:
    if (ref >= 0) { closed = FSClose(ref); if (closed) { stop = 1; if (!err) err = closed; } }
    edit_result(out, cap, status, code, path, temporary, backup_path, length, revision, previous, err);
    return stop;
}
/* Create-only folder publication: HFS creation is atomic, so one intent record
 * precedes it and one committed record follows verification. */
static int create_folder(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                         AgentJournal journal, void *context)
{
    char path[512], q[1100], record[AGENT_RESULT_CAP], call_id[800], envelope[AGENT_RESULT_CAP + 900];
    FSSpec target;
    CInfoPBRec pb;
    OSErr err;
    long created = 0;
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
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (journal(context, "mutation_intent", envelope)) { fail(out, cap, "JOURNAL", "Cannot record mutation intent; no folder created.", 0); return 1; }
    err = FSpDirCreate(&target, smSystemScript, &created);
    if (err == dupFNErr) { fail(out, cap, "EXISTS", "Destination appeared during creation; nothing was changed by this call.", err); return 0; }
    if (err) {
        /* A failed create may still have left a folder; never claim "unchanged" unless proven. */
        if (!catalog(&target, &pb)) { snprintf(record, sizeof(record), "{\"status\":\"uncertain\",\"code\":\"CREATE_FOLDER_UNCERTAIN\",\"path\":%s,\"os_error\":%d}", q, (int)err); strcpy(out, record); return 1; }
        fail(out, cap, "CREATE_FAILED", "Folder creation failed; no folder exists at the destination.", err); return 0;
    }
    err = FlushVol(NULL, target.vRefNum);
    if (!err) err = catalog(&target, &pb);
    if (!err && (!(pb.hFileInfo.ioFlAttrib & 16) || (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000))) err = ioErr;
    if (err) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"CREATE_FOLDER_UNVERIFIED\",\"path\":%s,\"os_error\":%d}", q, (int)err); return 1;
    }
    snprintf(out, cap, "{\"status\":\"ok\",\"code\":\"CREATED_FOLDER\",\"path\":%s,\"os_error\":0}", q);
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, out);
    if (journal(context, "mutation_committed", envelope)) {
        snprintf(out, cap, "{\"status\":\"uncertain\",\"code\":\"JOURNAL_AFTER_PUBLISH\",\"path\":%s,\"os_error\":0}", q); return 1;
    }
    return 0;
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
    char path[512], temporary[768], local[256], child[800], tempname[32];
    char record[AGENT_RESULT_CAP], call_id[800], envelope[AGENT_RESULT_CAP + 900];
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
        snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
        if (journal(context, "mutation_intent", envelope)) { fail(out, cap, "JOURNAL", "No project created: intent recording failed.", 0); return 1; }
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
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, record);
    if (journal(context, "mutation_staged", envelope)) { err = 0; code = "JOURNAL_STAGE_RETAINED"; goto retained; }
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
    snprintf(envelope, sizeof(envelope), "{\"call_id\":%s,\"mutation\":%s}", call_id, out);
    if (journal(context, "mutation_committed", envelope)) { code = "JOURNAL_AFTER_PUBLISH"; goto retained; }
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
    if (json_parse(call->arguments, strlen(call->arguments), tokens, 128) < 1 || tokens[0].type != JSON_OBJECT) {
        fail(out, cap, "ARGUMENTS", "Tool arguments must be a bounded JSON object.", 0); return 0;
    }
    /* The worker queue is retained execution evidence, never model-editable
     * source. HFS names are case-insensitive; do not allow text tools to forge
     * worker results, rewrite immutable snapshots, or alter launch authority. */
    if (!strcmp(call->name,"write_text") || !strcmp(call->name,"edit_text") ||
        !strcmp(call->name,"create_folder") || !strcmp(call->name,"create_project")) {
        char path[512]; size_t i;
        if(string_arg(call->arguments,tokens,"path",path,sizeof(path))>=0) {
            for(i=0;path[i];i++)if(path[i]>='A' && path[i]<='Z')path[i]=(char)(path[i]+'a'-'A');
            if(!strncmp(path,"worker01:buildjobs",18) && (!path[18] || path[18]==':')) {
                fail(out,cap,"EXECUTION_EVIDENCE_READ_ONLY","Worker queue and snapshots are read-only to source tools.",0);return 0;
            }
        }
    }
    if (!strcmp(call->name, "get_environment")) {
        if (tokens[0].next != 1) fail(out, cap, "ARGUMENTS", "get_environment takes no arguments.", 0);
        else environment(out, cap);
    } else if (!strcmp(call->name, "list_files")) list(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "read_text")) read(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "search_text")) search_text(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "write_text")) return write_text(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "edit_text")) return edit_text(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "create_project")) return create_project(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "create_folder")) return create_folder(call, tokens, out, cap, journal, context);
    else if (!strcmp(call->name, "get_file_info") || !strcmp(call->name, "resolve_alias") ||
             !strcmp(call->name, "list_processes") || !strcmp(call->name, "list_fonts") ||
             !strcmp(call->name, "measure_text") || !strcmp(call->name, "list_resources") ||
             !strcmp(call->name, "read_resource")) inspect_execute(call, out, cap);
    else fail(out, cap, "UNKNOWN_TOOL", "This tool is not installed.", 0);
    return 0;
}
void tools_execute(const AgentCall *call, char *out, size_t cap)
{
    (void)tools_execute_recorded(call, out, cap, NULL, NULL);
}
