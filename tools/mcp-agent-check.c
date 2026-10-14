/* #165: real model/MCP acceptance with the production event-loop paths.
 * Reads the owner's private, editor-saved configuration; never logs bodies,
 * schemas, headers or session IDs. Run only in a reserved OS 9 guest.
 * No production hooks: include the controller to observe parsing and drain. */
#define main SherclawkMain
#include "app/main.c"
#undef main
#include "mcp/run.c"
#include <LowMem.h>
static FILE *accept_log;
static int failures, mode, opens, search_calls, extract_calls, pages;
static int discovery_count, retained, links, notices;
static McpConfig private_config;
static char private_session[1024];
static char artifacts[MCP_RESULT_MAX][344];
static int artifact_count, schemas_checked;
static char journals[16][256];
static int journal_count, search_ok, extract_ok;
static char page_ids[64][64];
static long page_offsets[64];
static int distinct_pages;
static long draining_id;
static int drain_advanced;
static unsigned long worst_step;
static long least_heap = 0x7fffffffL, least_stack = 0x7fffffffL;

static void Check(const char *label, int ok)
{
    fprintf(accept_log, "%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
    fflush(accept_log);
}
static long Heap(void)
{
    Ptr end = LMGetHeapEnd(), limit = GetApplLimit();
    return (long)FreeMem() + (limit > end ? (long)(limit - end) : 0);
}
static void Memory(const char *label)
{
    long heap = Heap(), stack = StackSpace();
    if (heap < least_heap) least_heap = heap;
    if (stack < least_stack) least_stack = stack;
    fprintf(accept_log, "MEM %s heap=%ld stack=%ld model=%d primary=%d auxiliary=%d drain=%d\n",
        label, heap, stack, gNet.ctx != NULL, client.exchange.ctx != NULL,
        client.auxiliary.ctx != NULL, mcp_run_draining());
    fflush(accept_log);
}
/* Independent exact-value oracle; exhaustive JSON spellings are host-tested.
 * Never print the offending value or content on failure. */
static int Clean(const char *text)
{
    size_t i;
    if (*gPrefs.api_key && strstr(text, gPrefs.api_key)) return 0;
    if (*private_session && strstr(text, private_session)) return 0;
    for (i = 0; i < private_config.secrets_len; ) {
        const char *value = private_config.secrets + i;
        const char *token = strchr(value, ' ');
        if (*value && strstr(text, value)) return 0;
        if (token && token[1] && strstr(text, token + 1)) return 0;
        i += strlen(value) + 1;
    }
    return 1;
}
static McpRunLoad Load(char *bytes, size_t cap, size_t *len)
{
    if (mode) {
        const char *fixture = mode == 1 ? "{\"mcpServers\":{}}" :
            "{\"mcpServers\":{\"failed\":{\"url\":\"https://mcp.tavily.com/acceptance-missing\"}}}";
        if (strlen(fixture) >= cap) return MCP_RUN_LOAD_UNREADABLE;
        strcpy(bytes, fixture); *len = strlen(bytes); return MCP_RUN_LOAD_OK;
    }
    return McpHostLoad(bytes, cap, len);
}
static void Open(void) { opens++; McpHostOpenTransport(); }
static int Journal(void *context, const char *event, const char *json)
{
    if (!Clean(json)) Check("journal credential oracle", 0);
    if (!strcmp(event, "mcp_discovery")) discovery_count++;
    if (!strcmp(event, "mcp_result_retained")) {
        JsonToken tokens[32]; char id[64];
        retained++;
        if (artifact_count < MCP_RESULT_MAX && json_parse(json, strlen(json), tokens, 32) > 0 &&
            json_string(json, tokens, json_member(json, tokens, 0, "result_id"), id, sizeof(id)) >= 0) {
            snprintf(artifacts[artifact_count++], sizeof(artifacts[0]), "%s%s:%s",
                tools_workspace(), MCP_RESULT_FOLDER, id);
        } else Check("retained artifact identity captured", 0);
    }
    if (!strcmp(event, "mcp_notice")) notices++;
    return ToolEventJournal(context, event, json);
}
static const McpRunHost acceptance_host = {
    Load, tools_workspace, Open, McpHostTicks, Journal, &gSession
};
static void Pump(void)
{
    EventRecord event;
    unsigned long start;
    int next = gAgent.next;
    static char executed[128], arguments[AGENT_ARGUMENT_CAP];
    executed[0] = arguments[0] = 0;
    WaitNextEvent(everyEvent, &event, RunBusy() ? 1 : 0, NULL);
    SetPort(gWindow); HandleEvent(&event);
    if (*client.session) strcpy(private_session, client.session);
    if (draining_id && client.id && client.id != draining_id) drain_advanced = 1;
    if (!schemas_checked && mcp_run_admitted()) {
        Check("frozen schema credential oracle", Clean(client.registry.schemas)); schemas_checked = 1;
    }
    if (gRun == RUN_TOOLS && gAgent.next < gAgent.count) {
        const char *name = gAgent.calls[gAgent.next].name;
        if (!strcmp(name, "mcp_tavily_tavily_search")) search_calls++;
        if (!strcmp(name, "mcp_tavily_tavily_extract")) extract_calls++;
        if (!strcmp(name, "read_mcp_result")) pages++;
    }
    if ((gRun == RUN_TOOLS || gRun == RUN_MCP) && next < gAgent.count) {
        strcpy(executed, gAgent.calls[next].name); strcpy(arguments, gAgent.calls[next].arguments);
    }
    start = TickCount();
    if (RunBusy()) DriveChatStep();
    if (*executed && gAgent.next == next + 1 && strstr(gToolResult, "\"status\":\"ok\"")) {
        if (!strcmp(executed, "mcp_tavily_tavily_search") && strstr(gToolResult, "\"is_error\":false")) search_ok++;
        if (!strcmp(executed, "mcp_tavily_tavily_extract") && strstr(gToolResult, "\"is_error\":false")) extract_ok++;
        if (!strcmp(executed, "read_mcp_result")) {
            JsonToken t[32]; char id[64]; long offset = 0; int i;
            if (json_parse(arguments, strlen(arguments), t, 32) > 0 &&
                json_string(arguments, t, json_member(arguments, t, 0, "result_id"), id, sizeof(id)) >= 0) {
                int at = json_member(arguments, t, 0, "start_byte");
                if (at >= 0) (void)json_integer(arguments, t, at, &offset);
                for (i = 0; i < distinct_pages; i++)
                    if (offset == page_offsets[i] && !strcmp(id, page_ids[i])) break;
                if (i == distinct_pages && distinct_pages < 64) {
                    strcpy(page_ids[i], id); page_offsets[i] = offset; distinct_pages++;
                }
            }
        }
    }
    if ((unsigned long)TickCount() - start > worst_step) worst_step = TickCount() - start;
    DrainAbandonedExchange(); DrainMcpClient();
    if (Heap() < least_heap) Memory("low");
    if (StackSpace() < least_stack) least_stack = StackSpace();
}
static int Settle(unsigned long seconds)
{
    unsigned long start = TickCount();
    int timed_out;
    while (!gQuit && (RunBusy() || mcp_run_drain_pending() || gExchangeDrain) &&
           (unsigned long)TickCount() - start < seconds * 60) Pump();
    timed_out = RunBusy() || mcp_run_drain_pending() || gExchangeDrain;
    if (RunBusy()) AbortChat("Acceptance deadline; no replay.");
    return !timed_out && !gQuit;
}
static void Begin(const char *prompt)
{
    NewChat();
    strcpy(gRunModel, gPrefs.model); strcpy(gPending, prompt);
    SendBegin();
    if (gSession.open && journal_count < 16) strcpy(journals[journal_count++], gSession.path);
}
static void ScanFile(const char *path)
{
    static char scan[8193];
    FILE *f = fopen(path, "r");
    size_t kept = 0, n;
    int clean = f != NULL;
    if (f) {
        while ((n = fread(scan + kept, 1, 4096, f)) != 0) {
            scan[kept + n] = 0;
            if (!Clean(scan)) clean = 0;
            kept += n;
            if (kept > 4096) { memmove(scan, scan + kept - 4096, 4096); kept = 4096; }
        }
        if (ferror(f)) clean = 0;
        fclose(f);
    }
    Check("persisted file credential oracle", clean);
    memset(scan, 0, sizeof(scan));
}
static void StopAt(int target)
{
    unsigned long started = TickCount(), stopped, elapsed;
    long request;
    int hit = 0;
    mode = 0;
    Begin("Call mcp_tavily_tavily_search for classic Mac OS memory management, max_results 2. Read three retained pages using read_mcp_result, then finish.");
    while (!gQuit && RunBusy() && (unsigned long)TickCount() - started < 180 * 60) {
        hit = target == 0 ? phase == MCP_RUN_DISCOVERING && client.exchange.ctx != NULL :
              target == 1 ? phase == MCP_RUN_CALLING && client.exchange.ctx != NULL :
              target == 2 ? phase == MCP_RUN_CALLING && client.working :
              target == 3 ? phase == MCP_RUN_RETAINING :
              gRun == RUN_TOOLS && gAgent.next < gAgent.count &&
                  !strcmp(gAgent.calls[gAgent.next].name, "read_mcp_result");
        if (hit) break;
        Pump();
    }
    Check("Stop target reached", hit);
    request = client.id; stopped = TickCount();
    AbortChat("Acceptance Stop; outcome unknown; never replay.");
    elapsed = (unsigned long)TickCount() - stopped;
    fprintf(accept_log, "STOP target=%d abort_ticks=%lu working=%d drain=%d\n",
        target, elapsed, client.working, mcp_run_draining());
    Check("Stop returns within one second", elapsed < 60);
    Check("Stop clears advertised remote names", !mcp_run_admitted());
    draining_id = request; drain_advanced = 0;
    if (target == 0 && mcp_run_draining()) {
        /* Allocation stress only: hold the actual model network context
         * beside MCP cleanup, without pumping/sending a model request. */
        Check("model TLS allocation during MCP drain", !network_start(&gNet, "", 0));
        Memory("model-context-with-MCP-drain");
    }
    Check("Stop settles", Settle(45));
    if (gNet.ctx) CloseChatContext();
    Check("drain does not advance RPC ID", !drain_advanced && (!client.id || client.id == request));
    draining_id = 0;
    Check("stopped history credential oracle", Clean(gAgent.history));
}
int main(void)
{
    static char config_bytes[MCP_CONFIG_CAP + 1];
    char error[256];
    size_t length = 0;
    int i, before;
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Handle size_resource;
    unsigned long preferred = 0, minimum = 1;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor(); MacTLS_Init();
    PrefsLoad(); tools_set_workspace(gPrefs.workspace); UIInit();
    accept_log = fopen("Retro68:SherclawkMCPAgentCheck.log", "w");
    if (!accept_log || !gWindow) return 1;
    memset(&info, 0, sizeof(info)); info.processInfoLength = sizeof(info);
    GetCurrentProcess(&psn); GetProcessInformation(&psn, &info);
    size_resource = Get1Resource('SIZE', -1);
    if (size_resource && GetHandleSize(size_resource) >= 10) {
        const unsigned char *p = (const unsigned char *)*size_resource;
        preferred = ((unsigned long)p[2] << 24) | ((unsigned long)p[3] << 16) | ((unsigned long)p[4] << 8) | p[5];
        minimum = ((unsigned long)p[6] << 24) | ((unsigned long)p[7] << 16) | ((unsigned long)p[8] << 8) | p[9];
    }
    fprintf(accept_log, "partition=%lu SIZE_preferred=%lu SIZE_minimum=%lu\n",
        (unsigned long)info.processSize, preferred, minimum);
    Check("minimum partition (including PEF loader overhead)", preferred == minimum && minimum >= 10UL * 1024 * 1024 &&
          info.processSize < 12UL * 1024 * 1024);
    Check("private model key present", *gPrefs.api_key != 0);
    Check("private configuration readable", McpHostLoad(config_bytes, sizeof(config_bytes), &length) == MCP_RUN_LOAD_OK);
    Check("private Tavily configuration enabled", !mcp_config_parse(config_bytes, length, &private_config,
          error, sizeof(error)) && private_config.enabled && private_config.official_tavily);
    memset(config_bytes, 0, sizeof(config_bytes));
    if (failures) goto done;
    gPrefs.max_rounds = 32; gPrefs.max_tools = 64;
    mcp_run_init(&acceptance_host);
    chat_reset(&gChat); agent_reset(&gAgent, session_journal, &gSession);
    LogOpen(); Memory("setup");
    Begin("Acceptance check: call mcp_tavily_tavily_search for classic Mac OS memory management with max_results 2. Then call mcp_tavily_tavily_extract for https://www.iana.org/help/example-domains with basic depth. Read the extracted result with read_mcp_result and at least three distinct pages of the search result using next_byte. Finish with two facts and their source links. Do not write files.");
    /* Changing the loader after start cannot alter this run's frozen config. */
    mode = 1;
    Check("live integrated run completes", Settle(300));
    if (gQuit) goto done;
    Check("model selected successful search and extraction", search_calls > 0 && extract_calls > 0 && search_ok > 0 && extract_ok > 0);
    Check("model read multiple distinct retained pages", pages >= 4 && distinct_pages >= 4);
    Check("discovery and both artifacts journaled", discovery_count == 1 && retained >= 2);
    links = strstr(gAgent.text, "https://") != NULL;
    Check("model final answer has source link", links);
    Check("history and final answer credential oracle", Clean(gAgent.history) && Clean(gAgent.text));
    Check("model request credential oracle", Clean(gJSON));
    for (i = 0; i < artifact_count; i++) ScanFile(artifacts[i]);
    StartHandoff(); Check("real handoff completes", Settle(180) && *gHandoffPath);
    if (gQuit) goto done;
    if (*gHandoffPath) ScanFile(gHandoffPath);
    if (gSession.open && journal_count < 16) strcpy(journals[journal_count++], gSession.path);
    before = opens;
    Begin("Call get_environment once, then report the available free heap. Do not use remote tools.");
    Check("disabled run completes natively", Settle(180) && strstr(gAgent.history, "free_heap_bytes"));
    if (gQuit) goto done;
    Check("disabled configuration never opens MCP transport", opens == before);
    mode = 2; before = notices;
    Begin("Call get_environment once, then report the available free heap. Do not use remote tools.");
    Check("failed discovery leaves native tools usable", Settle(180) && strstr(gAgent.history, "free_heap_bytes") && notices > before);
    if (gQuit) goto done;
    for (i = 0; i < 5 && !gQuit; i++) StopAt(i);
    for (i = 0; i < artifact_count; i++) ScanFile(artifacts[i]);
    Memory("finished");
    fprintf(accept_log, "minimum_heap=%ld minimum_stack=%ld longest_step_ticks=%lu\n",
        least_heap, least_stack, worst_step);
    LogClose(); ScanFile("Retro68:Sherclawk.log");
done:
    session_close(&gSession);
    for (i = 0; i < journal_count; i++) ScanFile(journals[i]);
    fprintf(accept_log, "RESULT failures=%d\n", failures); fclose(accept_log);
    FlushVol(NULL, 0);
    memset(&private_config, 0, sizeof(private_config)); memset(private_session, 0, sizeof(private_session));
    UIDispose(); MacTLS_Shutdown(); return failures ? 1 : 0;
}
