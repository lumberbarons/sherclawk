/* Read-only OS 9 inspection tools: resource maps and bounded resource bytes,
 * Finder catalog identity, alias resolution, Process Manager liveness and
 * QuickDraw text metrics. Nothing here writes to a volume or changes Finder
 * state; results are bounded and paginate with next_* cursors. Resource reads
 * never load whole resources: handles are opened with automatic loading
 * disabled, sizes come from the map, and bytes arrive in pages through
 * ReadPartialResource. */
#include "inspect.h"
#include "tools.h"
#include "json.h"
#include "text.h"
#include "config.h"
#include <Files.h>
#include <Resources.h>
#include <Aliases.h>
#include <Processes.h>
#include <Fonts.h>
#include <Quickdraw.h>
#include <QuickdrawText.h>
#include <Memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESOURCE_NAME_CAP 32
#define ALIAS_BYTES_MAX 32768
#define WALK_BUDGET 512

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
static int string_arg(const char *s, const JsonToken *tokens, const char *name, char *out, size_t cap)
{
    return json_string(s, tokens, json_member(s, tokens, 0, name), out, cap);
}
/* Optional signed integer; absent keeps the default, malformed fails. */
static int number_arg(const char *s, const JsonToken *tokens, const char *name, long def,
                      long low, long high, long *out)
{
    char value[24];
    long n;
    int i = json_member(s, tokens, 0, name), k, size, at = 0;
    if (i < 0) { *out = def; return 0; }
    size = tokens[i].end - tokens[i].start;
    if (tokens[i].type != JSON_PRIMITIVE || size < 1 || size >= (int)sizeof(value)) return -1;
    if (s[tokens[i].start] == '-') at = 1;
    if (at >= size) return -1;
    for (k = at; k < size; k++) if (s[tokens[i].start + k] < '0' || s[tokens[i].start + k] > '9') return -1;
    memcpy(value, s + tokens[i].start, (size_t)size); value[size] = 0;
    n = strtol(value, NULL, 10);
    if (n < low || n > high) return -1;
    *out = n; return 0;
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
static OSErr catalog(const FSSpec *spec, CInfoPBRec *pb)
{
    memset(pb, 0, sizeof(*pb)); pb->hFileInfo.ioNamePtr = (unsigned char *)spec->name;
    pb->hFileInfo.ioVRefNum = spec->vRefNum; pb->hFileInfo.ioDirID = spec->parID;
    return PBGetCatInfoSync(pb);
}
static int same_file(const CInfoPBRec *a, const CInfoPBRec *b)
{
    return a->hFileInfo.ioDirID == b->hFileInfo.ioDirID &&
        a->hFileInfo.ioFlMdDat == b->hFileInfo.ioFlMdDat &&
        a->hFileInfo.ioFlLgLen == b->hFileInfo.ioFlLgLen &&
        a->hFileInfo.ioFlRLgLen == b->hFileInfo.ioFlRLgLen &&
        (a->hFileInfo.ioFlAttrib & 0x11) == (b->hFileInfo.ioFlAttrib & 0x11) &&
        a->hFileInfo.ioFlFndrInfo.fdType == b->hFileInfo.ioFlFndrInfo.fdType &&
        a->hFileInfo.ioFlFndrInfo.fdCreator == b->hFileInfo.ioFlFndrInfo.fdCreator &&
        a->hFileInfo.ioFlFndrInfo.fdFlags == b->hFileInfo.ioFlFndrInfo.fdFlags;
}
static int is_alias(const CInfoPBRec *pb)
{
    return pb->hFileInfo.ioFlFndrInfo.fdType == 'alis' || (pb->hFileInfo.ioFlFndrInfo.fdFlags & 0x8000);
}
/* Four-character types stay readable; anything with control or high bytes is
 * rendered as a hex literal, which read_resource accepts in return. */
static void type_name(unsigned long value, char *out)
{
    unsigned char bytes[4];
    int i, printable = 1;
    bytes[0] = (unsigned char)(value >> 24); bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8); bytes[3] = (unsigned char)value;
    for (i = 0; i < 4; i++) if (bytes[i] < 32 || bytes[i] > 126) printable = 0;
    if (printable) { memcpy(out, bytes, 4); out[4] = 0; }
    else snprintf(out, 11, "0x%08lx", value & 0xffffffffUL);
}
static int parse_type(const char *s, unsigned long *out)
{
    size_t n = strlen(s), i;
    unsigned long value = 0;
    if (n == 4) {
        for (i = 0; i < 4; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c < 32 || c > 126) return -1;
            value = (value << 8) | c;
        }
        *out = value; return 0;
    }
    if (n == 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (i = 2; i < 10; i++) {
            char c = s[i]; int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return -1;
            value = (value << 4) | (unsigned long)digit;
        }
        *out = value; return 0;
    }
    return -1;
}
static int name_json(const Str255 name, char *out, size_t cap, int *truncated)
{
    char utf[128];
    size_t n = name[0];
    if (n > RESOURCE_NAME_CAP) { n = RESOURCE_NAME_CAP; if (truncated) *truncated = 1; }
    if (text_to_utf8((const char *)name + 1, n, utf, sizeof(utf)) < 0) return -1;
    return json_quote(utf, out, cap) < 0 ? -1 : 0;
}
static void civil_from_days(long long z, long *yy, unsigned *mm, unsigned *dd)
{
    long long era, y;
    unsigned long long doe, yoe, doy, mp, day, month;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long long)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    *yy = (long)(y + (month <= 2)); *mm = (unsigned)month; *dd = (unsigned)day;
}
/* Catalog dates are local seconds since 1904-01-01; format deterministically
 * instead of depending on International Utilities resource state. */
