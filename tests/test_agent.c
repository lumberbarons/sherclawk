/* Exercise actual call/result protocol, persistence barriers, Stop and bounds. */
#include "agent.h"
#include "json.h"
#include "text.h"
#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static Agent a, saved, candidate;
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
    assert(strstr(req, "\"tools\"") && strstr(req, "\"role\":\"system\"") && strstr(req, "write_text") && strstr(req, "create_project") && strstr(req, "create_folder") && strstr(req, "edit_text") && strstr(req, "search_text") && strstr(req,"build_project") && strstr(req,"read_build_log"));
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
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1 && a.used == used && strstr(error, "token limit"));
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
    /* Grow a real conversation beyond the old history limit, then fill the
     * history nearly to capacity. The wire request must still fit with the
     * system policy and every tool schema present. */
    begin();
    memset(req, 'x', 4096); req[4096] = 0;
    while (a.used < AGENT_HISTORY_CAP * 3 / 4) {
        assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
        assert(!agent_begin(&a, req, error, sizeof(error)));
    }
    call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "ok", error, sizeof(error)));
    assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
    while (a.used < AGENT_HISTORY_CAP - 12 * 1024) {
        assert(!agent_begin(&a, req, error, sizeof(error)));
        assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
    }
    memset(req, 'x', sizeof(a.history) - a.used - 128);
    req[sizeof(a.history) - a.used - 128] = 0;
    assert(!agent_begin(&a, req, error, sizeof(error)));
    assert(agent_request(&a, "model", req, sizeof(req)) > AGENT_HISTORY_CAP);
    assert(strstr(req, "\"tools\"") && strstr(req, "read_build_log"));
    call("tool_calls"); used = a.used;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(a.used == used && !a.count && strstr(error, "History capacity"));
    /* Handoff does not append to a full history or use tools. It is allowed
     * only between runs, and failed candidate persistence cannot clear it. */
    assert(!agent_stop(&a, "history full"));
    saved = a;
    assert(agent_handoff_request(&a, "model", req, sizeof(req)) > AGENT_HISTORY_CAP);
    assert(!strstr(req, "\"tools\":") && strstr(req, "Write the handoff summary now"));
    assert(!memcmp(&a, &saved, sizeof(a)));
    assert(!agent_handoff_response(final, strlen(final), 200, req, sizeof(req), error, sizeof(error)));
    assert(!strcmp(req, "Inspected."));
    assert(agent_handoff_response(final, strlen(final), 500, req, sizeof(req), error, sizeof(error)) == -1);
    call("tool_calls");
    assert(agent_handoff_response(response, strlen(response), 200, req, sizeof(req), error, sizeof(error)) == -1);
    call("length");
    assert(agent_handoff_response(response, strlen(response), 200, req, sizeof(req), error, sizeof(error)) == -1);
    for (i = 0; i < strlen(final); i++)
        assert(agent_handoff_response(final, i, 200, req, sizeof(req), error, sizeof(error)) == -1);
    agent_reset(&candidate, journal, NULL); fail_record = 1;
    assert(agent_handoff_seed(&candidate, "# Goal\nBuild a prototype.", "old.jsonl", "handoff.md") == -1);
    assert(!candidate.messages && !candidate.used && !memcmp(&a, &saved, sizeof(a)));
    fail_record = 0;
    assert(!agent_handoff_seed(&candidate, "# Goal\nBuild a prototype.", "old.jsonl", "handoff.md"));
    assert(candidate.messages == 1 && !candidate.active && strstr(candidate.history, "old.jsonl") && strstr(candidate.history, "handoff.md"));
    assert(!agent_begin(&candidate, "Continue", error, sizeof(error)));
    assert(agent_handoff_request(&candidate, "model", req, sizeof(req)) == -1);
    assert(agent_request(&candidate, "model", req, sizeof(req)) > 0 && strstr(req, "Build a prototype"));
    begin(); call("tool_calls");
    a.used = sizeof(a.history) - 10;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(text_to_macroman_strict("\xf0\x9f\xa6\x80", req, sizeof(req)) == -1);
    assert(text_to_macroman_strict("caf\xc3\xa9", req, sizeof(req)) == 4);
    puts("PASS agent tools, history, truncation, Stop, persistence barriers and bounds");
    return 0;
}
