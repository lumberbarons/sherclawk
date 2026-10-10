/* Guest acceptance for the MCP configuration guard (#160). The workspace is
 * pointed at the boot volume root so it contains the real Preferences folder,
 * the case the guard exists for. Every hostile call runs with no journal, so a
 * tool that wrongly got past the guard still stops before it can mutate. The
 * log carries only fixed milestones, status, code, os_error and identifiers:
 * never a result body, because the file under test holds credentials. If the
 * configuration is absent a placeholder is created and removed afterwards. */
#include "tools.h"
#include "mcp_guard.h"
#include "mcp_store.h"
#include "json.h"
#include "config.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Files.h>
#include <Folders.h>
#include <Resources.h>
#include <Aliases.h>
#include <Script.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diagnostic-tools.h"

#define CONFIG_NAME "Sherclawk MCP Servers.json"
#define PLACEHOLDER "{\"mcpServers\":{}} GUARD-PLACEHOLDER"

static FILE *logfile;
static int failures;
static AgentCall call;
static char result[AGENT_RESULT_CAP];
static char workspace[40], prefs_rel[200];
/* What the last call reported, parsed from its result and nothing else. */
static char last_status[16], last_code[32];
static int last_os;

static void check(int ok, const char *name)
{
    fprintf(logfile, "%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
    fflush(logfile);
}
static int journal(void *ctx, const char *event, const char *json)
{
    (void)ctx; (void)json;
    fprintf(logfile, "journal %s\n", event);
    return fflush(logfile) || FlushVol(NULL, 0);
}
/* Run one tool call and keep only its envelope. */
static void run(const char *name, const char *arguments, AgentJournal record)
{
    JsonToken tokens[128];
    int i;
    char value[24];
    strcpy(call.id, "guard"); strcpy(call.name, name); strcpy(call.arguments, arguments);
    tools_execute_recorded(&call, result, sizeof(result), record, NULL);
    last_status[0] = last_code[0] = 0; last_os = 0;
    if (json_parse(result, strlen(result), tokens, 128) > 0) {
        json_string(result, tokens, json_member(result, tokens, 0, "status"), last_status, sizeof(last_status));
        i = json_member(result, tokens, 0, "code");
        if (i >= 0) json_string(result, tokens, i, last_code, sizeof(last_code));
        i = json_member(result, tokens, 0, "os_error");
        if (i >= 0 && tokens[i].type == JSON_PRIMITIVE) {
            size_t n = (size_t)(tokens[i].end - tokens[i].start);
            if (n && n < sizeof(value)) { memcpy(value, result + tokens[i].start, n); value[n] = 0; last_os = atoi(value); }
        }
    }
    fprintf(logfile, "call %s status=%s code=%s os_error=%d\n", name, last_status, last_code, last_os);
    fflush(logfile);
}
static int refused(void)
{
    return !strcmp(last_status, "error") && !strcmp(last_code, "PROTECTED") && last_os == MCP_GUARD_DENIED;
}
static int contains(const char *needle) { return strstr(result, needle) != NULL; }

static OSErr spec_at(const char *relative, FSSpec *spec)
{
    char full[256];
    Str255 name;
    size_t n;
    if (snprintf(full, sizeof(full), "%s%s", workspace, relative) >= (int)sizeof(full)) return paramErr;
    n = strlen(full);
    name[0] = (unsigned char)n; memcpy(name + 1, full, n);
    return FSMakeFSSpec(0, 0, name, spec);
}
/* Volume name and Preferences' path below it, read by walking up the catalog. */
static OSErr locate(short vref, long dir)
{
    char names[8][32], path[200] = "";
    int depth = 0, k;
    for (;;) {
        FSSpec spec;
        OSErr err = FSMakeFSSpec(vref, dir, (ConstStr255Param)"\p", &spec);
        if (err) return err;
        if (depth >= 8 || spec.name[0] > 31) return paramErr;
        memcpy(names[depth], spec.name + 1, spec.name[0]); names[depth][spec.name[0]] = 0;
        depth++;
        if (spec.parID == 1) break; /* the root's parent: this was the volume */
        dir = spec.parID;
    }
    snprintf(workspace, sizeof(workspace), "%.31s:", names[depth - 1]);
    for (k = depth - 2; k >= 0; k--) {
        if (strlen(path) + strlen(names[k]) + 2 >= sizeof(path)) return paramErr;
        strcat(path, names[k]); strcat(path, ":");
    }
    snprintf(prefs_rel, sizeof(prefs_rel), "%s", path);
    return noErr;
}
static OSErr make_alias(const char *relative, const FSSpec *target)
{
    FSSpec spec;
    AliasHandle alias = NULL;
    OSErr err;
    short ref = -1, previous = CurResFile();
    Str255 name = {0};
    if (spec_at(relative, &spec) != fnfErr) return paramErr;
    err = NewAlias(NULL, target, &alias);
    if (err) return err;
    FSpCreateResFile(&spec, 'MACS', 'alis', smSystemScript);
    err = ResError();
    if (!err) { ref = FSpOpenResFile(&spec, fsWrPerm); err = ResError(); }
    if (!err) {
        UseResFile(ref);
        AddResource((Handle)alias, 'alis', 0, name);
        err = ResError();
        if (!err) { WriteResource((Handle)alias); err = ResError(); ReleaseResource((Handle)alias); alias = NULL; }
    }
    UseResFile(previous);
    if (ref >= 0) CloseResFile(ref);
    if (alias) DisposeHandle((Handle)alias);
    if (!err) {
        FInfo info;
        err = FSpGetFInfo(&spec, &info);
        if (!err) { info.fdFlags |= 0x8000; err = FSpSetFInfo(&spec, &info); }
    }
    if (!err) err = FlushVol(NULL, spec.vRefNum);
    return err;
}
static OSErr create_placeholder(const FSSpec *spec)
{
    short ref;
    long count = (long)strlen(PLACEHOLDER);
    OSErr err = FSpCreate(spec, MCP_STORE_CREATOR, MCP_STORE_TYPE, smSystemScript), closed;
    if (!err) err = FSpOpenDF(spec, fsWrPerm, &ref);
    if (!err) { err = FSWrite(ref, &count, PLACEHOLDER); closed = FSClose(ref); if (!err) err = closed; }
    if (!err) err = FlushVol(NULL, spec->vRefNum);
    return err;
}
static OSErr catalog(const FSSpec *spec, long *size, unsigned long *modified)
{
    CInfoPBRec pb;
    Str255 name;
    OSErr err;
    memset(&pb, 0, sizeof(pb));
    memcpy(name, spec->name, (size_t)spec->name[0] + 1);
    pb.hFileInfo.ioNamePtr = name; pb.hFileInfo.ioVRefNum = spec->vRefNum; pb.hFileInfo.ioDirID = spec->parID;
    err = PBGetCatInfoSync(&pb);
    if (!err) { *size = pb.hFileInfo.ioFlLgLen; *modified = pb.hFileInfo.ioFlMdDat; }
    return err;
}
/* Every guarded tool against one protected path. */
static void hostile(const char *path, const char *label)
{
    char arguments[512], title[96];
    struct { const char *tool, *format; } calls[] = {
        { "read_text", "{\"path\":\"%s\"}" },
        { "get_file_info", "{\"path\":\"%s\"}" },
        { "list_resources", "{\"path\":\"%s\"}" },
        { "read_resource", "{\"path\":\"%s\",\"type\":\"STR \",\"id\":128}" },
        { "resolve_alias", "{\"path\":\"%s\"}" },
        { "write_text", "{\"path\":\"%s\",\"text\":\"{}\"}" },
        { "create_folder", "{\"path\":\"%s\"}" },
        { "create_project", "{\"path\":\"%s\"}" },
        { "edit_text", "{\"path\":\"%s\",\"expected_revision\":\"full-00000000-00000000-00000000-00000000\",\"old_text\":\"a\",\"new_text\":\"b\"}" },
        { "move_to_trash", "{\"files\":[{\"path\":\"%s\",\"revision\":\"cat-00000000-00000000-00000000-00000000\"}]}" },
    };
    size_t k;
    for (k = 0; k < sizeof(calls) / sizeof(calls[0]); k++) {
        snprintf(arguments, sizeof(arguments), calls[k].format, path);
        /* move_to_trash wants a journal before it validates anything; its pinned
         * revision cannot match, so it still refuses before any record or move. */
        run(calls[k].tool, arguments, !strcmp(calls[k].tool, "move_to_trash") ? journal : NULL);
        snprintf(title, sizeof(title), "%s refuses %s", calls[k].tool, label);
        check(refused(), title);
    }
}
int main(void)
{
    char path[300], arguments[512], fixture[64], alias_path[320], note_path[300], cursor[16];
    FSSpec config, fixture_spec, note_spec, alias_spec;
    short vref = 0;
    long dir = 0, size_before = 0, size_after = 0, fixture_dir;
    unsigned long modified_before = 0, modified_after = 0;
    int pages, saw_note, saw_config, created_placeholder = 0, skipped_seen = 0;
    OSErr err;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    logfile = fopen(SHERCLAWK_WORKSPACE "SherclawkMCPGuardCheck.log", "w");
    if (!logfile) return 1;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vref, &dir);
    fprintf(logfile, "NOTE FindFolder err=%d vref=%d dir=%ld\n", (int)err, (int)vref, dir);
    check(!err && dir, "FindFolder locates Preferences");
    err = locate(vref, dir);
    check(!err, "Preferences path read from the catalog");
    if (err) { fprintf(logfile, "RESULT failures=%d\n", failures); fclose(logfile); return 1; }
    tools_set_workspace(workspace);
    fprintf(logfile, "NOTE workspace=%s preferences=%s\n", workspace, prefs_rel);

    snprintf(path, sizeof(path), "%s%s", prefs_rel, CONFIG_NAME);
    err = spec_at(path, &config);
    if (err == fnfErr) { check(!create_placeholder(&config), "placeholder configuration created"); created_placeholder = 1; err = spec_at(path, &config); }
    check(!err, "configuration present");
    /* The assumption the host cannot prove: the spec the tools build and the
     * folder FindFolder reports name the same volume and directory. */
    fprintf(logfile, "NOTE spec vref=%d parID=%ld\n", (int)config.vRefNum, config.parID);
    check(config.vRefNum == vref && config.parID == dir, "tool specs and FindFolder agree on volume and directory");
    check(mcp_guard_check(&config) == MCP_GUARD_DENIED, "guard recognises the configuration spec");
    check(!catalog(&config, &size_before, &modified_before), "configuration catalog read");

    {
        const char *names[] = { CONFIG_NAME, CONFIG_NAME ".new", CONFIG_NAME ".old", "SHERCLAWK MCP SERVERS.JSON", "sherclawk mcp servers.json.OLD" };
        size_t k;
        for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            snprintf(path, sizeof(path), "%s%s", prefs_rel, names[k]);
            hostile(path, names[k]);
        }
    }

    /* Listing and search over the real folder. */
    snprintf(fixture, sizeof(fixture), "Sherclawk Guard %08lx", (unsigned long)TickCount());
    snprintf(note_path, sizeof(note_path), "%s%s.txt", prefs_rel, fixture);
    snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\",\"text\":\"guard control\\r\"}", note_path);
    run("write_text", arguments, journal);
    check(!strcmp(last_code, "CREATED"), "ordinary file created in the Preferences folder");
    snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", note_path);
    run("read_text", arguments, NULL);
    check(!strcmp(last_status, "ok") && contains("guard control"), "ordinary file in the Preferences folder reads");
    run("get_file_info", arguments, NULL);
    check(!strcmp(last_status, "ok"), "ordinary file in the Preferences folder has info");

    saw_note = saw_config = 0; strcpy(cursor, "0");
    for (pages = 0; pages < 40; pages++) {
        char *at;
        snprintf(arguments, sizeof(arguments), "{\"root\":\"%.*s\",\"limit\":12,\"cursor\":%s}",
            (int)strlen(prefs_rel) - 1, prefs_rel, cursor);
        run("list_files", arguments, NULL);
        if (strcmp(last_status, "ok")) break;
        if (contains("MCP Servers")) saw_config = 1;
        if (contains(fixture)) saw_note = 1;
        if (contains("\"truncated\":false")) break;
        at = strstr(result, "\"next_cursor\":");
        if (!at) break;
        snprintf(cursor, sizeof(cursor), "%ld", strtol(at + 14, NULL, 10));
    }
    check(pages < 40 && saw_note && !saw_config, "list_files pages past the configuration and shows the rest");

    saw_config = 0; cursor[0] = 0;
    for (pages = 0; pages < 60; pages++) {
        char *at, *s;
        if (cursor[0]) snprintf(arguments, sizeof(arguments), "{\"root\":\"%.*s\",\"query\":\"mcpServers\",\"cursor\":\"%s\"}",
            (int)strlen(prefs_rel) - 1, prefs_rel, cursor);
        else snprintf(arguments, sizeof(arguments), "{\"root\":\"%.*s\",\"query\":\"mcpServers\"}", (int)strlen(prefs_rel) - 1, prefs_rel);
        run("search_text", arguments, NULL);
        if (strcmp(last_status, "ok")) break;
        if (contains("MCP Servers")) saw_config = 1;
        s = strstr(result, "\"skipped\":");
        if (s && atoi(s + 10) > 0) skipped_seen = 1;
        if (contains("\"truncated\":false")) break;
        at = strstr(result, "\"next_cursor\":\"");
        if (!at) break;
        { size_t n = strcspn(at + 15, "\""); if (n >= sizeof(cursor)) break; memcpy(cursor, at + 15, n); cursor[n] = 0; }
    }
    check(pages < 60 && !saw_config && skipped_seen, "search_text skips the configuration without reading it");

    /* An alias to the configuration is resolved by identity and refused. */
    snprintf(path, sizeof(path), "%s", fixture);
    err = spec_at(path, &fixture_spec);
    check(err == fnfErr && !FSpDirCreate(&fixture_spec, smSystemScript, &fixture_dir), "alias fixture folder created");
    snprintf(alias_path, sizeof(alias_path), "%s:Alias to configuration", fixture);
    check(!make_alias(alias_path, &config), "alias to the configuration created");
    snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", alias_path);
    run("resolve_alias", arguments, NULL);
    check(refused() && !contains("Servers"), "resolve_alias refuses a configuration target");
    snprintf(path, sizeof(path), "%s:Alias to fixture", fixture);
    check(!make_alias(path, &fixture_spec), "control alias created");
    snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}", path);
    run("resolve_alias", arguments, NULL);
    check(!strcmp(last_status, "ok"), "resolve_alias still resolves an ordinary target");

    /* Cleanup of what this run created, then proof the configuration is untouched. */
    spec_at(path, &alias_spec); FSpDelete(&alias_spec);
    spec_at(alias_path, &alias_spec); FSpDelete(&alias_spec);
    FSpDelete(&fixture_spec);
    if (!spec_at(note_path, &note_spec)) FSpDelete(&note_spec);
    if (created_placeholder) FSpDelete(&config);
    else {
        check(!catalog(&config, &size_after, &modified_after) && size_after == size_before && modified_after == modified_before,
            "configuration size and modification date unchanged");
    }
    FlushVol(NULL, vref);
    fprintf(logfile, "RESULT failures=%d\n", failures);
    fclose(logfile);
    return failures ? 1 : 0;
}
