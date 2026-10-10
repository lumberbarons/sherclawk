/* Phase-one native diagnostic. Reads the user's Preferences configuration;
 * never logs its bytes, tokens, server descriptions or response bodies.
 * Logs only fixed milestones/counts to Retro68:SherclawkMCPCheck.log. */
#include "mcp_client.h"
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Folders.h>
#include <Files.h>
#include <OpenTransport.h>
#include <stdio.h>
#include <string.h>
static McpClient client;
static McpConfig config;
static char bytes[MCP_CONFIG_CAP + 1], error[256];
static FILE *logfile;
static int load(void)
{
    FSSpec spec;
    short volume, ref = 0;
    long directory, length = 0, count;
    OSErr err, close_err;
    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &volume, &directory);
    if (!err) err = FSMakeFSSpec(volume, directory, (ConstStr255Param)"\pSherclawk MCP Servers.json", &spec);
    if (!err) err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (!err) err = GetEOF(ref, &length);
    if (!err && (length < 0 || length > MCP_CONFIG_CAP)) err = ioErr;
    if (!err) {
        count = length; err = FSRead(ref, &count, bytes);
        if (!err && count != length) err = ioErr;
    }
    if (ref) { close_err = FSClose(ref); if (!err) err = close_err; }
    if (err) {
        fputs("FAIL configuration missing, unreadable or oversized\n", logfile);
        memset(bytes, 0, sizeof(bytes)); return -1;
    }
    if (mcp_config_parse(bytes, (size_t)length, &config, error, sizeof(error)) < 0) {
        /* Parser errors contain field names only. */
        fprintf(logfile, "FAIL %s\n", error); memset(bytes, 0, sizeof(bytes)); return -1;
    }
    memset(bytes, 0, sizeof(bytes));
    if (!config.enabled || !config.official_tavily) {
        fputs("FAIL configure the official Tavily MCP endpoint for this diagnostic\n", logfile); return -1;
    }
    fputs("PASS configuration\n", logfile); return 0;
}
static int wait_client(void)
{
    EventRecord event;
    int r, stopped = 0;
    unsigned long stop_ticks = 0;
    long stop_id = 0;
    while (!(r = mcp_client_step(&client, TickCount()))) {
        WaitNextEvent(everyEvent, &event, 1, NULL);
        if (!stopped && event.what == keyDown && (event.message & charCodeMask) == 27) {
            MacTLS_State state = client.exchange.ctx ? MacTLS_GetState(client.exchange.ctx) : kMacTLS_Closed;
            stopped = 1; stop_ticks = TickCount(); stop_id = client.id;
            fprintf(logfile, "STOP Escape phase=%d tls_state=%d pending_connect=%d request_id=%ld\n",
                client.state, (int)state, state == kMacTLS_Idle || state == kMacTLS_Connecting, stop_id);
            fflush(logfile);
            mcp_client_stop(&client, stop_ticks);
        }
    }
    if (stopped && client.state == MCP_STOPPED)
        fprintf(logfile, "PASS Stop drained ticks=%lu contexts=%d request_id_unchanged=%d (server cancellation unproven)\n",
            (unsigned long)(TickCount() - stop_ticks),
            (client.exchange.ctx != NULL) + (client.auxiliary.ctx != NULL), client.id == stop_id);
    if (r < 0) {
        fprintf(logfile, "FAIL %s\n", client.state == MCP_STOPPED ? "stopped (cancellation best effort; no replay)" : client.error);
    }
    fflush(logfile); return r;
}
int main(void)
{
    int i, found = -1, result;
    unsigned long start;
    EventRecord event;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL);
    logfile = fopen("Retro68:SherclawkMCPCheck.log", "w");
    if (!logfile) return 1;
    if (load() < 0) { fclose(logfile); return 1; }
    fflush(logfile);
    InitOpenTransport();
    if (MacTLS_Init() != kMacTLS_OK) {
        fputs("FAIL TLS library initialization\n", logfile);
        CloseOpenTransport(); fclose(logfile); return 1;
    }
    result = mcp_client_discover(&client, &config, TickCount());
    memset(&config, 0, sizeof(config));
    if (result >= 0) result = wait_client();
    if (result > 0) {
        fprintf(logfile, "PASS TLS initialize version=%s pages=%d entries=%d eligible=%d\n",
            client.version, client.registry.pages, client.registry.entries, client.registry.count);
        fflush(logfile);
        for (i = 0; i < client.registry.count; i++)
            if (!strcmp(client.registry.tools[i].original, "tavily_search")) found = i;
        if (found < 0) { fputs("FAIL select tavily_search in the configuration\n", logfile); result = -1; }
        else {
            result = mcp_client_call(&client, client.registry.tools[found].name,
                "{\"query\":\"Model Context Protocol official documentation\",\"max_results\":5,\"search_depth\":\"basic\",\"include_images\":false,\"include_raw_content\":false}", TickCount());
            if (result >= 0) result = wait_client();
            if (result > 0) {
                static JsonToken tokens[8192];
                int r, content, text;
                r = json_parse(client.response, strlen(client.response), tokens, 8192);
                r = r > 0 ? json_member(client.response, tokens, 0, "result") : -1;
                content = r >= 0 ? json_member(client.response, tokens, r, "content") : -1;
                if (client.call_error || content < 0 || tokens[content].type != JSON_ARRAY || tokens[content].next == content + 1) {
                    fputs("FAIL search error or missing content\n", logfile); result = -1;
                } else {
                    /* Inspect only structure/known markers; never log server text.
                     * Reuse the closed exchange's request buffer as decoded scratch. */
                    text = json_member(client.response, tokens, content + 1, "text");
                    if (text < 0 || json_string(client.response, tokens, text,
                            client.exchange.request, sizeof(client.exchange.request)) < 0) {
                        fputs("FAIL search text missing or oversized\n", logfile); result = -1;
                    } else {
                        char *body = client.exchange.request;
                        int results, count = 0, entry, url;
                        r = json_parse(body, strlen(body), tokens, 8192);
                        results = r > 0 ? json_member(body, tokens, 0, "results") : -1;
                        if (results >= 0 && tokens[results].type == JSON_ARRAY) {
                            for (entry = results + 1; entry < tokens[results].next; entry = tokens[entry].next) {
                                url = json_member(body, tokens, entry, "url");
                                if (url >= 0 && json_string(body, tokens, url, bytes, sizeof(bytes)) > 0 &&
                                    (!strncmp(bytes, "https://", 8) || !strncmp(bytes, "http://", 7)))
                                    count++;
                                memset(bytes, 0, sizeof(bytes));
                            }
                        } else if (strstr(body, "Detailed Results:")) {
                            char *at = body;
                            while ((at = strstr(at, "\nURL: "))) {
                                at += 6;
                                if (!strncmp(at, "https://", 8) || !strncmp(at, "http://", 7)) count++;
                            }
                        }
                        if (!count) {
                            fputs("FAIL search returned no result URLs (body deliberately omitted)\n", logfile); result = -1;
                        } else fprintf(logfile, "PASS Tavily search results=%d bytes=%lu (body deliberately omitted)\n",
                            count, (unsigned long)strlen(client.response));
                        memset(body, 0, sizeof(client.exchange.request));
                    }
                }
            }
        }
    } else if (client.error[0]) fprintf(logfile, "FAIL %s\n", client.error);
    mcp_client_close(&client);
    memset(client.response, 0, sizeof(client.response));
    /* Yield before global OT shutdown, as the existing native probe does. */
    start = TickCount();
    while ((unsigned long)(TickCount() - start) < 60) WaitNextEvent(everyEvent, &event, 1, NULL);
    MacTLS_Shutdown(); CloseOpenTransport();
    fprintf(logfile, "END %s\n", result > 0 ? "PASS" : "FAIL"); fclose(logfile);
    return result > 0 ? 0 : 1;
}
