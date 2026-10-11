/* Retained MCP results; see mcp_result.h. One retention runs at a time in
 * bounded steps: parse, decode, redact-check, stage, flush, read back,
 * publish, journal, advertise. An artifact is never advertised before its
 * read-back verification and its journal record exist. */
#include "mcp_result.h"
#include "json.h"
#include <Files.h>
#include <Script.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
extern unsigned long TickCount(void);

#define MCP_RESULT_TOKENS 2048
#define MCP_RESULT_CHUNK 8192
#define MCP_RESULT_ID_LEN 17
/* A Pascal string holds 255 bytes; longer paths would silently truncate and
 * resolve to some other file or folder, so they are refused instead. */
#define MCP_RESULT_PATH_MAX 255

typedef struct {
    char id[MCP_RESULT_ID_LEN + 1];
    char path[344];      /* full colon path of the published artifact */
    long bytes;
    int is_error;
    char error_code[16]; /* JSON-RPC error code slice, empty when none */
} McpResultEntry;
static McpResultEntry results[MCP_RESULT_MAX];
static int result_count;
static unsigned long result_scope, result_sequence;

enum {
    R_PARSE, R_ITEM, R_DECODE, R_DECODE_STEP, R_SCAN, R_FINISH,
    R_STAGE_CREATE, R_STAGE_OPEN, R_STAGE_WRITE, R_STAGE_CLOSE, R_FLUSH,
    R_VERIFY_OPEN, R_VERIFY_READ, R_VERIFY_CLOSE, R_PUBLISH, R_JOURNAL, R_SUMMARY
};
static struct {
    McpResultRetain in;
    unsigned long scope;
    char folder[344], id[MCP_RESULT_ID_LEN + 1];
    char stage_path[344], final_path[344];
    FSSpec stage_spec;
    int phase, item, text, list_end, items, unsupported, is_error, first_item;
    int stage_created, renamed, ref_open;
    short ref;
    long written, checked;
    size_t used, item_at, item_len;
    char error_code[16];
    char links[MCP_RESULT_LINK_MAX][MCP_RESULT_LINK_LEN + 1];
    int link_count;
    JsonParser parser;
    JsonDecode decode;
    McpRedact redact;
} work;
static int active;
static JsonToken tokens[MCP_RESULT_TOKENS];
static char stage[MCP_RESULT_CAP + 1];
static char verify[MCP_RESULT_CHUNK];

static void pstr(Str255 p, const char *s)
{
    size_t n = strlen(s);
    if (n > MCP_RESULT_PATH_MAX) n = MCP_RESULT_PATH_MAX; /* callers reject longer paths first */
    p[0] = (unsigned char)n;
    memcpy(&p[1], s, n);
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
    memcpy(out + *at, s, n + 1);
    *at += n;
    return 0;
}
static int quote_at(char *out, size_t cap, size_t *at, const char *s)
{
    int n = json_quote(s, out + *at, cap - *at);
    if (n < 0) return -1;
    *at += (size_t)n;
    return 0;
}

/* End the retention with an error: an unrenamed stage is closed and deleted
 * (a busy file cannot be deleted), a renamed artifact stays on disk as
 * unadvertised evidence, and the ID is never published. Returns 1 (the
 * caller's terminal failure code). */
static int retain_fail(char *out, size_t cap, const char *code, const char *message, int native)
{
    FSSpec spec;
    Str255 path;
    if (work.ref_open) {
        (void)FSClose(work.ref);
        work.ref_open = 0;
    }
    if (work.stage_created && !work.renamed) {
        pstr(path, work.stage_path);
        if (!FSMakeFSSpec(0, 0, path, &spec)) (void)FSpDelete(&spec);
        work.stage_created = 0;
    }
    active = 0;
    fail(out, cap, code, message, native);
    return 1;
}

void mcp_result_new_session(void)
{
    result_scope++;
    result_count = 0;
}

/* Source links are surfaced, bounded and best-effort: http(s) URLs cut from
 * the redacted decoded text, whole and short enough to quote. */
