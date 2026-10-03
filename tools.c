/* Native bounded tools. Mutations stage verified TEXT files and journal every
 * publication boundary. All catalog/text work is bounded; aliases, binary
 * files, resource forks and parent traversal are never followed as text. */
#include "tools.h"
#include "json.h"
#include "text.h"
#include "config.h"
#include <Files.h>
#include <Script.h>
#include <Memory.h>
#include <Gestalt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    if (snprintf(full, sizeof(full), "%s%s", SHERCLAWK_WORKSPACE, local) >= (int)sizeof(full)) return paramErr;
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
    if (text_to_utf8(SHERCLAWK_WORKSPACE, strlen(SHERCLAWK_WORKSPACE), root, sizeof(root)) < 0 ||
        json_quote(root, q, sizeof(q)) < 0) { fail(out, cap, "CONFIG", "Invalid workspace encoding.", 0); return; }
    snprintf(out, cap, "{\"status\":\"ok\",\"os\":\"classic Mac OS\",\"system_version_hex\":\"%04lx\","
        "\"architecture\":\"PowerPC\",\"workspace\":%s,\"paths\":\"relative colon-separated\","
        "\"encoding\":\"MacRoman data fork to UTF-8\",\"read_only\":false,\"free_heap_bytes\":%ld,"
        "\"tools\":[\"get_environment\",\"list_files\",\"read_text\",\"write_text\"],"
        "\"write_policy\":\"create_only_existing_parent\",\"write_max_bytes\":4096,"
        "\"build_supported\":false,\"launch_supported\":false}", system, q, (long)FreeMem());
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
    if (pb->hFileInfo.ioFlFndrInfo.fdType == 'alis') return 0;
    if (pb->hFileInfo.ioFlFndrInfo.fdType == 'TEXT') return 1;
    if (spec->name[0] > 31) return 0;
    for (i = 0; i < spec->name[0]; i++) name[i] = (char)tolower(spec->name[i + 1]);
    name[i] = 0; dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".r") ||
        !strcmp(dot, ".txt") || !strcmp(dot, ".md") || !strcmp(dot, ".sh") ||
        !strcmp(dot, ".out") || !strcmp(dot, ".log") || !strcmp(dot, ".conf"));
}
static void read(const char *s, const JsonToken *tokens, char *out, size_t cap)
{
    static char bytes[8193], utf8[2200];
    char path[512], header[300], revision[80], quoted[1100], line_info[100];
    FSSpec spec;
    CInfoPBRec pb;
    short ref;
    OSErr err, closed;
    long size;
    size_t begin = 0, end = 0, at = 0, i;
    int line = 1, start = int_arg(s, tokens, "start_line", 1, 1, 100000), maximum = int_arg(s, tokens, "max_lines", 20, 1, 30);
    int emitted = 0, truncated, base = int_arg(s, tokens, "start_byte", 0, 0, 2147483647);
    unsigned long hash = 2166136261UL;
    if (valid_keys(s, tokens, "|path||start_byte||start_line||max_lines|") || string_arg(s, tokens, "path", path, sizeof(path)) < 0 || start < 0 || maximum < 0 || base < 0 || (base && start != 1)) {
        fail(out, cap, "ARGUMENTS", "Expected path, optional start_line or start_byte, and max_lines.", 0); return;
    }
    err = spec_for(path, 0, &spec); if (!err) err = catalog(&spec, &pb);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace file.", err); return; }
    if (!plain_file(&spec, &pb)) { fail(out, cap, "NOT_TEXT", "Only plain data-fork text is supported; binary files, aliases and resource forks are refused.", 0); return; }
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err) { fail(out, cap, "READ", "Cannot open text file.", err); return; }
    if ((long)base > pb.hFileInfo.ioFlLgLen || SetFPos(ref, fsFromStart, (long)base)) {
        FSClose(ref); fail(out, cap, "RANGE", "Byte cursor is outside the file.", 0); return;
    }
    size = pb.hFileInfo.ioFlLgLen - (long)base; if (size > 8192) size = 8192;
    err = FSRead(ref, &size, bytes); closed = FSClose(ref);
    if ((err && err != eofErr) || closed) { fail(out, cap, "READ", "Text read failed.", err ? err : closed); return; }
    bytes[size] = 0;
    for (i = 0; i < (size_t)size; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (!c || (c < 32 && c != 9 && c != 10 && c != 13)) { fail(out, cap, "NOT_TEXT", "The data fork contains binary control bytes.", 0); return; }
        hash = ((hash ^ c) * 16777619UL) & 0xffffffffUL;
    }
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
    snprintf(revision, sizeof(revision), "%08lx-%08lx-%08lx", (unsigned long)pb.hFileInfo.ioFlMdDat,
        (unsigned long)pb.hFileInfo.ioFlLgLen, hash);
    if (base) strcpy(line_info, "\"start_line\":null,\"next_line\":null");
    else if (end > begin && bytes[end - 1] != 10 && bytes[end - 1] != 13 && truncated)
        snprintf(line_info, sizeof(line_info), "\"start_line\":%d,\"next_line\":null", start);
    else snprintf(line_info, sizeof(line_info), "\"start_line\":%d,\"next_line\":%d", start, start + emitted);
    snprintf(header, sizeof(header), "{\"status\":\"ok\",\"encoding\":\"MacRoman\",\"revision\":\"%s\",%s,\"text\":", revision, line_info);
    if (append(out, cap, &at, header) || append(out, cap, &at, quoted)) { fail(out, cap, "LIMIT", "Text result exceeds output capacity.", 0); return; }
    snprintf(header, sizeof(header), ",\"truncated\":%s,\"start_byte\":%ld,\"next_byte\":%ld,\"line_partial\":%s}",
        truncated ? "true" : "false", base + (long)begin, base + (long)end,
        end > begin && bytes[end - 1] != 10 && bytes[end - 1] != 13 && truncated ? "true" : "false");
    if (append(out, cap, &at, header)) fail(out, cap, "LIMIT", "Text result exceeds output capacity.", 0);

}
/* Resolve each existing ancestor by directory ID, never through an alias.
 * Only fnfErr for the final leaf is a valid create destination. */