static void format_date(unsigned long seconds, char *out, size_t cap)
{
    long long total = (long long)seconds - 2082844800LL;
    long long days = total / 86400, rem = total % 86400;
    long y; unsigned m, d, hh, mm, ss;
    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &y, &m, &d);
    hh = (unsigned)(rem / 3600); rem %= 3600;
    mm = (unsigned)(rem / 60); ss = (unsigned)(rem % 60);
    snprintf(out, cap, "%04ld-%02u-%02u %02u:%02u:%02u", y, m, d, hh, mm, ss);
}
static int parse_hex32(const char **p, unsigned long *out)
{
    unsigned long value = 0;
    int digits = 0;
    while (**p && **p != ':') {
        char c = **p; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        value = (value << 4) | (unsigned long)d;
        if (++digits > 8) return -1;
        (*p)++;
    }
    if (!digits) return -1;
    *out = value; return 0;
}
/* ------------------------------------------------------------------ */
static void file_info(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    char path[512], file_type[12], creator[12], created[32], modified[32], tail[320];
    FSSpec spec;
    CInfoPBRec pb;
    OSErr err;
    size_t at = 0;
    long flags;
    if (valid_keys(call->arguments, tokens, "|path|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected only a path string.", 0); return;
    }
    err = tools_resolve(path, &spec);
    if (!err) err = catalog(&spec, &pb);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace path.", err); return; }
    format_date(pb.hFileInfo.ioFlCrDat, created, sizeof(created));
    format_date(pb.hFileInfo.ioFlMdDat, modified, sizeof(modified));
    flags = (long)pb.hFileInfo.ioFlFndrInfo.fdFlags;
    if (append(out, cap, &at, "{\"status\":\"ok\",\"path\":") || quote(out, cap, &at, path)) goto limit;
    if (pb.hFileInfo.ioFlAttrib & 16) {
        if (append(out, cap, &at, ",\"kind\":\"folder\"")) goto limit;
    } else {
        type_name(pb.hFileInfo.ioFlFndrInfo.fdType, file_type);
        type_name(pb.hFileInfo.ioFlFndrInfo.fdCreator, creator);
        if (append(out, cap, &at, ",\"kind\":\"file\",\"file_type\":") || quote(out, cap, &at, file_type) ||
            append(out, cap, &at, ",\"creator\":") || quote(out, cap, &at, creator)) goto limit;
    }
    snprintf(tail, sizeof(tail), ",\"finder_flags\":\"0x%04lx\",\"alias\":%s,\"custom_icon\":%s,\"bundle\":%s,"
        "\"invisible\":%s,\"locked\":%s,\"label\":%ld,\"data_bytes\":%ld,\"resource_bytes\":%ld,"
        "\"created\":\"%s\",\"modified\":\"%s\"}",
        (unsigned long)flags & 0xffffUL,
        is_alias(&pb) ? "true" : "false",
        (flags & 0x0400) ? "true" : "false", (flags & 0x2000) ? "true" : "false",
        (flags & 0x4000) ? "true" : "false", (pb.hFileInfo.ioFlAttrib & 1) ? "true" : "false",
        (flags >> 1) & 7,
        (long)((pb.hFileInfo.ioFlAttrib & 16) ? 0 : pb.hFileInfo.ioFlLgLen),
        (long)((pb.hFileInfo.ioFlAttrib & 16) ? 0 : pb.hFileInfo.ioFlRLgLen),
        created, modified);
    if (append(out, cap, &at, tail)) goto limit;
    return;
limit:
    fail(out, cap, "LIMIT", "File information exceeds the result capacity.", 0);
}
/* ------------------------------------------------------------------ */
static void list_processes(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    char cursor[48];
    ProcessSerialNumber psn, current, front, last;
    ProcessInfoRec info;
    Str255 process_name;
    FSSpec app_spec;
    OSErr err;
    long limit;
    int count = 0, done = 0, truncated = 0;
    size_t at = 0;
    if (valid_keys(call->arguments, tokens, "|cursor||limit|") ||
        number_arg(call->arguments, tokens, "limit", 8, 1, 12, &limit) ||
        (json_member(call->arguments, tokens, 0, "cursor") >= 0 &&
         string_arg(call->arguments, tokens, "cursor", cursor, sizeof(cursor)) < 0)) {
        fail(out, cap, "ARGUMENTS", "Expected optional cursor (returned value) and limit 1-12.", 0); return;
    }
    memset(&psn, 0, sizeof(psn));
    if (json_member(call->arguments, tokens, 0, "cursor") >= 0 && cursor[0]) {
        const char *p = cursor;
        if (parse_hex32(&p, &psn.highLongOfPSN) || *p != ':') {
            fail(out, cap, "ARGUMENTS", "Cursor must be a returned high:low ProcessSerialNumber.", 0); return;
        }
        p++;
        if (parse_hex32(&p, &psn.lowLongOfPSN) || *p) {
            fail(out, cap, "ARGUMENTS", "Cursor must be a returned high:low ProcessSerialNumber.", 0); return;
        }
    }
    GetCurrentProcess(&current);
    GetFrontProcess(&front);
    last = psn;
    if (append(out, cap, &at, "{\"status\":\"ok\",\"processes\":[")) goto limit;
    while (count < limit) {
        char entry[400], psn_text[24], name_utf[512], app_utf[512];
        size_t pos = 0;
        int self, is_front, have_app = 0;
        err = GetNextProcess(&psn);
        if (err) {
            if (err == procNotFound) { done = 1; break; }
            fail(out, cap, "PROCESS", "Process enumeration failed.", err); return;
        }
        memset(&process_name, 0, sizeof(process_name));
        memset(&app_spec, 0, sizeof(app_spec));
        memset(&info, 0, sizeof(info));
        info.processInfoLength = sizeof(ProcessInfoRec);
        info.processName = process_name;
        info.processAppSpec = &app_spec;
        GetProcessInformation(&psn, &info);
        if (text_to_utf8((const char *)process_name + 1, process_name[0], name_utf, sizeof(name_utf)) < 0)
            name_utf[0] = 0;
        if (app_spec.name[0] &&
            text_to_utf8((const char *)app_spec.name + 1, app_spec.name[0], app_utf, sizeof(app_utf)) >= 0) have_app = 1;
        self = psn.highLongOfPSN == current.highLongOfPSN && psn.lowLongOfPSN == current.lowLongOfPSN;
        is_front = psn.highLongOfPSN == front.highLongOfPSN && psn.lowLongOfPSN == front.lowLongOfPSN;
        snprintf(psn_text, sizeof(psn_text), "%08lx:%08lx",
            (unsigned long)psn.highLongOfPSN, (unsigned long)psn.lowLongOfPSN);
        if (append(entry, sizeof(entry), &pos, "{\"name\":") || quote(entry, sizeof(entry), &pos, name_utf) ||
            append(entry, sizeof(entry), &pos, ",\"psn\":") || quote(entry, sizeof(entry), &pos, psn_text) ||
            append(entry, sizeof(entry), &pos, is_front ? ",\"front\":true" : ",\"front\":false") ||
            append(entry, sizeof(entry), &pos, self ? ",\"self\":true" : ",\"self\":false")) {
            fail(out, cap, "LIMIT", "Process entry exceeds the result capacity.", 0); return;
        }
        if (have_app) {
            if (append(entry, sizeof(entry), &pos, ",\"app\":") || quote(entry, sizeof(entry), &pos, app_utf)) {
                fail(out, cap, "LIMIT", "Process entry exceeds the result capacity.", 0); return;
            }
        }
        if (append(entry, sizeof(entry), &pos, "}")) { fail(out, cap, "LIMIT", "Process entry exceeds the result capacity.", 0); return; }
        if (at + pos + 120 >= cap) { truncated = 1; break; }
        if (count && append(out, cap, &at, ",")) { fail(out, cap, "LIMIT", "Result exceeds capacity.", 0); return; }
        if (append(out, cap, &at, entry)) { fail(out, cap, "LIMIT", "Result exceeds capacity.", 0); return; }
        last = psn;
        count++;
    }
    if (count == limit && !done) truncated = 1;
    if (append(out, cap, &at, "],\"truncated\":")) goto limit;
    if (append(out, cap, &at, truncated ? "true" : "false")) goto limit;
    if (truncated) {
        char psn_text[40];
        snprintf(psn_text, sizeof(psn_text), ",\"next_cursor\":\"%08lx:%08lx\"}",
            (unsigned long)last.highLongOfPSN, (unsigned long)last.lowLongOfPSN);
        if (append(out, cap, &at, psn_text)) goto limit;
    } else if (append(out, cap, &at, ",\"next_cursor\":null}")) goto limit;
    return;
limit:
    fail(out, cap, "LIMIT", "Process listing exceeds the result capacity.", 0);
}
/* ------------------------------------------------------------------ */
static void list_fonts(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    FMFontFamilyIterator iterator;
    FMFontFamily family;
    OSStatus status;
    long cursor, limit, index = 0, count = 0;
    int truncated = 0;
    size_t at = 0;
    if (valid_keys(call->arguments, tokens, "|cursor||limit|") ||
        number_arg(call->arguments, tokens, "cursor", 0, 0, 100000, &cursor) ||
        number_arg(call->arguments, tokens, "limit", 16, 1, 24, &limit)) {
        fail(out, cap, "ARGUMENTS", "Expected optional cursor (returned value) and limit 1-24.", 0); return;
    }
    status = FMCreateFontFamilyIterator(NULL, NULL, 0, &iterator);
    if (status) { fail(out, cap, "FONTS", "Font Manager enumeration is unavailable.", (int)status); return; }
    if (append(out, cap, &at, "{\"status\":\"ok\",\"fonts\":[")) goto limit;
    /* A nonzero return means the iterator is exhausted. */
    while (!FMGetNextFontFamily(&iterator, &family)) {
        Str255 name;
        char utf[128], entry[300];
        size_t pos = 0;
        if (index++ < cursor) continue;
        if (count == limit) { truncated = 1; break; }
        name[0] = 0;
        FMGetFontFamilyName(family, name);
        if (text_to_utf8((const char *)name + 1, name[0], utf, sizeof(utf)) < 0) utf[0] = 0;
        if (append(entry, sizeof(entry), &pos, "{\"id\":")) goto limit;
        snprintf(entry + pos, sizeof(entry) - pos, "%d,\"name\":", (int)family);
        pos += strlen(entry + pos);
        if (quote(entry, sizeof(entry), &pos, utf)) goto limit;
        if (append(entry, sizeof(entry), &pos, "}")) goto limit;
        if (at + pos + 60 >= cap) { truncated = 1; break; }
        if (count && append(out, cap, &at, ",")) goto limit;
        if (append(out, cap, &at, entry)) goto limit;
        count++;
    }
    FMDisposeFontFamilyIterator(&iterator);
    if (append(out, cap, &at, "],\"truncated\":")) goto limit;
    if (append(out, cap, &at, truncated ? "true" : "false")) goto limit;
    if (truncated) {
        char tail[64];
        snprintf(tail, sizeof(tail), ",\"next_cursor\":%ld}", cursor + count);
        if (append(out, cap, &at, tail)) goto limit;
    } else if (append(out, cap, &at, ",\"next_cursor\":null}")) goto limit;
    return;
limit:
    fail(out, cap, "LIMIT", "Font listing exceeds the result capacity.", 0);
}
/* ------------------------------------------------------------------ */
static int style_parse(const char *s, long *bits, char *canonical, size_t cap)
{
    char token[24];
    size_t at = 0, length = strlen(s);
    long value = 0;
    if (!length || !strcmp(s, "plain")) { *bits = 0; snprintf(canonical, cap, "plain"); return 0; }
    while (at <= length) {
        size_t start = at, n;
        while (at < length && s[at] != ',') at++;
        while (start < at && s[start] == ' ') start++;
        while (at > start && s[at - 1] == ' ') at--;
        n = at - start;
        if (!n || n >= sizeof(token)) return -1;
        { size_t i; for (i = 0; i < n; i++) {
            char c = s[start + i];
            token[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        } token[n] = 0; }
        if (!strcmp(token, "bold")) value |= bold;
        else if (!strcmp(token, "italic")) value |= italic;
        else if (!strcmp(token, "underline")) value |= underline;
        else if (!strcmp(token, "outline")) value |= outline;
        else if (!strcmp(token, "shadow")) value |= shadow;
        else if (!strcmp(token, "condense")) value |= condense;
        else if (!strcmp(token, "extend")) value |= extend;
        else return -1;
        at++;
    }
    *bits = value;
    if (!value) { snprintf(canonical, cap, "plain"); return 0; }
    canonical[0] = 0;
    if (value & bold) strcat(canonical, "bold");
    if (value & italic) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "italic"); }
    if (value & underline) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "underline"); }
    if (value & outline) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "outline"); }
    if (value & shadow) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "shadow"); }
    if (value & condense) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "condense"); }
    if (value & extend) { if (canonical[0]) strcat(canonical, ","); strcat(canonical, "extend"); }
    return 0;
}
/* Font names are MacRoman; family 0 (Chicago) is valid, so a name lookup is
 * verified by round-tripping the family name instead of trusting a zero. */