static void scan_links(const char *text, size_t len)
{
    size_t i = 0;
    while (work.link_count < MCP_RESULT_LINK_MAX && i + 7 < len) {
        int plain = text[i] == 'h' && text[i+1] == 't' && text[i+2] == 't' && text[i+3] == 'p' &&
            text[i+4] == ':' && text[i+5] == '/' && text[i+6] == '/';
        int secure = text[i] == 'h' && text[i+1] == 't' && text[i+2] == 't' && text[i+3] == 'p' &&
            text[i+4] == 's' && text[i+5] == ':' && text[i+6] == '/' && text[i+7] == '/';
        if (plain || secure) {
            size_t k = i;
            while (k < len) {
                unsigned char c = (unsigned char)text[k];
                if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' ||
                    c == '\\' || c == '`' || c == '{' || c == '|' || c == '}') break;
                k++;
            }
            if (k - i > 8 && k - i <= MCP_RESULT_LINK_LEN) {
                memcpy(work.links[work.link_count], text + i, k - i);
                work.links[work.link_count][k - i] = 0;
                work.link_count++;
            }
            i = k;
        } else i++;
    }
}

static int item_type_is_text(const char *s, int item)
{
    char type[24];
    int at = json_member(s, tokens, item, "type");
    return at >= 0 && tokens[at].type == JSON_STRING &&
        json_string(s, tokens, at, type, sizeof(type)) > 0 && !strcmp(type, "text");
}

/* Classify the finished parse and set up the item walk. */
static int classify(char *out, size_t cap)
{
    const char *s = work.in.response;
    int result = json_member(s, tokens, 0, "result");
    int error = json_member(s, tokens, 0, "error");
    if ((result >= 0) == (error >= 0))
        return retain_fail(out, cap, "RETENTION",
            "The response carries neither or both of result and error; nothing was retained.", 0);
    if (error >= 0) {
        int code, message;
        work.is_error = 1;
        if (tokens[error].type != JSON_OBJECT)
            return retain_fail(out, cap, "RETENTION", "The error response is malformed; nothing was retained.", 0);
        code = json_member(s, tokens, error, "code");
        if (code >= 0 && tokens[code].type == JSON_PRIMITIVE &&
            tokens[code].end > tokens[code].start &&
            tokens[code].end - tokens[code].start < (int)sizeof(work.error_code)) {
            int i, ok = 1;
            for (i = tokens[code].start; i < tokens[code].end; i++)
                if (s[i] != '-' && !isdigit((unsigned char)s[i])) { ok = 0; break; }
            if (ok) {
                memcpy(work.error_code, s + tokens[code].start, (size_t)(tokens[code].end - tokens[code].start));
                work.error_code[tokens[code].end - tokens[code].start] = 0;
            }
        }
        message = json_member(s, tokens, error, "message");
        if (message >= 0 && tokens[message].type == JSON_STRING) {
            work.text = message;
            work.phase = R_DECODE;
        } else work.phase = R_FINISH;
        return 2;
    }
    if (tokens[result].type != JSON_OBJECT)
        return retain_fail(out, cap, "RETENTION", "The tool result is malformed; nothing was retained.", 0);
    {
        int hint = json_member(s, tokens, result, "isError");
        if (hint >= 0 && tokens[hint].type == JSON_PRIMITIVE &&
            tokens[hint].end - tokens[hint].start == 4 && !strncmp(s + tokens[hint].start, "true", 4))
            work.is_error = 1;
    }
    {
        int list = json_member(s, tokens, result, "content");
        if (list < 0) { work.phase = R_FINISH; return 2; }
        if (tokens[list].type != JSON_ARRAY)
            return retain_fail(out, cap, "RETENTION", "The tool result content is malformed; nothing was retained.", 0);
        work.item = list + 1;
        work.list_end = tokens[list].next;
        work.phase = R_ITEM;
    }
    return 2;
}