static OSErr create_spec(const char *path, FSSpec *spec)
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
    err = create_spec(path, &target);
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
    snprintf(revision, sizeof(revision), "%08lx-%08lx-%08lx", (unsigned long)pb.hFileInfo.ioFlMdDat, (unsigned long)length, hash);
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
    snprintf(revision, sizeof(revision), "%08lx-%08lx-%08lx", (unsigned long)pb.hFileInfo.ioFlMdDat, (unsigned long)length, hash);
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
int tools_execute_recorded(const AgentCall *call, char *out, size_t cap, AgentJournal journal, void *context)
{
    JsonToken tokens[128];
    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return 1; }
    if (json_parse(call->arguments, strlen(call->arguments), tokens, 128) < 1 || tokens[0].type != JSON_OBJECT) {
        fail(out, cap, "ARGUMENTS", "Tool arguments must be a bounded JSON object.", 0); return 0;
    }
    if (!strcmp(call->name, "get_environment")) {
        if (tokens[0].next != 1) fail(out, cap, "ARGUMENTS", "get_environment takes no arguments.", 0);
        else environment(out, cap);
    } else if (!strcmp(call->name, "list_files")) list(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "read_text")) read(call->arguments, tokens, out, cap);
    else if (!strcmp(call->name, "write_text")) return write_text(call, tokens, out, cap, journal, context);
    else fail(out, cap, "UNKNOWN_TOOL", "This tool is not installed.", 0);
    return 0;
}
void tools_execute(const AgentCall *call, char *out, size_t cap)
{
    (void)tools_execute_recorded(call, out, cap, NULL, NULL);
}
