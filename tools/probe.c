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
void tools_execute(const AgentCall *call, char *out, size_t cap)
{
    if (!strcmp(call->name, "get_environment"))
        snprintf(out, cap, "{\"status\":\"ok\",\"backend\":\"host diagnostic fixture\",\"workspace\":\"Retro68:\",\"read_only\":true}");
    else if (!strcmp(call->name, "list_files"))
        snprintf(out, cap, "{\"status\":\"ok\",\"files\":[{\"path\":\"Sherclawk Fixture.txt\",\"kind\":\"file\"}],\"truncated\":false}");
    else if (!strcmp(call->name, "read_text"))
        snprintf(out, cap, "{\"status\":\"ok\",\"text\":\"The diagnostic codeword is copper-crab.\",\"truncated\":false}");
    else snprintf(out, cap, "{\"status\":\"error\",\"code\":\"UNKNOWN_TOOL\"}");
}
#else
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
        "Report the diagnostic codeword from the file. You must use all three tools.", error, sizeof(error))) return 1;
    while (agent.active && rounds++ < 8) {
        length = agent_request(&agent, SHERCLAWK_MODEL, body, sizeof(body));
        if (length < 0) return 1;
        length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", SHERCLAWK_API_KEY,
            attribution, body, (size_t)length, net.request, sizeof(net.request));
        if (exchange(length) < 0 || agent_response(&agent, net.body, net.body_len, net.status, error, sizeof(error))) {
            fprintf(logfile, "FAIL model %s\n", error); return 1;
        }
        while (agent.next < agent.count) {
            AgentCall *call = &agent.calls[agent.next];
            fprintf(logfile, "CALL %s %s\n", call->name, call->arguments);
            if (!strcmp(call->name, "get_environment")) seen |= 1;
            if (!strcmp(call->name, "list_files")) seen |= 2;
            if (!strcmp(call->name, "read_text")) seen |= 4;
            tools_execute(call, tool_result, sizeof(tool_result));
            fprintf(logfile, "TOOL %s\n", tool_result);
            if (agent_tool_result(&agent, tool_result, error, sizeof(error))) return 1;
        }
        fflush(logfile);
    }
    fprintf(logfile, "FINAL %s\n", agent.text);
    if (agent.active || seen != 7 || !strstr(agent.text, "copper-crab")) return 1;
    fprintf(logfile, "PASS real three-tool conversation and model follow-up\n");
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