/* The retention summary: what the completed call hands the model. */
static int summary_out(char *out, size_t cap, int retained)
{
    char tail[96];
    size_t at = 0;
    int i;
    if (append(out, cap, &at, "{\"status\":\"ok\",\"content_type\":\"text\"") ||
        (retained && (append(out, cap, &at, ",\"result_id\":") ||
                      quote_at(out, cap, &at, work.id)))) return -1;
    snprintf(tail, sizeof(tail), ",\"bytes\":%lu,\"is_error\":%s", (unsigned long)work.used,
             work.is_error ? "true" : "false");
    if (append(out, cap, &at, tail)) return -1;
    if (work.error_code[0] &&
        (append(out, cap, &at, ",\"error_code\":") || quote_at(out, cap, &at, work.error_code))) return -1;
    snprintf(tail, sizeof(tail), ",\"items\":%d,\"unsupported\":%d", work.items, work.unsupported);
    if (append(out, cap, &at, tail) || append(out, cap, &at, ",\"links\":[")) return -1;
    for (i = 0; i < work.link_count; i++) {
        if ((i && append(out, cap, &at, ",")) || quote_at(out, cap, &at, work.links[i])) return -1;
    }
    return append(out, cap, &at, "]}") ? -1 : 0;
}

static int journal_event(void)
{
    char body[512], quoted[MCP_RESULT_ID_LEN * 6 + 8];
    if (!work.in.journal) return 0;
    if (json_quote(work.id, quoted, sizeof(quoted)) < 0) return -1;
    snprintf(body, sizeof(body),
        "{\"result_id\":%s,\"bytes\":%lu,\"is_error\":%s,\"items\":%d,\"unsupported\":%d}",
        quoted, (unsigned long)work.used, work.is_error ? "true" : "false",
        work.items, work.unsupported);
    return work.in.journal(work.in.journal_context, "mcp_result_retained", body) ? -1 : 0;
}

int mcp_result_retain_begin(const McpResultRetain *in, char *out, size_t cap)
{
    char id[MCP_RESULT_ID_LEN + 1], final_path[344], stage_path[344];
    FSSpec spec;
    CInfoPBRec pb;
    Str255 path;
    unsigned long now;
    size_t n;
    int attempt, made;

    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return 1; }
    cap = AGENT_RESULT_CAP;
    if (active) { fail(out, cap, "RETAIN_BUSY", "A result retention is already active.", 0); return 1; }
    if (!in || !in->response || !in->response_len || in->response_len > MCP_MESSAGE_CAP ||
        !in->folder || !*in->folder) {
        fail(out, cap, "ARGUMENTS", "Retention needs a completed response and an artifact folder.", 0);
        return 0;
    }
    n = strlen(in->folder);
    if (n + MCP_RESULT_ID_LEN + 4 > MCP_RESULT_PATH_MAX || in->folder[n - 1] != ':') {
        fail(out, cap, "ARGUMENTS", "The artifact folder path is not usable.", 0);
        return 0;
    }
    if (result_count >= MCP_RESULT_MAX) {
        fail(out, cap, "LIMIT", "The session's retained-result table is full; start a new chat to retain more.", 0);
        return 0;
    }
    {
        /* Resolve the folder without its trailing colon: a missing folder is
         * then a plain fnfErr with a filled spec, the create pattern. */
        char dir_path[344];
        Str255 dir_name;
        snprintf(dir_path, sizeof(dir_path), "%.*s", (int)n - 1, in->folder);
        pstr(dir_name, dir_path);
        if (FSMakeFSSpec(0, 0, dir_name, &spec) == fnfErr) {
            long dir;
            OSErr err = FSpDirCreate(&spec, smSystemScript, &dir);
            if (err) { fail(out, cap, "FOLDER", "The retained-results folder could not be created.", err); return 0; }
        } else {
            memset(&pb, 0, sizeof(pb));
            pb.hFileInfo.ioNamePtr = spec.name;
            pb.hFileInfo.ioVRefNum = spec.vRefNum;
            pb.hFileInfo.ioDirID = spec.parID;
            if (PBGetCatInfoSync(&pb) || !(pb.hFileInfo.ioFlAttrib & 16) ||
                (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000)) {
                fail(out, cap, "FOLDER", "The retained-results folder is not a plain folder.", 0);
                return 0;
            }
        }
    }
    now = TickCount();
    made = 0;
    for (attempt = 0; attempt < 100 && !made; attempt++) {
        if (snprintf(id, sizeof(id), "mcp-%08lx-%04lx", (now + (unsigned long)attempt) & 0xffffffffUL,
                     (++result_sequence) & 0xffffUL) != MCP_RESULT_ID_LEN) {
            fail(out, cap, "STAGE", "The retained-result ID does not fit.", 0);
            return 0;
        }
        snprintf(final_path, sizeof(final_path), "%s%s", in->folder, id);
        snprintf(stage_path, sizeof(stage_path), "%s.new", final_path);
        pstr(path, stage_path);
        if (FSMakeFSSpec(0, 0, path, &spec) != fnfErr) continue; /* stage taken */
        pstr(path, final_path);
        if (FSMakeFSSpec(0, 0, path, &spec) != fnfErr) continue; /* id taken */
        made = 1;
    }
    if (!made) { fail(out, cap, "STAGE", "No unused retained-result ID was found.", 0); return 0; }
    memset(&work, 0, sizeof(work));
    work.in = *in;
    work.scope = result_scope;
    work.phase = R_PARSE;
    work.first_item = 1;
    strcpy(work.id, id);
    strcpy(work.folder, in->folder);
    strcpy(work.stage_path, stage_path);
    strcpy(work.final_path, final_path);
    pstr(path, stage_path);
    if (FSMakeFSSpec(0, 0, path, &work.stage_spec) != fnfErr) {
        active = 0;
        fail(out, cap, "STAGE", "The retained-result stage could not be located.", 0);
        return 0;
    }
    json_parser_init(&work.parser, in->response, in->response_len, tokens, MCP_RESULT_TOKENS);
    active = 1;
    return 2;
}

