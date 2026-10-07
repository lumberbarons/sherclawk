/* Native read-only inspection acceptance: Finder identity and formatted
 * dates, alias targets, Process Manager paging, Font Manager families and
 * metrics, and resource map/byte reads against a retained fixture whose
 * resource fork is written through the real Resource Manager. Fixtures stay
 * in Retro68:Sherclawk Inspect <ticks>: for inspection. */
#include "tools.h"
#include "json.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Resources.h>
#include <Aliases.h>
#include <Script.h>
#include <Memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *logfile;
static AgentCall call;
static char result[AGENT_RESULT_CAP];
static int failures;
static int journal(void *ctx, const char *event, const char *json)
{
    (void)ctx;
    return fprintf(logfile, "%s %s\n", event, json) < 0 || fflush(logfile) || FlushVol(NULL, 0);
}
static void execute(void)
{
    if (tools_execute_recorded(&call, result, sizeof(result), journal, NULL)) failures++;
    fprintf(logfile, "%s %s\n", call.name, result); fflush(logfile);
}
static void check(int ok, const char *name)
{
    fprintf(logfile, "%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
    fflush(logfile);
}
static int field(const char *key, char *out, size_t cap)
{
    JsonToken tokens[128];
    return json_parse(result, strlen(result), tokens, 128) > 0 &&
        json_string(result, tokens, json_member(result, tokens, 0, key), out, cap) >= 0;
}
static int int_field(const char *key)
{
    JsonToken tokens[128];
    int i;
    char value[16];
    size_t n;
    if (json_parse(result, strlen(result), tokens, 128) < 1) return -1;
    i = json_member(result, tokens, 0, key);
    if (i < 0 || tokens[i].type != JSON_PRIMITIVE) return -1;
    n = (size_t)(tokens[i].end - tokens[i].start);
    if (n < 1 || n >= sizeof(value)) return -1;
    memcpy(value, result + tokens[i].start, n); value[n] = 0;
    return atoi(value);
}
static OSErr spec_for(const char *relative, int folder, FSSpec *spec)
{
    char full[256];
    Str255 name;
    size_t n;
    (void)folder;
    if (snprintf(full, sizeof(full), "%s%s", SHERCLAWK_WORKSPACE, relative) >= (int)sizeof(full)) return paramErr;
    n = strlen(full);
    if (n > 255) return paramErr;
    name[0] = (unsigned char)n; memcpy(name + 1, full, n);
    return FSMakeFSSpec(0, 0, name, spec);
}
static OSErr folder_create(const char *relative)
{
    FSSpec spec;
    long dir;
    OSErr err = spec_for(relative, 1, &spec);
    return err == fnfErr ? FSpDirCreate(&spec, smSystemScript, &dir) : dupFNErr;
}
static int add_resource(short ref, ResType type, short id, const char *name, const void *data, long size)
{
    Handle handle = NewHandle(size);
    Str255 native;
    if (!handle) return -1;
    HLock(handle);
    memcpy(*handle, data, (size_t)size);
    HUnlock(handle);
    native[0] = 0;
    if (name) { native[0] = (unsigned char)strlen(name); memcpy(native + 1, name, native[0]); }
    UseResFile(ref);
    AddResource(handle, type, id, native);
    if (ResError()) { DisposeHandle(handle); return -1; }
    WriteResource(handle);
    ReleaseResource(handle);
    return ResError() ? -1 : 0;
}
static int make_alias(const char *relative, const FSSpec *target)
{
    FSSpec spec;
    AliasHandle alias = NULL;
    Handle record;
    OSErr err;
    short ref = -1, previous = CurResFile();
    Str255 name = {0};
    if (spec_for(relative, 0, &spec) != fnfErr) return -1;
    err = NewAlias(NULL, target, &alias);
    if (err) return -1;
    record = (Handle)alias;
    /* Use the real Finder alias layout: an empty data fork and alis/0. */
    FSpCreateResFile(&spec, 'MACS', 'alis', smSystemScript);
    err = ResError();
    if (!err) { ref = FSpOpenResFile(&spec, fsWrPerm); err = ResError(); }
    if (!err) {
        UseResFile(ref);
        AddResource(record, 'alis', 0, name);
        err = ResError();
        if (!err) {
            WriteResource(record);
            err = ResError();
            ReleaseResource(record);
            record = NULL;
        }
    }
    UseResFile(previous);
    if (ref >= 0) CloseResFile(ref);
    if (record) DisposeHandle(record);
    if (!err) {
        FInfo info;
        err = FSpGetFInfo(&spec, &info);
        if (!err) { info.fdFlags |= 0x8000; err = FSpSetFInfo(&spec, &info); }
    }
    if (!err) err = FlushVol(NULL, spec.vRefNum);
    return err ? -1 : 0;
}
/* ------------------------------------------------------------------ */
int main(void)
{
    char folder[80], sample_path[160], alias_path[160];
    char alias2_path[180], resource_path[180], nofork_path[180], cursor[64], field_value[800];
    FSSpec sample_spec, folder_spec;
    short ref, previous;
    int i, pages, total, self_seen, front_seen, font_id = -1, chicago_seen = 0;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile = fopen(SHERCLAWK_WORKSPACE "SherclawkInspectCheck.log", "w");
    if (!logfile) return 1;
    snprintf(folder, sizeof(folder), "Sherclawk Inspect %08lx", (unsigned long)TickCount());
    check(!folder_create(folder), "fixture folder");
    snprintf(sample_path, sizeof(sample_path), "%s:Sample.txt", folder);
    snprintf(nofork_path, sizeof(nofork_path), "%s:Plain.c", folder);
    snprintf(alias_path, sizeof(alias_path), "%s:Alias to Sample", folder);
    snprintf(alias2_path, sizeof(alias2_path), "%s:Alias to Folder", folder);
    snprintf(resource_path, sizeof(resource_path), "%s:Resources.bin", folder);
    strcpy(call.id, "native-inspect"); strcpy(call.name, "write_text");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"text\":\"Copper lobster\\rline two\\r\"}", sample_path);
    execute(); check(strstr(result, "CREATED") != NULL, "fixture text written");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"text\":\"plain\\r\"}", nofork_path);
    execute(); check(strstr(result, "CREATED") != NULL, "plain fixture written");

    /* Finder identity, formatted dates and kind/flag reporting. */
    strcpy(call.name, "get_file_info");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", sample_path);
    execute();
    check(strstr(result, "\"kind\":\"file\"") && strstr(result, "\"file_type\":\"TEXT\"") &&
        strstr(result, "\"creator\":\"ttxt\""), "file info type and creator");
    check(strstr(result, "\"alias\":false") && strstr(result, "\"custom_icon\":false") &&
        strstr(result, "\"locked\":false"), "file info flags");
    check(field("modified", field_value, sizeof(field_value)) && strlen(field_value) == 19 &&
        field_value[4] == '-' && field_value[13] == ':', "formatted modification date");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", folder);
    execute(); check(strstr(result, "\"kind\":\"folder\"") != NULL, "folder info");
    strcpy(call.arguments, "{\"path\":\"definitely:missing.c\"}");
    execute(); check(strstr(result, "\"code\":\"FILE\"") != NULL, "missing path refused");

    /* Alias resolution to sibling file and to the fixture folder. */
    check(spec_for(sample_path, 0, &sample_spec) == 0 && spec_for(folder, 1, &folder_spec) == 0, "fixture specs");
    check(!make_alias(alias_path, &sample_spec), "alias file created");
    check(!make_alias(alias2_path, &folder_spec), "folder alias created");
    strcpy(call.name, "resolve_alias");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", alias_path);
    execute();
    check(strstr(result, "\"target_name\":\"Sample.txt\"") && strstr(result, "\"target_kind\":\"file\"") &&
        strstr(result, "\"target_exists\":true"), "alias target identity");
    check(field("relative_path", field_value, sizeof(field_value)) && strstr(field_value, ":Sample.txt") &&
        strstr(result, "\"outside_workspace\":false"), "alias workspace-relative path");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", alias2_path);
    execute();
    check(strstr(result, "\"target_kind\":\"folder\"") && strstr(result, "\"target_name\":\"") &&
        strstr(result, "\"relative_path\":") && !strstr(result, "\"relative_path\":null"), "folder alias resolved");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", sample_path);
    execute(); check(strstr(result, "NOT_ALIAS") != NULL, "non-alias refused");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", sample_path);
    strcpy(call.name, "get_file_info");
    execute(); check(strstr(result, "\"alias\":false") != NULL, "sample still not alias");

    /* Process Manager: our own process must appear, with paging. */
    strcpy(call.name, "list_processes"); strcpy(call.arguments, "{\"limit\":2}");
    pages = total = self_seen = front_seen = 0;
    do {
        execute();
        check(strstr(result, "\"status\":\"error\"") == NULL, "process page");
        total++;
        if (strstr(result, "\"self\":true")) self_seen = 1;
        if (strstr(result, "\"front\":true")) front_seen = 1;
        if (strstr(result, "\"truncated\":false")) break;
        if (!field("next_cursor", cursor, sizeof(cursor))) break;
        snprintf(call.arguments, sizeof(call.arguments), "{\"limit\":2,\"cursor\":\"%s\"}", cursor);
        if (++pages > 40) break;
    } while (1);
    check(self_seen && front_seen && total >= 1, "process listing includes self and front");
    check(!strstr(result, "\"status\":\"error\""), "process paging completed");

    /* Font families: enumerate all, then measure with the first family. */
    strcpy(call.name, "list_fonts"); strcpy(call.arguments, "{\"limit\":24}");
    pages = total = 0;
    do {
        execute();
        total++;
        if (strstr(result, "\"name\":\"Chicago\"")) chicago_seen = 1;
        { JsonToken tokens[256];
          int fonts, first;
          if (json_parse(result, strlen(result), tokens, 256) > 0 &&
              (fonts = json_member(result, tokens, 0, "fonts")) >= 0 &&
              tokens[fonts].next > fonts + 1) {
              first = fonts + 1;
              if (font_id < 0) {
                  int idtok = json_member(result, tokens, first, "id");
                  if (idtok > 0) {
                      char value[16]; size_t n = (size_t)(tokens[idtok].end - tokens[idtok].start);
                      if (n > 0 && n < sizeof(value)) {
                          memcpy(value, result + tokens[idtok].start, n); value[n] = 0;
                          font_id = atoi(value);
                      }
                  }
              }
          } }
        if (strstr(result, "\"truncated\":false")) break;
        if (!field("next_cursor", cursor, sizeof(cursor))) break;
        snprintf(call.arguments, sizeof(call.arguments), "{\"limit\":24,\"cursor\":%s}", cursor);
        if (++pages > 20) break;
    } while (1);
    check(font_id >= 0 && total >= 1, "font families listed");
    check(strstr(result, "\"status\":\"error\"") == NULL, "font paging completed");
    check(chicago_seen, "Chicago family present");
    strcpy(call.name, "measure_text");
    snprintf(call.arguments, sizeof(call.arguments), "{\"text\":\"Hg 12\",\"font_id\":%d,\"size\":12,\"style\":\"plain\"}", font_id);
    execute();
    check(int_field("width") > 0, "measured width positive");
    check(strstr(result, "\"ascent\":") && strstr(result, "\"line_height\":") && strstr(result, "\"font_id\":") &&
        !strstr(result, "\"status\":\"error\""), "measured metrics present");
    snprintf(call.arguments, sizeof(call.arguments), "{\"text\":\"Hg\",\"font_id\":%d,\"style\":\"bold, italic\"}", font_id);
    execute();
    check(strstr(result, "\"style_bits\":3") != NULL, "bold italic style bits");
    strcpy(call.arguments, "{\"text\":\"Hg\",\"font\":\"No Such Font Family\"}");
    execute(); check(strstr(result, "NOT_FOUND") != NULL, "unknown font refused");

    /* Resource fixture written with the real Resource Manager. */
    {
        FSSpec spec;
        const unsigned char str_data[] = {16, 'H','e','l','l','o',',',' ','r','e','s','o','u','r','c','e','!'};
        const unsigned char text_data[] = "line one\rline two";
        const unsigned char test_data[] = {1, 0, 127, 255};
        const unsigned char vers_data[] = {0x01,0x02,0x80,0x00,0x00,0x00,10,'1','.','0','.','2',' ','t','e','s','t',
            16,'b','u','i','l','t',' ','f','o','r',' ','c','h','e','c','k','s'};
        unsigned char blob[300];
        for (i = 0; i < 300; i++) blob[i] = (unsigned char)(i * 7 + 3);
        check(spec_for(resource_path, 0, &spec) == fnfErr, "resource fixture destination new");
        FSpCreateResFile(&spec, 'ShCk', 'TEST', smSystemScript);
        previous = CurResFile();
        ref = FSpOpenResFile(&spec, fsRdWrPerm);
        check(ref >= 0, "resource fixture opened");
        check(!add_resource(ref, 'STR ', 128, "greeting", str_data, sizeof(str_data)) &&
              !add_resource(ref, 'TEXT', 129, "", text_data, sizeof(text_data) - 1) &&
              !add_resource(ref, 'TEST', -2, "", test_data, sizeof(test_data)) &&
              !add_resource(ref, 'BLOB', 300, "long blob", blob, 300) &&
              !add_resource(ref, 'vers', 1, "", vers_data, sizeof(vers_data)), "five resources added");
        UpdateResFile(ref);
        CloseResFile(ref);
        UseResFile(previous);
        check(!FlushVol(NULL, spec.vRefNum), "resource fixture flushed");
    }
    strcpy(call.name, "list_resources");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"limit\":16}", resource_path);
    execute();
    check(strstr(result, "\"type\":\"STR \"") && strstr(result, "\"type\":\"TEXT\"") && strstr(result, "\"type\":\"vers\"") &&
        strstr(result, "\"type\":\"BLOB\""), "resource types listed");
    check(strstr(result, "\"id\":128") && strstr(result, "\"id\":-2") && strstr(result, "\"id\":300") &&
        strstr(result, "\"name\":\"greeting\""), "resource ids and names listed");
    check(int_field("types") == 5 && strstr(result, "\"truncated\":false"), "resource type count");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"limit\":1}", resource_path);
    pages = total = 0;
    do {
        execute();
        total++;
        if (strstr(result, "\"truncated\":false")) break;
        if (!field("next_cursor", cursor, sizeof(cursor))) break;
        snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"limit\":1,\"cursor\":\"%s\"}", resource_path, cursor);
        if (++pages > 10) break;
    } while (1);
    check(total == 5 && !strstr(result, "\"status\":\"error\""), "resource paging reaches five entries");
    strcpy(call.name, "read_resource");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"STR \",\"id\":128}", resource_path);
    execute();
    check(strstr(result, "\"format\":\"text\"") && strstr(result, "\"text\":\"Hello, resource!\"") &&
        strstr(result, "\"content_bytes\":16"), "string resource decoded");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"TEST\",\"id\":-2}", resource_path);
    execute();
    check(strstr(result, "\"format\":\"hex\"") && strstr(result, "\"hex\":\"01007FFF\""), "binary resource hex");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"0x54455354\",\"id\":-2}", resource_path);
    execute();
    check(strstr(result, "\"type\":\"TEST\"") && strstr(result, "\"format\":\"hex\""), "hex resource type accepted");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"TEXT\",\"id\":129}", resource_path);
    execute();
    check(strstr(result, "\"format\":\"text\"") && strstr(result, "line one") && strstr(result, "line two"), "text resource decoded");
    snprintf(call.arguments, sizeof(call.arguments),
        "{\"path\":\"%s\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":0,\"max_bytes\":256}", resource_path);
    execute();
    check(strstr(result, "\"truncated\":true") && strstr(result, "\"next_byte\":256"), "large resource first page");
    snprintf(call.arguments, sizeof(call.arguments),
        "{\"path\":\"%s\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":256,\"max_bytes\":256}", resource_path);
    execute();
    check(strstr(result, "\"truncated\":false") && strstr(result, "\"next_byte\":300"), "large resource continuation");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"vers\",\"id\":1}", resource_path);
    execute();
    check(strstr(result, "\"format\":\"vers\"") && strstr(result, "\"version\":\"1.0.2\"") &&
        strstr(result, "\"stage\":\"release\"") && strstr(result, "\"short\":\"1.0.2 test\"") &&
        strstr(result, "\"long\":\"built for checks\""), "version resource decoded");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"STR \",\"id\":999}", resource_path);
    execute(); check(strstr(result, "NOT_FOUND") != NULL, "missing resource refused");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"STR \",\"id\":128}", nofork_path);
    execute(); check(strstr(result, "NO_RESOURCE_FORK") != NULL, "no-fork file refused");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\",\"type\":\"TOOLONG\",\"id\":1}", resource_path);
    execute(); check(strstr(result, "ARGUMENTS") != NULL, "malformed type refused");
    snprintf(call.arguments, sizeof(call.arguments),
        "{\"path\":\"%s\",\"type\":\"BLOB\",\"id\":300,\"start_byte\":301}", resource_path);
    execute(); check(strstr(result, "RANGE") != NULL, "byte range refused");
    strcpy(call.name, "get_file_info");
    snprintf(call.arguments, sizeof(call.arguments), "{\"path\":\"%s\"}", resource_path);
    execute();
    check(int_field("resource_bytes") > 0 && strstr(result, "\"file_type\":\"TEST\""), "resource fixture fork reported");

    fprintf(logfile, "RESULT failures=%d\n", failures);
    fclose(logfile);
    return failures ? 1 : 0;
}
