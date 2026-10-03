/* Exercise actual call/result protocol, persistence barriers, Stop and bounds. */
#include "agent.h"
#include "json.h"
#include "text.h"
#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static Agent a;
static char req[CHAT_REQUEST_CAP], error[256], response[4096];
static int records, fail_record;
static int journal(void *ctx, const char *event, const char *json)
{
    JsonToken tokens[512];
    (void)ctx; (void)event;
    assert(json_parse(json, strlen(json), tokens, 512) > 0);
    if (fail_record) return -1;
    records++; return 0;
}
static void begin(void)
{
    records = fail_record = 0;
    agent_reset(&a, journal, NULL);
    assert(!agent_begin(&a, "Inspect my files", error, sizeof(error)));
}
static void named_call(const char *finish, const char *name, const char *arguments)
{
    char quoted[2048];
    assert(json_quote(arguments, quoted, sizeof(quoted)) > 0);
    snprintf(response, sizeof(response), "{\"choices\":[{\"finish_reason\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":%s}}]}}]}", finish, name, quoted);
}
static void call(const char *finish) { named_call(finish, "get_environment", "{}"); }
int main(void)
{
    const char *final = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Inspected.\"}}]}";
    size_t used, i;
    begin();
    assert(agent_request(&a, "model", req, sizeof(req)) > 0);
    assert(strstr(req, "\"tools\"") && strstr(req, "\"role\":\"system\"") && strstr(req, "write_text") && strstr(req, "edit_text") && strstr(req, "search_text"));
    call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(a.count == 1 && !a.next && a.active && records == 2);
    assert(!strcmp(a.calls[0].id, "c1") && !strcmp(a.calls[0].arguments, "{}"));
    assert(agent_request(&a, "model", req, sizeof(req)) == -1);
    assert(!agent_tool_result(&a, "{\"status\":\"ok\",\"text\":\"caf\xc3\xa9\"}", error, sizeof(error)));
    assert(agent_request(&a, "model", req, sizeof(req)) > 0);
    assert(strstr(req, "\"tool_call_id\":\"c1\"") && strstr(req, "caf\xc3\xa9"));
    assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
    assert(!a.active && a.messages == 4 && records == 4);
    assert(!agent_begin(&a, "What did you find?", error, sizeof(error)));
    assert(strstr(a.history, "Inspected."));
    used = a.used;
    assert(agent_response(&a, "{}", 2, 503, error, sizeof(error)) == -1 && a.used == used);
    assert(!agent_stop(&a, "network failed") && a.used == used);
    begin(); call("length"); used = a.used;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1 && a.used == used);
    begin(); call("stop");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)) && a.count == 1);
    assert(!agent_stop(&a, "user stopped"));
    assert(!a.active && a.next == a.count && strstr(a.history, "interrupted") && strstr(a.history, "c1"));
    begin(); call("tool_calls"); used = a.used; fail_record = 1;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1 && a.used == used && a.count == 0);
    fail_record = 0;
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    used = a.used; fail_record = 1;
    assert(agent_tool_result(&a, "ok", error, sizeof(error)) == -1 && !a.next && a.used == used);
    fail_record = 0;
    assert(!agent_tool_result(&a, "ok", error, sizeof(error)));
    begin(); call("tool_calls");
    for (i = 0; i < strlen(response); i++) {
        assert(agent_response(&a, response, i, 200, error, sizeof(error)) == -1);
        assert(a.messages == 1 && a.count == 0);
    }
    /* A truncated mutation is rejected; Stop pairs an unexecuted write with
     * an interrupted result instead of sending it to the executor. */
    begin(); named_call("length", "write_text", "{\"path\":\"new.c\",\"text\":\"source\"}");
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1 && !a.count);
    named_call("tool_calls", "write_text", "{\"path\":\"new.c\",\"text\":\"source\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!strcmp(a.calls[0].name, "write_text") && !agent_stop(&a, "Stop before create"));
    assert(a.next == a.count && strstr(a.history, "interrupted") && !a.active);
    begin(); named_call("length", "edit_text", "{\"path\":\"old.c\",\"expected_revision\":\"full-x\",\"old_text\":\"a\",\"new_text\":\"b\"}");
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1 && !a.count);
    named_call("tool_calls", "edit_text", "{\"path\":\"old.c\",\"expected_revision\":\"full-x\",\"old_text\":\"a\",\"new_text\":\"b\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!strcmp(a.calls[0].name, "edit_text") && !agent_stop(&a, "Stop before edit"));
    assert(a.next == a.count && strstr(a.history, "interrupted") && !a.active);
    begin(); named_call("tool_calls", "search_text", "{\"root\":\"\",\"query\":\"lobster\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!strcmp(a.calls[0].name, "search_text") && !agent_stop(&a, "Stop before search"));
    assert(a.next == a.count && strstr(a.history, "interrupted") && !a.active);
    begin(); call("tool_calls");
    a.used = sizeof(a.history) - 10;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(text_to_macroman_strict("\xf0\x9f\xa6\x80", req, sizeof(req)) == -1);
    assert(text_to_macroman_strict("caf\xc3\xa9", req, sizeof(req)) == 4);
    puts("PASS agent tools, history, truncation, Stop, persistence barriers and bounds");
    return 0;
}