int mcp_result_retain_step(char *out, size_t cap, int stop)
{
    const char *s = work.in.response;
    OSErr err;
    long n;
    int r, seen;

    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return 1; }
    cap = AGENT_RESULT_CAP;
    if (!active) { fail(out, cap, "RETAIN_BUSY", "No result retention is active.", 0); return 1; }
    if (work.scope != result_scope)
        return retain_fail(out, cap, "SESSION",
            "The session changed while the result was being retained; nothing was retained.", 0);
    if (stop)
        return retain_fail(out, cap, "STOPPED",
            "The result was stopped before verification; nothing was retained and the call is not replayed.", 0);
    for (;;) {
        switch (work.phase) {
        case R_PARSE:
            r = json_parser_step(&work.parser, MCP_WORK_BUDGET);
            if (r < 0)
                return retain_fail(out, cap, "RETENTION", "The response could not be processed; nothing was retained.", 0);
            if (!r) return 2;
            if (tokens[0].type != JSON_OBJECT)
                return retain_fail(out, cap, "RETENTION", "The response is not a JSON-RPC object; nothing was retained.", 0);
            return classify(out, cap);
        case R_ITEM:
            seen = 0;
            while (work.item < work.list_end && seen < 64) {
                if (tokens[work.item].type != JSON_OBJECT || !item_type_is_text(s, work.item)) {
                    work.unsupported++;
                    work.item = tokens[work.item].next;
                    seen++;
                    continue;
                }
                work.text = json_member(s, tokens, work.item, "text");
                if (work.text < 0 || tokens[work.text].type != JSON_STRING) {
                    work.unsupported++;
                    work.item = tokens[work.item].next;
                    seen++;
                    continue;
                }
                work.phase = R_DECODE;
                break;
            }
            if (work.phase == R_ITEM) {
                if (work.item >= work.list_end) { work.phase = R_FINISH; break; }
                return 2; /* the per-step item budget ran out mid-list */
            }
            break;
        case R_DECODE:
            if (!work.first_item) {
                if (work.used >= MCP_RESULT_CAP)
                    return retain_fail(out, cap, "RETENTION", "The retained result exceeds its cap.", 0);
                stage[work.used++] = '\n';
            }
            work.first_item = 0;
            work.item_at = work.used;
            json_decode_init(&work.decode, s, tokens, work.text, stage + work.used,
                             (MCP_RESULT_CAP + 1) - work.used);
            work.phase = R_DECODE_STEP;
            break;
        case R_DECODE_STEP:
            r = json_decode_step(&work.decode, MCP_WORK_BUDGET);
            if (r < 0)
                return retain_fail(out, cap, "RETENTION", "The result text could not be decoded; nothing was retained.", 0);
            if (!r) return 2;
            work.item_len = work.decode.used;
            work.used += work.item_len;
            if (work.used > MCP_RESULT_CAP)
                return retain_fail(out, cap, "RETENTION", "The retained result exceeds its cap.", 0);
            if (work.list_end) work.items++; /* content items only; an error message is not one */
            if (work.in.config)
                mcp_redact_init_text(&work.redact, work.in.config,
                                     work.in.session ? work.in.session : "",
                                     stage + work.item_at, work.item_len, 0);
            work.phase = R_SCAN;
            break;
        case R_SCAN:
            if (work.in.config) {
                r = mcp_redact_step(&work.redact, MCP_WORK_BUDGET);
                if (!r) return 2;
            }
            scan_links(stage + work.item_at, work.item_len);
            if (work.list_end) work.item = tokens[work.item].next;
            work.phase = work.list_end ? R_ITEM : R_FINISH;
            break;
        case R_FINISH:
            if (!work.used) {
                if (summary_out(out, cap, 0)) return retain_fail(out, cap, "RETENTION", "The summary does not fit.", 0);
                active = 0;
                return 0;
            }
            /* Prove the summary fits before anything reaches the disk, so the
             * final step cannot fail after the artifact is published. */
            if (summary_out(out, cap, 1)) return retain_fail(out, cap, "RETENTION", "The summary does not fit.", 0);
            work.phase = R_STAGE_CREATE;
            break;
        case R_STAGE_CREATE:
            err = FSpCreate(&work.stage_spec, MCP_RESULT_CREATOR, MCP_RESULT_TYPE, smSystemScript);
            if (err) return retain_fail(out, cap, "STAGE", "The retained result could not be created.", err);
            work.stage_created = 1;
            work.phase = R_STAGE_OPEN;
            break;
        case R_STAGE_OPEN:
            err = FSpOpenDF(&work.stage_spec, fsWrPerm, &work.ref);
            if (err) return retain_fail(out, cap, "STAGE", "The retained result could not be created.", err);
            work.ref_open = 1;
            work.phase = R_STAGE_WRITE;
            break;
        case R_STAGE_WRITE: {
            long want = (long)work.used - work.written;
            if (want > MCP_RESULT_CHUNK) want = MCP_RESULT_CHUNK;
            n = want;
            err = FSWrite(work.ref, &n, stage + work.written);
            if (err || n != want)
                return retain_fail(out, cap, "STAGE", "The retained result could not be written.", err ? err : ioErr);
            work.written += n;
            if (work.written < (long)work.used) return 2;
            work.phase = R_STAGE_CLOSE;
            break;
        }
        case R_STAGE_CLOSE:
            err = FSClose(work.ref);
            work.ref_open = 0;
            if (err) return retain_fail(out, cap, "STAGE", "The retained result could not be written.", err);
            work.phase = R_FLUSH;
            break;
        case R_FLUSH:
            err = FlushVol(NULL, 0);
            if (err) return retain_fail(out, cap, "STAGE", "The retained result could not be flushed.", err);
            work.phase = R_VERIFY_OPEN;
            break;
        case R_VERIFY_OPEN: {
            long size;
            err = FSpOpenDF(&work.stage_spec, fsRdPerm, &work.ref);
            if (err) return retain_fail(out, cap, "STAGE", "The retained result could not be read back.", err);
            work.ref_open = 1;
            err = GetEOF(work.ref, &size);
            if (err || size != (long)work.used)
                return retain_fail(out, cap, "VERIFY", "The retained result did not read back whole.", err ? err : ioErr);
            work.checked = 0;
            work.phase = R_VERIFY_READ;
            break;
        }
        case R_VERIFY_READ: {
            long want = (long)work.used - work.checked;
            if (want > MCP_RESULT_CHUNK) want = MCP_RESULT_CHUNK;
            n = want;
            err = FSRead(work.ref, &n, verify);
            if (err || n != want)
                return retain_fail(out, cap, "VERIFY", "The retained result did not read back whole.", err ? err : ioErr);
            if (memcmp(verify, stage + work.checked, (size_t)n))
                return retain_fail(out, cap, "VERIFY", "The retained result did not read back equal.", ioErr);
            work.checked += n;
            if (work.checked < (long)work.used) return 2;
            work.phase = R_VERIFY_CLOSE;
            break;
        }
        case R_VERIFY_CLOSE:
            err = FSClose(work.ref);
            work.ref_open = 0;
            if (err) return retain_fail(out, cap, "VERIFY", "The retained result could not be read back.", err);
            work.phase = R_PUBLISH;
            break;
        case R_PUBLISH: {
            FSSpec spec;
            CInfoPBRec pb;
            Str255 name;
            pstr(name, work.id);
            err = FSpRename(&work.stage_spec, name);
            if (err) return retain_fail(out, cap, "RETENTION", "The retained result could not be published.", err);
            work.renamed = 1;
            pstr(name, work.final_path);
            if (FSMakeFSSpec(0, 0, name, &spec))
                return retain_fail(out, cap, "RETENTION", "The retained result could not be published.", ioErr);
            memset(&pb, 0, sizeof(pb));
            pb.hFileInfo.ioNamePtr = spec.name;
            pb.hFileInfo.ioVRefNum = spec.vRefNum;
            pb.hFileInfo.ioDirID = spec.parID;
            if (PBGetCatInfoSync(&pb) || (pb.hFileInfo.ioFlAttrib & 16) ||
                (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) ||
                pb.hFileInfo.ioFlFndrInfo.fdType != MCP_RESULT_TYPE ||
                pb.hFileInfo.ioFlLgLen != (long)work.used)
                return retain_fail(out, cap, "RETENTION", "The published result failed its catalog check.", ioErr);
            work.phase = R_JOURNAL;
            break;
        }
        case R_JOURNAL:
            if (journal_event())
                return retain_fail(out, cap, "JOURNAL",
                    "The retained result is not recorded; it is not advertised. Inspect the session.", 0);
            work.phase = R_SUMMARY;
            break;
        case R_SUMMARY: {
            McpResultEntry *entry;
            if (summary_out(out, cap, 1)) return retain_fail(out, cap, "RETENTION", "The summary does not fit.", 0);
            entry = &results[result_count++];
            strcpy(entry->id, work.id);
            snprintf(entry->path, sizeof(entry->path), "%s%s", work.folder, work.id);
            entry->bytes = (long)work.used;
            entry->is_error = work.is_error;
            strcpy(entry->error_code, work.error_code);
            active = 0;
            return 0;
        }
        default:
            return retain_fail(out, cap, "RETENTION", "Retention reached an unknown phase.", 0);
        }
    }
}