static int font_lookup(const char *name, long id, int have_id, FMFontFamily *family, char *display, size_t cap)
{
    Str255 native, check;
    if (have_id) {
        *family = (FMFontFamily)id;
        check[0] = 0;
        FMGetFontFamilyName(*family, check);
        if (!check[0]) return -1;
        return text_to_utf8((const char *)check + 1, check[0], display, cap) < 0 ? -1 : 0;
    }
    if (text_to_macroman_strict(name, (char *)native + 1, sizeof(native) - 1) <= 0) return -1;
    native[0] = (unsigned char)strlen((char *)native + 1);
    *family = FMGetFontFamilyFromName(native);
    check[0] = 0;
    FMGetFontFamilyName(*family, check);
    if (check[0] != native[0]) return -1;
    { size_t i; for (i = 0; i < native[0]; i++) {
        unsigned char a = check[i + 1], b = native[i + 1];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 32);
        if (a != b) return -1;
    } }
    return text_to_utf8((const char *)check + 1, check[0], display, cap) < 0 ? -1 : 0;
}
static void measure_text(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    static char bytes[257];
    char text[512], font[128], style[64], canonical[96], display[128];
    FMFontFamily family;
    FontInfo info;
    GrafPtr port;
    StyleField old_face;
    short old_font, old_size, width;
    long size, bits, font_id = -1;
    int length, have_name, have_id, i;
    if (valid_keys(call->arguments, tokens, "|text||font||font_id||size||style|") ||
        string_arg(call->arguments, tokens, "text", text, sizeof(text)) < 0 ||
        number_arg(call->arguments, tokens, "font_id", -1, 0, 32767, &font_id) ||
        number_arg(call->arguments, tokens, "size", 12, 1, 127, &size)) {
        fail(out, cap, "ARGUMENTS", "Expected text, one of font (name) or font_id, optional size 1-127 and style.", 0); return;
    }
    have_name = json_member(call->arguments, tokens, 0, "font") >= 0;
    if (have_name && string_arg(call->arguments, tokens, "font", font, sizeof(font)) < 0) {
        fail(out, cap, "ARGUMENTS", "font must be a name string.", 0); return;
    }
    if (have_name && !font[0]) have_name = 0; /* models may send an empty placeholder */
    have_id = font_id >= 0;
    if (!have_name && !have_id) {
        fail(out, cap, "ARGUMENTS", "Provide exactly one of font (name) or font_id.", 0); return;
    }
    /* Models often fill both optional fields; accept them only when the name
     * and id resolve to the same installed family. */
    if (have_name && have_id) {
        FMFontFamily named;
        char named_display[128];
        if (font_lookup(font, -1, 0, &named, named_display, sizeof(named_display)) ||
            named != (FMFontFamily)font_id) {
            fail(out, cap, "ARGUMENTS", "font and font_id do not name the same family; provide just one.", 0); return;
        }
    }
    if (json_member(call->arguments, tokens, 0, "style") >= 0) {
        if (string_arg(call->arguments, tokens, "style", style, sizeof(style)) < 0) {
            fail(out, cap, "ARGUMENTS", "style must be a string.", 0); return;
        }
    } else strcpy(style, "plain");
    length = text_to_macroman_strict(text, bytes, sizeof(bytes));
    if (length < 0 || length > 256) {
        fail(out, cap, "ENCODING_LIMIT", "text must be MacRoman and at most 256 encoded bytes.", 0); return;
    }
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 32 || c == 127) { fail(out, cap, "NOT_TEXT", "measure_text accepts one printable line; controls are refused.", 0); return; }
    }
    if (style_parse(style, &bits, canonical, sizeof(canonical))) {
        fail(out, cap, "ARGUMENTS", "Unknown style; use plain or comma-separated bold, italic, underline, outline, shadow, condense, extend.", 0); return;
    }
    if (font_lookup(font, font_id, have_id, &family, display, sizeof(display))) {
        fail(out, cap, "NOT_FOUND", "No installed font family matches; use list_fonts.", 0); return;
    }
    GetPort(&port);
    old_font = port->txFont; old_size = port->txSize; old_face = port->txFace;
    TextFont(family); TextSize((short)size); TextFace((StyleParameter)bits);
    width = TextWidth(bytes, 0, (short)length);
    GetFontInfo(&info);
    TextFont(old_font); TextSize(old_size); TextFace(old_face);
    {
        char fields[320];
        size_t at = 0;
        if (append(out, cap, &at, "{\"status\":\"ok\",\"font\":") || quote(out, cap, &at, display)) {
            fail(out, cap, "LIMIT", "Font name exceeds the result capacity.", 0); return;
        }
        snprintf(fields, sizeof(fields), ",\"font_id\":%d,\"size\":%ld,\"style\":\"%s\",\"style_bits\":%ld,"
            "\"text_bytes\":%d,\"width\":%d,\"ascent\":%d,\"descent\":%d,\"leading\":%d,\"line_height\":%d}",
            (int)family, size, canonical, bits, length, (int)width,
            (int)info.ascent, (int)info.descent, (int)info.leading,
            (int)(info.ascent + info.descent + info.leading));
        if (append(out, cap, &at, fields)) {
            fail(out, cap, "LIMIT", "Measure result exceeds the result capacity.", 0);
        }
    }
}
/* ------------------------------------------------------------------ */
/* Find the directory with the wanted ID by bounded depth-first catalog
 * order, filling the parent path in MacRoman bytes. The walk stops at 512
 * entries or depth 8 and reports incompleteness rather than guessing. */
