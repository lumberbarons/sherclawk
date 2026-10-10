/* Live diagnostic for the exact Sherclawk protocol/transport modules.
 * Guest: writes Retro68:SherclawkProbe.log, cycles OT between requests, quits.
 * Host: uses the existing Certainly socket shim, never prints credentials.
 * Public model endpoint metadata replaces the unbounded full catalog. */
#include "chat.h"
#include "agent.h"
#include "tools.h"
#include "network.h"
#include "http.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#ifdef SHERCLAWK_HOST
#include <unistd.h>
#include <signal.h>
#else
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OpenTransport.h>
#endif
extern unsigned long TickCount(void);
static ChatNetwork net;
static Agent agent;
static char body[CHAT_REQUEST_CAP], tool_result[AGENT_RESULT_CAP];
static FILE *logfile;
/* The guest printf has no %lld; render the 64-bit accumulator as digits. */
static void micros_text(long long value, char *out, size_t cap)
{
    char digits[24];
    size_t at = sizeof(digits), n;
    unsigned long long v = (unsigned long long)value;
    do { digits[--at] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    n = sizeof(digits) - at;
    if (n >= cap) n = cap - 1;
    memcpy(out, digits + sizeof(digits) - n, n);
    out[n] = 0;
}
static void yield(void)
{
#ifdef SHERCLAWK_HOST
    usleep(16000);
#else
    EventRecord event;
    WaitNextEvent(everyEvent, &event, 1, NULL);
#endif
}
static void cleanup(void)
{
    network_close(&net);
#ifndef SHERCLAWK_HOST
    unsigned long start = TickCount();
    while ((unsigned long)(TickCount() - start) < 60) yield();
    CloseOpenTransport();
    start = TickCount();
    while ((unsigned long)(TickCount() - start) < 60) yield();
#endif
}
static int exchange(int length)
{
    unsigned long start = TickCount();
    int result;
#ifndef SHERCLAWK_HOST
    InitOpenTransport();
#endif
    if (length < 0 || network_start(&net, net.request, (size_t)length) < 0) {
        fprintf(logfile, "FAIL start\n"); cleanup(); return -1;
    }
    while (!(result = network_step(&net))) {
        if ((unsigned long)(TickCount() - start) > 120UL * 60UL) {
            fprintf(logfile, "FAIL timeout\n"); cleanup(); return -1;
        }
        yield();
    }
    fprintf(logfile, "HTTP %d TLS %d received %lu bytes in %lu ticks\n", net.status,
        (int)net.version, (unsigned long)net.received, (unsigned long)(TickCount() - start));
    if (result < 0) fprintf(logfile, "FAIL %s\n", net.error);
    fflush(logfile); cleanup(); return result < 0 ? -1 : 0;
}

#ifdef SHERCLAWK_HOST
/* Host transport validation uses an explicitly labeled fixture executor.
 * Guest builds link the actual Toolbox implementation instead. */
int tools_execute(const AgentCall *call, char *out, size_t cap)
{
    if (!strcmp(call->name, "get_environment"))
        snprintf(out, cap, "{\"status\":\"ok\",\"backend\":\"host diagnostic fixture\",\"workspace\":\"Retro68:\",\"read_only\":true}");
    else if (!strcmp(call->name, "list_files"))
        snprintf(out, cap, "{\"status\":\"ok\",\"files\":[{\"path\":\"Sherclawk Fixture.txt\",\"kind\":\"file\"}],\"truncated\":false}");
    else if (!strcmp(call->name, "read_text"))
        snprintf(out, cap, "{\"status\":\"ok\",\"text\":\"The diagnostic codeword is copper-crab.\",\"truncated\":false}");
    else if (!strcmp(call->name, "get_file_info"))
        snprintf(out, cap, "{\"status\":\"ok\",\"path\":\"Sherclawk Fixture.txt\",\"kind\":\"file\",\"file_type\":\"TEXT\",\"creator\":\"ttxt\"}");
    else if (!strcmp(call->name, "list_fonts"))
        snprintf(out, cap, "{\"status\":\"ok\",\"fonts\":[{\"id\":16383,\"name\":\"Chicago\"},{\"id\":3,\"name\":\"Geneva\"}],\"truncated\":false,\"next_cursor\":null}");
    else if (!strcmp(call->name, "measure_text"))
        snprintf(out, cap, "{\"status\":\"ok\",\"font\":\"Chicago\",\"font_id\":16383,\"size\":12,\"width\":63,\"line_height\":14}");
    else if (!strcmp(call->name, "list_processes"))
        snprintf(out, cap, "{\"status\":\"ok\",\"processes\":[{\"name\":\"SherclawkProbe\",\"psn\":\"00000000:00000001\",\"front\":true,\"self\":true}],\"truncated\":false,\"next_cursor\":null}");
    else if (!strcmp(call->name, "list_resources"))
        snprintf(out, cap, "{\"status\":\"ok\",\"resources\":[{\"type\":\"SIZE\",\"id\":-1,\"bytes\":16,\"name\":\"\"}],\"truncated\":false,\"next_cursor\":null}");
    else if (!strcmp(call->name, "read_resource"))
        snprintf(out, cap, "{\"status\":\"ok\",\"type\":\"SIZE\",\"id\":-1,\"format\":\"hex\",\"hex\":\"0000001000000010\",\"truncated\":false}");
    else snprintf(out, cap, "{\"status\":\"error\",\"code\":\"UNKNOWN_TOOL\"}");
    return 0;
}
#else
#include "diagnostic-tools.h"
static int native_checks(void)
{
    AgentCall call;
    FILE *fixture;
    int failures = 0;
    fixture = fopen(SHERCLAWK_WORKSPACE "Sherclawk Fixture.txt", "w");
    if (!fixture) return 1;
    fputs("Sherclawk native fixture.\rThe diagnostic codeword is copper-crab.\r", fixture);
    fclose(fixture);
    memset(&call, 0, sizeof(call)); strcpy(call.id, "native");
    strcpy(call.name, "get_environment"); strcpy(call.arguments, "{}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE environment %s\n", tool_result);
    if (!strstr(tool_result, "\"status\":\"ok\"")) failures++;
    strcpy(call.name, "list_files"); strcpy(call.arguments, "{\"root\":\"\",\"limit\":12}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE list %s\n", tool_result);
    if (!strstr(tool_result, "\"status\":\"ok\"")) failures++;
    strcpy(call.name, "read_text"); strcpy(call.arguments, "{\"path\":\"Sherclawk Fixture.txt\"}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE read %s\n", tool_result);
    if (!strstr(tool_result, "copper-crab")) failures++;
    strcpy(call.arguments, "{\"path\":\":Sherclawk Fixture.txt\"}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    if (!strstr(tool_result, "\"status\":\"error\"")) failures++;
    strcpy(call.arguments, "{\"path\":\"Sherclawk\"}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    if (!strstr(tool_result, "NOT_TEXT")) failures++;
    strcpy(call.arguments, "{\"path\":\"Sherclawk Fixture.txt\",\"path\":\"other\"}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    if (!strstr(tool_result, "ARGUMENTS")) failures++;
    strcpy(call.name, "not_installed"); strcpy(call.arguments, "{}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    if (!strstr(tool_result, "UNKNOWN_TOOL")) failures++;
    strcpy(call.name, "get_file_info"); strcpy(call.arguments, "{\"path\":\"Sherclawk Fixture.txt\"}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE file_info %s\n", tool_result);
    if (!strstr(tool_result, "\"file_type\":\"TEXT\"") || !strstr(tool_result, "\"status\":\"ok\"")) failures++;
    strcpy(call.name, "list_fonts"); strcpy(call.arguments, "{\"limit\":24}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE fonts %s\n", tool_result);
    if (!strstr(tool_result, "\"fonts\":[{") || strstr(tool_result, "\"status\":\"error\"")) failures++;
    strcpy(call.name, "measure_text"); strcpy(call.arguments, "{\"text\":\"Sherclawk\",\"font\":\"Chicago\",\"size\":12}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE measure %s\n", tool_result);
    if (!strstr(tool_result, "\"width\":") || strstr(tool_result, "\"status\":\"error\"")) failures++;
    strcpy(call.name, "list_processes"); strcpy(call.arguments, "{}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE processes %s\n", tool_result);
    if (!strstr(tool_result, "\"self\":true")) failures++;
    strcpy(call.name, "list_resources"); strcpy(call.arguments, "{\"path\":\"Sherclawk\",\"limit\":4}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE resources %s\n", tool_result);
    if (!strstr(tool_result, "\"resources\":[{") || strstr(tool_result, "\"status\":\"error\"")) failures++;
    strcpy(call.name, "read_resource"); strcpy(call.arguments, "{\"path\":\"Sherclawk\",\"type\":\"SIZE\",\"id\":-1}");
    tools_execute(&call, tool_result, sizeof(tool_result));
    fprintf(logfile, "NATIVE resource %s\n", tool_result);
    if (!strstr(tool_result, "\"format\":\"hex\"") || strstr(tool_result, "\"status\":\"error\"")) failures++;
    fprintf(logfile, "NATIVE checks failures=%d\n", failures); fflush(logfile);
    return failures;
}
#endif
static int run_agent(void)
{
    char error[256], attribution[256];
    int rounds = 0, seen = 0, length;
    agent_reset(&agent, NULL, NULL);
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        fprintf(logfile, "FAIL attribution URL is too long\n"); return 1;
    }
    if (agent_begin(&agent, "Use get_environment, then list_files for the workspace root (empty root). "
        "Then use read_text on Sherclawk Fixture.txt. If the listing is paginated you may read that exact path directly. "
        "Report the diagnostic codeword from the file. Then call get_file_info on that same file and state its Finder type. "
        "Then call list_fonts with limit 24, and call measure_text with the first font id from that list, text \"Sherclawk\" and size 12, and state its width. "
        "You must use all six tools.", error, sizeof(error))) return 1;
    while (agent.active && rounds++ < 8) {
        length = agent_request(&agent, SHERCLAWK_MODEL, body, sizeof(body));
        if (length < 0) return 1;
        length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", SHERCLAWK_API_KEY,
            attribution, body, (size_t)length, net.request, sizeof(net.request));
        if (exchange(length) < 0 || agent_response(&agent, net.body, net.body_len, net.status, error, sizeof(error))) {
            fprintf(logfile, "FAIL model %s\n", error); return 1;
        }
        {
            char cost[24];
            micros_text(agent.cost_micros, cost, sizeof(cost));
            fprintf(logfile, "USAGE tokens=%ld cost_micros=%s\n", agent.context_tokens, cost);
        }
        while (agent.next < agent.count) {
            AgentCall *call = &agent.calls[agent.next];
            fprintf(logfile, "CALL %s %s\n", call->name, call->arguments);
            if (!strcmp(call->name, "get_environment")) seen |= 1;
            if (!strcmp(call->name, "list_files")) seen |= 2;
            if (!strcmp(call->name, "read_text")) seen |= 4;
            if (!strcmp(call->name, "get_file_info")) seen |= 8;
            if (!strcmp(call->name, "list_fonts")) seen |= 16;
            if (!strcmp(call->name, "measure_text")) seen |= 32;
            tools_execute(call, tool_result, sizeof(tool_result));
            fprintf(logfile, "TOOL %s\n", tool_result);
            if (agent_tool_result(&agent, tool_result, error, sizeof(error))) return 1;
        }
        fflush(logfile);
    }
    fprintf(logfile, "FINAL %s\n", agent.text);
    if (agent.active || seen != 63 || !strstr(agent.text, "copper-crab")) return 1;
    fprintf(logfile, "PASS real six-tool conversation and model follow-up\n");
    return 0;
}
int main(void)
{
    int failures = 0;
#ifdef SHERCLAWK_HOST
    signal(SIGPIPE, SIG_IGN); logfile = stdout;
#else
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    logfile = fopen(SHERCLAWK_WORKSPACE "SherclawkProbe.log", "w");
    if (!logfile) return 1;
    failures += native_checks();
#endif
    MacTLS_Init();
    fprintf(logfile, "Sherclawk tool probe model=%s\n", SHERCLAWK_MODEL); fflush(logfile);
    if (!SHERCLAWK_API_KEY[0]) { fprintf(logfile, "FAIL no local diagnostic key\n"); failures++; }
    else failures += run_agent();
    fprintf(logfile, "RESULT failures=%d\n", failures); fflush(logfile);
    MacTLS_Shutdown();
#ifndef SHERCLAWK_HOST
    fclose(logfile);
#endif
    return failures ? 1 : 0;
}