/* ── read_mcp_result ─────────────────────────────────────────────────── */

static int valid_id(const char *id)
{
    int i;
    if (strlen(id) != MCP_RESULT_ID_LEN || strncmp(id, "mcp-", 4)) return 0;
    for (i = 4; i < 12; i++) if (!isxdigit((unsigned char)id[i])) return 0;
    if (id[12] != '-') return 0;
    for (i = 13; i < MCP_RESULT_ID_LEN; i++) if (!isxdigit((unsigned char)id[i])) return 0;
    return 1;
}

/* Only result_id and start_byte, no duplicates (the tools.c argument rules). */
static int read_args(const char *s, const JsonToken *t, char *id, size_t cap, long *start)
{
    int i, j, at = -1;
    char key[64];
    *start = 0;
    for (i = 1; i < t[0].next; i = t[i + 1].next) {
        if (json_string(s, t, i, key, sizeof(key)) < 0 ||
            (strcmp(key, "result_id") && strcmp(key, "start_byte"))) return -1;
        for (j = 1; j < i; j = t[j + 1].next) {
            char previous[64];
            if (json_string(s, t, j, previous, sizeof(previous)) < 0 || !strcmp(previous, key)) return -1;
        }
    }
    at = json_member(s, t, 0, "result_id");
    if (at < 0 || json_string(s, t, at, id, cap) < 0) return -1;
    at = json_member(s, t, 0, "start_byte");
    if (at >= 0) {
        long value = 0;
        int k;
        if (t[at].type != JSON_PRIMITIVE || t[at].end <= t[at].start || t[at].end - t[at].start > 10)
            return -1;
        for (k = t[at].start; k < t[at].end; k++) {
            int d = (unsigned char)s[k] - '0';
            if (d < 0 || d > 9 || value > (2147483647L - d) / 10) return -1;
            value = value * 10 + d;
        }
        *start = value;
    }
    return 0;
}