static int find_dir(const FSSpec *spec, long dir, const char *prefix, long wanted,
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
        if (depth < 8 && find_dir(spec, pb.dirInfo.ioDrDirID, child, wanted, found, cap, depth + 1, budget, complete))
            return 1;
    }
    *complete = 0;
    return 0;
}
static void resolve_alias(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    static char alias_bytes[ALIAS_BYTES_MAX];
    char path[512], relative[800], relative_utf[1600];
    Str255 workspace_name;
    FSSpec spec, workspace, target;
    CInfoPBRec pb, target_pb, workspace_pb;
    Handle record = NULL;
    Boolean changed = 0;
    OSErr err, target_err;
    short ref = -1;
    long size, count;
    int budget = WALK_BUDGET, complete = 1, have_relative = 0, outside = -1;
    if (valid_keys(call->arguments, tokens, "|path|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0) {
        fail(out, cap, "ARGUMENTS", "Expected only a path string.", 0); return;
    }
    err = tools_resolve(path, &spec);
    if (!err) err = catalog(&spec, &pb);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace alias file.", err); return; }
    if (pb.hFileInfo.ioFlAttrib & 16) { fail(out, cap, "FOLDER", "Expected an alias file, not a folder.", 0); return; }
    if (!is_alias(&pb)) { fail(out, cap, "NOT_ALIAS", "This file is not an alias between its Finder type and flags.", 0); return; }
    size = pb.hFileInfo.ioFlLgLen;
    if (size < 1 || size > ALIAS_BYTES_MAX) {
        fail(out, cap, "ALIAS_LIMIT", "Alias record is empty or larger than 32768 bytes.", 0); return;
    }
    err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (!err) { count = size; err = FSRead(ref, &count, alias_bytes); if (!err && count != size) err = ioErr; }
    { OSErr closed = ref >= 0 ? FSClose(ref) : 0; ref = -1; if (!err) err = closed; }
    if (err) { fail(out, cap, "ALIAS", "Cannot read the alias record from the data fork.", err); return; }
    err = PtrToHand(alias_bytes, &record, size);
    if (!err) err = ResolveAlias(NULL, (AliasHandle)record, &target, &changed);
    if (record) DisposeHandle(record);
    if (err) { fail(out, cap, "ALIAS", "Alias resolution failed; the target volume may not be mounted.", err); return; }
    workspace_name[0] = (unsigned char)strlen(SHERCLAWK_WORKSPACE);
    memcpy(workspace_name + 1, SHERCLAWK_WORKSPACE, workspace_name[0]);
    err = FSMakeFSSpec(0, 0, workspace_name, &workspace);
    if (!err) err = catalog(&workspace, &workspace_pb);
    if (err || !(workspace_pb.hFileInfo.ioFlAttrib & 16)) {
        fail(out, cap, "WORKSPACE", "Cannot resolve the configured workspace root.", err); return;
    }
    target_err = catalog(&target, &target_pb);
    if (target.vRefNum == workspace.vRefNum && target_err == noErr) {
        if (target.parID == workspace_pb.dirInfo.ioDrDirID) {
            snprintf(relative, sizeof(relative), "%.*s", (int)target.name[0], (const char *)target.name + 1);
            have_relative = 1;
        } else {
            char parent[768];
            if (find_dir(&workspace, workspace_pb.dirInfo.ioDrDirID, "", target.parID, parent, sizeof(parent),
                         0, &budget, &complete)) {
                snprintf(relative, sizeof(relative), "%s%.*s", parent, (int)target.name[0], (const char *)target.name + 1);
                have_relative = 1;
            }
        }
    } else {
        outside = 1;
    }
    if (have_relative) outside = 0;
    else if (outside < 0) outside = (target_err == noErr && complete) ? 1 : -1;
    {
        char target_utf[128], quoted[1700];
        size_t at = 0;
        const char *kind = "unknown";
        if (target_err == noErr) kind = (target_pb.hFileInfo.ioFlAttrib & 16) ? "folder" : "file";
        if (text_to_utf8((const char *)target.name + 1, target.name[0], target_utf, sizeof(target_utf)) < 0)
            target_utf[0] = 0;
        if (append(out, cap, &at, "{\"status\":\"ok\",\"path\":") || quote(out, cap, &at, path)) goto limit;
        if (append(out, cap, &at, changed ? ",\"was_changed\":true" : ",\"was_changed\":false")) goto limit;
        {
            char fields[200];
            snprintf(fields, sizeof(fields), ",\"alias_bytes\":%ld,\"target_name\":", size);
            if (append(out, cap, &at, fields) || quote(out, cap, &at, target_utf)) goto limit;
        }
        if (append(out, cap, &at, ",\"target_kind\":\"") || append(out, cap, &at, kind) ||
            append(out, cap, &at, "\",\"target_exists\":") || append(out, cap, &at, target_err == noErr ? "true" : "false")) goto limit;
        if (have_relative) {
            if (text_to_utf8(relative, strlen(relative), relative_utf, sizeof(relative_utf)) < 0) {
                fail(out, cap, "ENCODING", "Target path conversion failed.", 0); return;
            }
            if (json_quote(relative_utf, quoted, sizeof(quoted)) < 0) {
                fail(out, cap, "LIMIT", "Target path exceeds the result capacity.", 0); return;
            }
            if (append(out, cap, &at, ",\"relative_path\":") || append(out, cap, &at, quoted)) goto limit;
        } else if (append(out, cap, &at, ",\"relative_path\":null")) goto limit;
        {
            char fields[160];
            snprintf(fields, sizeof(fields), ",\"outside_workspace\":%s,\"volume\":%d,\"parent_dir_id\":%ld}",
                outside == 1 ? "true" : outside == 0 ? "false" : "null", (int)target.vRefNum, (long)target.parID);
            if (append(out, cap, &at, fields)) goto limit;
        }
    }
    return;
limit:
    fail(out, cap, "LIMIT", "Alias result exceeds the result capacity.", 0);
}
/* ------------------------------------------------------------------ */
static int resource_open(const FSSpec *spec, short *ref)
{
    short handle = FSpOpenResFile(spec, fsRdPerm);
    if (handle < 0) return (int)handle;
    SetResLoad(0);
    UseResFile(handle);
    *ref = handle;
    return 0;
}
static void resource_close(short ref, short previous)
{
    if (ref >= 0) {
        UseResFile(previous);
        CloseResFile(ref);
    }
    SetResLoad(1);
}
static void list_resources(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    char path[512], cursor[48], tail[400];
    FSSpec spec;
    CInfoPBRec before, after;
    OSErr err;
    short ref = -1, previous = 0, types, type_index;
    long limit, cursor_type = 1, cursor_res = 1;
    int count = 0, truncated = 0, names_truncated = 0;
    size_t at = 0;
    if (valid_keys(call->arguments, tokens, "|path||cursor||limit|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0 ||
        number_arg(call->arguments, tokens, "limit", 8, 1, 16, &limit) ||
        (json_member(call->arguments, tokens, 0, "cursor") >= 0 &&
         string_arg(call->arguments, tokens, "cursor", cursor, sizeof(cursor)) < 0)) {
        fail(out, cap, "ARGUMENTS", "Expected path, optional returned cursor and limit 1-16.", 0); return;
    }
    if (json_member(call->arguments, tokens, 0, "cursor") >= 0 && cursor[0]) {
        const char *p = cursor;
        char *end;
        long a, b;
        a = strtol(p, &end, 10); if (end == p || *end != ':' || a < 1 || a > 32767) goto arguments;
        b = strtol(end + 1, &end, 10); if (*end || b < 1 || b > 32767) goto arguments;
        cursor_type = a; cursor_res = b;
    }
    err = tools_resolve(path, &spec);
    if (!err) err = catalog(&spec, &before);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace file.", err); return; }
    if (before.hFileInfo.ioFlAttrib & 16) { fail(out, cap, "FOLDER", "Expected a file, not a folder.", 0); return; }
    if (is_alias(&before)) { fail(out, cap, "ALIAS", "Aliases are not resource files.", 0); return; }
    if (before.hFileInfo.ioFlRLgLen == 0) { fail(out, cap, "NO_RESOURCE_FORK", "This file has no resource fork.", 0); return; }
    previous = CurResFile();
    err = resource_open(&spec, &ref);
    if (err) { fail(out, cap, "RESOURCE_MAP", "Cannot open the resource fork's map.", err); return; }
    types = Count1Types();
    if (append(out, cap, &at, "{\"status\":\"ok\",\"path\":") || quote(out, cap, &at, path) ||
        append(out, cap, &at, ",\"resources\":[")) goto limit;
    for (type_index = 1; type_index <= types && !truncated; type_index++) {
        ResType type = 0;
        short resources, resource_index;
        if (type_index < cursor_type) continue;
        Get1IndType(&type, type_index);
        if (ResError()) { resource_close(ref, previous); fail(out, cap, "RESOURCE_MAP", "Resource type enumeration failed.", 0); return; }
        resources = Count1Resources(type);
        for (resource_index = 1; resource_index <= resources; resource_index++) {
            Handle resource;
            short id = 0;
            ResType actual = 0;
            Str255 name;
            long bytes;
            if (type_index == cursor_type && resource_index < cursor_res) continue;
            if (count == limit) {
                truncated = 1;
                cursor_type = type_index; cursor_res = resource_index;
                break;
            }
            resource = Get1IndResource(type, resource_index);
            if (!resource) { resource_close(ref, previous); fail(out, cap, "RESOURCE_MAP", "Resource entry disappeared.", 0); return; }
            name[0] = 0;
            GetResInfo(resource, &id, &actual, name);
            bytes = GetResourceSizeOnDisk(resource);
            ReleaseResource(resource);
            if (ResError()) { resource_close(ref, previous); fail(out, cap, "RESOURCE_MAP", "Resource information failed.", 0); return; }
            {
                char type_text[12], entry[500], quoted[400];
                size_t pos = 0;
                int name_cut = 0;
                type_name(actual, type_text);
                if (append(entry, sizeof(entry), &pos, "{\"type\":") || quote(entry, sizeof(entry), &pos, type_text)) goto limit;
                snprintf(entry + pos, sizeof(entry) - pos, ",\"id\":%d,\"bytes\":%ld,\"name\":", (int)id, bytes);
                pos += strlen(entry + pos);
                if (name_json(name, quoted, sizeof(quoted), &name_cut)) goto limit;
                if (append(entry, sizeof(entry), &pos, quoted) || append(entry, sizeof(entry), &pos, "}")) goto limit;
                if (name_cut) names_truncated = 1;
                if (at + pos + 80 >= cap) {
                    truncated = 1;
                    cursor_type = type_index; cursor_res = resource_index;
                    break;
                }
                if (count && append(out, cap, &at, ",")) goto limit;
                if (append(out, cap, &at, entry)) goto limit;
                count++;
            }
        }
    }
    resource_close(ref, previous);
    err = catalog(&spec, &after);
    if (err || !same_file(&before, &after)) { fail(out, cap, "CHANGED", "File changed during resource listing; list resources again.", err); return; }
    snprintf(tail, sizeof(tail), "],\"resource_fork_bytes\":%ld,\"types\":%d,\"names_truncated\":%s,\"truncated\":%s",
        (long)before.hFileInfo.ioFlRLgLen, (int)types, names_truncated ? "true" : "false", truncated ? "true" : "false");
    if (append(out, cap, &at, tail)) goto limit;
    if (truncated) {
        char next[64];
        snprintf(next, sizeof(next), ",\"next_cursor\":\"%ld:%ld\"}", cursor_type, cursor_res);
        if (append(out, cap, &at, next)) goto limit;
    } else if (append(out, cap, &at, ",\"next_cursor\":null}")) goto limit;
    return;
arguments:
    fail(out, cap, "ARGUMENTS", "Cursor must be a returned type:resource index pair.", 0); return;
limit:
    resource_close(ref, previous);
    fail(out, cap, "LIMIT", "Resource listing exceeds the result capacity.", 0);
}
static int resource_read(Handle resource, long offset, long count, char *buffer)
{
    if (count > 0) {
        ReadPartialResource(resource, offset, buffer, count);
        if (ResError()) return -1;
    }
    return 0;
}
/* 'STR ' strings are length-prefixed; the page cursor addresses decoded
 * content, not the resource's raw bytes. */
static int string_bounds(Handle resource, long size, long *offset, long *length)
{
    unsigned char head[2];
    long declared;
    *offset = 0; *length = size;
    if (size == 0) return 0;
    if (resource_read(resource, 0, 1, (char *)head)) return -1;
    if (head[0] <= 127) { declared = head[0]; *offset = 1; }
    else {
        if (size < 2) return -1;
        if (resource_read(resource, 1, 1, (char *)head + 1)) return -1;
        declared = ((long)head[0] << 8) | head[1];
        *offset = 2;
    }
    if (declared > size - *offset) declared = size - *offset;
    *length = declared;
    return 0;
}
static void read_vers(Handle resource, short id, const Str255 name, long size,
                      const char *path, const char *type_text, char *out, size_t cap)
{
    static char whole[257];
    char short_utf[400], long_utf[400], short_q[820], long_q[820], name_q[600];
    unsigned char *b = (unsigned char *)whole;
    int version_major, version_minor, strings_cut = 0, name_cut = 0;
    long short_len, long_len, long_at;
    const char *stage;
    size_t at = 0;
    if (resource_read(resource, 0, size, whole)) {
        fail(out, cap, "RESOURCE_MAP", "Resource read failed.", 0); return;
    }
    version_major = ((b[0] >> 4) & 15) * 10 + (b[0] & 15);
    version_minor = ((b[1] >> 4) & 15) * 10 + (b[1] & 15);
    stage = b[2] == 0x20 ? "alpha" : b[2] == 0x40 ? "beta" :
            b[2] == 0x60 ? "final" : b[2] == 0x80 ? "release" : "development";
    short_len = b[6];
    if (7 + short_len > size) { fail(out, cap, "MALFORMED", "Version resource string bounds are invalid.", 0); return; }
    if (short_len > 120) { short_len = 120; strings_cut = 1; }
    if (text_to_utf8((char *)b + 7, (size_t)short_len, short_utf, sizeof(short_utf)) < 0) {
        fail(out, cap, "MALFORMED", "Version short string is not MacRoman text.", 0); return;
    }
    long_at = 7 + b[6];
    if (long_at >= size) { fail(out, cap, "MALFORMED", "Version resource long string is missing.", 0); return; }
    long_len = b[long_at];
    if (long_at + 1 + long_len > size) { fail(out, cap, "MALFORMED", "Version resource long string bounds are invalid.", 0); return; }
    if (long_len > 200) { long_len = 200; strings_cut = 1; }
    if (text_to_utf8((char *)b + long_at + 1, (size_t)long_len, long_utf, sizeof(long_utf)) < 0) {
        fail(out, cap, "MALFORMED", "Version long string is not MacRoman text.", 0); return;
    }
    if (json_quote(short_utf, short_q, sizeof(short_q)) < 0 || json_quote(long_utf, long_q, sizeof(long_q)) < 0 ||
        name_json(name, name_q, sizeof(name_q), &name_cut)) {
        fail(out, cap, "LIMIT", "Version strings exceed the result capacity.", 0); return;
    }
    if (append(out, cap, &at, "{\"status\":\"ok\",\"path\":") || quote(out, cap, &at, path) ||
        append(out, cap, &at, ",\"type\":") || quote(out, cap, &at, type_text) ||
        append(out, cap, &at, ",\"name\":") || append(out, cap, &at, name_q)) goto limit;
    {
        char fields[256];
        snprintf(fields, sizeof(fields), ",\"id\":%d,\"resource_bytes\":%ld,\"format\":\"vers\","
            "\"content_bytes\":%ld,\"start_byte\":0,\"next_byte\":%ld,\"truncated\":false,"
            "\"version\":\"%d.%d\",\"stage\":\"%s\",\"prerelease\":%d,\"region\":%d,\"short\":",
            (int)id, size, size, size, version_major, version_minor, stage,
            (int)b[3], (int)((b[4] << 8) | b[5]));
        if (append(out, cap, &at, fields) || append(out, cap, &at, short_q) ||
            append(out, cap, &at, ",\"long\":") || append(out, cap, &at, long_q)) goto limit;
        snprintf(fields, sizeof(fields), ",\"strings_truncated\":%s}", strings_cut ? "true" : "false");
        if (append(out, cap, &at, fields)) goto limit;
    }
    (void)name_cut;
    return;
limit:
    fail(out, cap, "LIMIT", "Version result exceeds the result capacity.", 0);
}
static void read_resource(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap)
{
    static char page[257];
    char path[512], type_text[16], name_out[600];
    FSSpec spec;
    CInfoPBRec before, after;
    OSErr err;
    short ref = -1, previous = 0, id = 0;
    ResType type = 0, actual = 0;
    Handle resource;
    Str255 name;
    unsigned long requested_type;
    long resource_id, start, maximum, size, offset = 0, length = 0, count, i;
    size_t at = 0;
    if (valid_keys(call->arguments, tokens, "|path||type||id||start_byte||max_bytes|") ||
        string_arg(call->arguments, tokens, "path", path, sizeof(path)) < 0 ||
        string_arg(call->arguments, tokens, "type", type_text, sizeof(type_text)) < 0 ||
        number_arg(call->arguments, tokens, "id", 0, -32768, 32767, &resource_id) ||
        number_arg(call->arguments, tokens, "start_byte", 0, 0, 1048576, &start) ||
        number_arg(call->arguments, tokens, "max_bytes", 128, 1, 256, &maximum)) {
        fail(out, cap, "ARGUMENTS", "Expected path, four-character type, integer id, optional start_byte and max_bytes 1-256.", 0); return;
    }
    if (parse_type(type_text, &requested_type)) {
        fail(out, cap, "ARGUMENTS", "type must be four printable characters or 0xXXXXXXXX.", 0); return;
    }
    type = (ResType)requested_type;
    err = tools_resolve(path, &spec);
    if (!err) err = catalog(&spec, &before);
    if (err) { fail(out, cap, "FILE", "Cannot resolve the workspace file.", err); return; }
    if (before.hFileInfo.ioFlAttrib & 16) { fail(out, cap, "FOLDER", "Expected a file, not a folder.", 0); return; }
    if (is_alias(&before)) { fail(out, cap, "ALIAS", "Aliases are not resource files.", 0); return; }
    if (before.hFileInfo.ioFlRLgLen == 0) { fail(out, cap, "NO_RESOURCE_FORK", "This file has no resource fork.", 0); return; }
    previous = CurResFile();
    err = resource_open(&spec, &ref);
    if (err) { fail(out, cap, "RESOURCE_MAP", "Cannot open the resource fork's map.", err); return; }
    resource = Get1Resource(type, (short)resource_id);
    if (!resource) { resource_close(ref, previous); fail(out, cap, "NOT_FOUND", "No resource with that type and id.", 0); return; }
    name[0] = 0;
    GetResInfo(resource, &id, &actual, name);
    size = GetResourceSizeOnDisk(resource);
    if (ResError() || size < 0) { ReleaseResource(resource); resource_close(ref, previous); fail(out, cap, "RESOURCE_MAP", "Resource size is unavailable.", 0); return; }
    type_name(actual, type_text);
    if (actual == 'vers') {
        if (start != 0) {
            ReleaseResource(resource); resource_close(ref, previous);
            fail(out, cap, "RANGE", "Version resources are returned whole; start_byte must be 0.", 0); return;
        }
        if (size < 7 || size > 256) {
            ReleaseResource(resource); resource_close(ref, previous);
            fail(out, cap, "LIMIT", "Version resources larger than 256 bytes or shorter than 7 are refused.", 0); return;
        }
        read_vers(resource, id, name, size, path, type_text, out, cap);
        ReleaseResource(resource);
        resource_close(ref, previous);
        if (strstr(out, "\"status\":\"ok\"")) {
            err = catalog(&spec, &after);
            if (err || !same_file(&before, &after)) fail(out, cap, "CHANGED", "File changed during resource read.", err);
        }
        return;
    }
    if (actual == 'STR ') {
        if (string_bounds(resource, size, &offset, &length)) {
            ReleaseResource(resource); resource_close(ref, previous);
            fail(out, cap, "MALFORMED", "String resource length prefix is invalid.", 0); return;
        }
    } else length = size;
    if (start > length) {
        ReleaseResource(resource); resource_close(ref, previous);
        fail(out, cap, "RANGE", "start_byte is beyond the resource content.", 0); return;
    }
    count = length - start; if (count > maximum) count = maximum;
    if (resource_read(resource, offset + start, count, page)) {
        ReleaseResource(resource); resource_close(ref, previous);
        fail(out, cap, "RESOURCE_MAP", "Resource read failed.", 0); return;
    }
    ReleaseResource(resource);
    resource_close(ref, previous);
    err = catalog(&spec, &after);
    if (err || !same_file(&before, &after)) { fail(out, cap, "CHANGED", "File changed during resource read.", err); return; }
    page[count] = 0;
    {
        int text = (actual == 'TEXT' || actual == 'STR ');
        if (text) for (i = 0; i < count; i++) {
            unsigned char c = (unsigned char)page[i];
            if ((c < 32 && c != 9 && c != 13 && c != 10) || c == 127) text = 0;
        }
        if (name_json(name, name_out, sizeof(name_out), NULL)) {
            fail(out, cap, "LIMIT", "Resource name exceeds the result capacity.", 0); return;
        }
        if (append(out, cap, &at, "{\"status\":\"ok\",\"path\":") || quote(out, cap, &at, path) ||
            append(out, cap, &at, ",\"type\":") || quote(out, cap, &at, type_text)) goto limit;
        {
            char fields[1000];
            snprintf(fields, sizeof(fields), ",\"id\":%d,\"name\":%s,\"resource_bytes\":%ld,\"format\":\"%s\","
                "\"content_bytes\":%ld,\"start_byte\":%ld,\"next_byte\":%ld,\"truncated\":%s",
                (int)id, name_out, size, text ? "text" : "hex", length, start, start + count,
                (start + count < length) ? "true" : "false");
            if (append(out, cap, &at, fields)) goto limit;
        }
        if (text) {
            char utf[1000], quoted[1100];
            if (text_to_utf8(page, (size_t)count, utf, sizeof(utf)) < 0) {
                fail(out, cap, "MALFORMED", "Resource text is not MacRoman.", 0); return;
            }
            if (json_quote(utf, quoted, sizeof(quoted)) < 0) { fail(out, cap, "LIMIT", "Result exceeds the result capacity.", 0); return; }
            if (append(out, cap, &at, ",\"text\":") || append(out, cap, &at, quoted)) goto limit;
        } else {
            static const char digits[] = "0123456789ABCDEF";
            char hex[520];
            size_t pos = 0;
            for (i = 0; i < count; i++) {
                unsigned char c = (unsigned char)page[i];
                hex[pos++] = digits[c >> 4]; hex[pos++] = digits[c & 15];
            }
            hex[pos] = 0;
            if (append(out, cap, &at, ",\"hex\":\"") || append(out, cap, &at, hex) || append(out, cap, &at, "\"")) goto limit;
        }
        if (append(out, cap, &at, "}")) goto limit;
    }
    return;
limit:
    fail(out, cap, "LIMIT", "Resource read exceeds the result capacity.", 0);
}
/* ------------------------------------------------------------------ */
void inspect_execute(const AgentCall *call, char *out, size_t cap)
{
    JsonToken tokens[128];
    if (cap < AGENT_RESULT_CAP) { if (cap) out[0] = 0; return; }
    if (json_parse(call->arguments, strlen(call->arguments), tokens, 128) < 1 || tokens[0].type != JSON_OBJECT) {
        fail(out, cap, "ARGUMENTS", "Tool arguments must be a bounded JSON object.", 0); return;
    }
    if (!strcmp(call->name, "get_file_info")) file_info(call, tokens, out, cap);
    else if (!strcmp(call->name, "resolve_alias")) resolve_alias(call, tokens, out, cap);
    else if (!strcmp(call->name, "list_processes")) list_processes(call, tokens, out, cap);
    else if (!strcmp(call->name, "list_fonts")) list_fonts(call, tokens, out, cap);
    else if (!strcmp(call->name, "measure_text")) measure_text(call, tokens, out, cap);
    else if (!strcmp(call->name, "list_resources")) list_resources(call, tokens, out, cap);
    else if (!strcmp(call->name, "read_resource")) read_resource(call, tokens, out, cap);
    else fail(out, cap, "UNKNOWN_TOOL", "This tool is not installed.", 0);
}