int mcp_result_read(const AgentCall *call, char *out, size_t cap)
{
    static char page[MCP_RESULT_PAGE + 8];
    JsonToken t[32];
    McpResultEntry *entry = NULL;
    char id[MCP_RESULT_ID_LEN + 2], tail[192];
    FSSpec spec;
    CInfoPBRec pb;
    Str255 path;
    long start, n, limit;
    size_t at = 0;
    short ref;
    int i, err;

    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return 1; }
    cap = AGENT_RESULT_CAP;
    if (!call || strlen(call->arguments) >= AGENT_ARGUMENT_CAP ||
        json_parse(call->arguments, strlen(call->arguments), t, 32) < 1 || t[0].type != JSON_OBJECT ||
        read_args(call->arguments, t, id, sizeof(id), &start)) {
        fail(out, cap, "ARGUMENTS", "Expected only result_id and a start_byte offset.", 0);
        return 0;
    }
    if (!valid_id(id)) {
        fail(out, cap, "STALE_ID", "No retained result with this ID is available in this session.", 0);
        return 0;
    }
    for (i = 0; i < result_count; i++)
        if (!strcmp(results[i].id, id)) { entry = &results[i]; break; }
    if (!entry) {
        fail(out, cap, "STALE_ID", "No retained result with this ID is available in this session.", 0);
        return 0;
    }
    pstr(path, entry->path);
    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err) { fail(out, cap, "CHANGED", "The retained result changed on disk; it is no longer trustworthy.", err); return 0; }
    memset(&pb, 0, sizeof(pb));
    pb.hFileInfo.ioNamePtr = spec.name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    if (PBGetCatInfoSync(&pb) || (pb.hFileInfo.ioFlAttrib & 16) ||
        (pb.hFileInfo.ioFlFndrInfo.fdFlags & 0x8000) ||
        pb.hFileInfo.ioFlFndrInfo.fdType != MCP_RESULT_TYPE ||
        pb.hFileInfo.ioFlLgLen != entry->bytes) {
        fail(out, cap, "CHANGED", "The retained result changed on disk; it is no longer trustworthy.", 0);
        return 0;
    }
    if (start > entry->bytes) {
        fail(out, cap, "RANGE", "start_byte lies beyond the end of the retained result.", 0);
        return 0;
    }
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err) { fail(out, cap, "READ", "The retained result could not be read.", err); return 0; }
    err = SetFPos(ref, fsFromStart, start);
    if (!err) {
        n = entry->bytes - start;
        if (n > MCP_RESULT_PAGE + 3) n = MCP_RESULT_PAGE + 3;
        err = FSRead(ref, &n, page);
        if (!err && n <= 0 && start < entry->bytes) err = ioErr;
    }
    if (FSClose(ref) && !err) err = ioErr;
    if (err) { fail(out, cap, "READ", "The retained result could not be read.", err); return 0; }
    if (n > 0 && (page[0] & 0xc0) == 0x80) {
        fail(out, cap, "RANGE", "start_byte falls inside a multi-byte character; use a next_byte value.", 0);
        return 0;
    }
    /* Never split a multi-byte character: extend the page to the next
     * boundary, at most three bytes past the cap. */
    limit = start + MCP_RESULT_PAGE;
    if (limit > entry->bytes) limit = entry->bytes;
    while (limit < entry->bytes && limit - start < n && (page[limit - start] & 0xc0) == 0x80) limit++;
    page[limit - start] = 0;
    snprintf(tail, sizeof(tail),
        ",\"content_type\":\"text\",\"bytes\":%ld,\"is_error\":%s,\"start_byte\":%ld,"
        "\"next_byte\":%ld,\"truncated\":%s,\"text\":",
        entry->bytes, entry->is_error ? "true" : "false",
        start, limit, limit < entry->bytes ? "true" : "false");
    if (append(out, cap, &at, "{\"status\":\"ok\",\"result_id\":") ||
        quote_at(out, cap, &at, id) ||
        append(out, cap, &at, tail) ||
        quote_at(out, cap, &at, page) ||
        append(out, cap, &at, "}")) {
        fail(out, cap, "ENCODING", "The retained result is no longer valid UTF-8.", 0);
        return 0;
    }
    return 0;
}
