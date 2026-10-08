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
static const char *usage1 =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Counted.\"}}],"
    "\"usage\":{\"prompt_tokens\":1200,\"completion_tokens\":30,\"total_tokens\":1230,\"cost\":0.012345}}";
static const char *usage2 =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Counted again.\"}}],"
    "\"usage\":{\"prompt_tokens\":2400,\"cost\":0.000002}}";
static const char *usage_missing =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"No usage.\"}}]}";
static const char *usage_big = "{\"usage\":{\"prompt_tokens\":900000,\"cost\":999999999.999999}}";
static const char *handoff_usage =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Handoff summary.\"}}],"
    "\"usage\":{\"prompt_tokens\":1500,\"cost\":0.034567}}";
static void usage(void)
{
    size_t i, used;
    begin();
    assert(!agent_response(&a, usage1, strlen(usage1), 200, error, sizeof(error)));
    assert(a.context_seen && a.context_tokens == 1200);
    assert(a.cost_seen && a.cost_micros == 12345);
    assert(!agent_begin(&a, "Again", error, sizeof(error)));
    assert(!agent_response(&a, usage2, strlen(usage2), 200, error, sizeof(error)));
    assert(a.context_tokens == 2400 && a.cost_micros == 12347);
    /* Missing or malformed usage never moves either total. */
    agent_usage_absorb(&a, usage_missing, strlen(usage_missing));
    assert(a.context_tokens == 2400 && a.cost_micros == 12347);
    agent_usage_absorb(&a, "{\"usage\":{\"prompt_tokens\":\"x\",\"cost\":\"cheap\"}}",
                       strlen("{\"usage\":{\"prompt_tokens\":\"x\",\"cost\":\"cheap\"}}"));
    assert(a.context_tokens == 2400 && a.cost_micros == 12347);
    agent_usage_absorb(&a, "{\"usage\":[-1]}", strlen("{\"usage\":[-1]}"));
    agent_usage_absorb(&a, "not json", strlen("not json"));
    agent_usage_absorb(&a, "{\"usage\":{\"prompt_tokens\":-5,\"cost\":-1}}",
                       strlen("{\"usage\":{\"prompt_tokens\":-5,\"cost\":-1}}"));
    assert(a.context_tokens == 2400 && a.cost_micros == 12347);
    /* Rejected, non-2xx, truncated and journal-failed responses keep totals. */
    begin();
    used = a.used;
    assert(agent_response(&a, "{\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"cost\":1.5}}",
                          strlen("{\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"cost\":1.5}}"),
                          200, error, sizeof(error)) == -1);
    assert(!a.context_seen && !a.cost_seen && a.used == used);
    assert(agent_response(&a, usage1, strlen(usage1), 503, error, sizeof(error)) == -1);
    assert(!a.context_seen && !a.cost_seen);
    for (i = 1; i < strlen(usage1); i++) {
        assert(agent_response(&a, usage1, i, 200, error, sizeof(error)) == -1);
        assert(!a.context_seen && !a.cost_seen && a.used == used);
    }
    fail_record = 1;
    assert(agent_response(&a, usage1, strlen(usage1), 200, error, sizeof(error)) == -1);
    assert(a.used == used && !a.context_seen && !a.cost_seen);
    fail_record = 0;
    /* The accumulator clamps instead of wrapping and stays clamped. */
    agent_reset(&a, journal, NULL);
    for (i = 0; i < 16; i++) agent_usage_absorb(&a, usage_big, strlen(usage_big));
    assert(a.cost_seen && a.cost_micros == AGENT_COST_MICROS_MAX && a.context_tokens == 900000);
    agent_usage_absorb(&a, usage2, strlen(usage2));
    assert(a.cost_micros == AGENT_COST_MICROS_MAX);
    /* A handoff completion shares the absorber; the fresh candidate starts
     * reset and receives cost only, leaving the context line unset. */
    agent_reset(&a, journal, NULL);
    agent_usage_absorb(&a, handoff_usage, strlen(handoff_usage));
    assert(a.cost_micros == 34567 && a.context_tokens == 1500);
    assert(!agent_handoff_response(handoff_usage, strlen(handoff_usage), 200, req, sizeof(req), error, sizeof(error)));
    assert(!strcmp(req, "Handoff summary."));
    agent_reset(&candidate, journal, NULL);
    assert(!agent_handoff_seed(&candidate, req, "old.jsonl", "handoff.md"));
    candidate.cost_micros = a.cost_micros;
    candidate.cost_seen = a.cost_seen;
    assert(candidate.cost_micros == 34567 && candidate.cost_seen);
    assert(!candidate.context_seen && !candidate.context_tokens);
}
static void model_metadata(void)
{
    static const char *page =
        "{\"total_count\":4,\"data\":["
        "{\"id\":\"openai/gpt-4o-pro\",\"name\":\"GPT-4o Pro\",\"context_length\":262144},"
        "{\"id\":\"openai/gpt-4o-2024-08-06\",\"name\":\"GPT-4o (2024-08-06)\",\"context_length\":128000},"
        "{\"id\":\"openai/gpt-4o\",\"name\":\"GPT-4o\",\"context_length\":131072,"
        "\"reasoning\":{\"mandatory\":true,\"default_enabled\":false,\"default_effort\":\"medium\","
        "\"supported_efforts\":[\"high\",\"medium\",\"low\",\"minimal\",\"none\"]}},"
        "{\"id\":\"openai/gpt-4o:batch\",\"name\":\"GPT-4o (batch)\",\"context_length\":65536}]}";
    static const char *minimal =
        "{\"data\":[{\"id\":\"openai/o3\",\"name\":\"o3\",\"context_length\":200000,"
        "\"reasoning\":{\"mandatory\":false}}]}";
    static const char *plain =
        "{\"data\":[{\"id\":\"z-ai/glm\",\"name\":\"GLM\",\"context_length\":8192}]}";
    static char big[65536], query[CHAT_MODEL_CAP * 3 + 32];
    AgentModelInfo info;
    size_t i, at = 0;
    /* The lookup path percent-encodes the model and pins the page size. */
    assert(agent_model_query(query, sizeof(query), "openai/gpt-4o:free") ==
           (int)strlen("/api/v1/models?q=openai%2Fgpt-4o%3Afree&limit=10"));
    assert(!strcmp(query, "/api/v1/models?q=openai%2Fgpt-4o%3Afree&limit=10"));
    assert(agent_model_query(query, sizeof(query), "deepseek.v3_x-1") > 0);
    assert(!strcmp(query, "/api/v1/models?q=deepseek.v3_x-1&limit=10"));
    assert(agent_model_query(query, 8, "openai/gpt-4o") == -1);
    /* Substring variants (-pro, dated alias, :batch) are returned with the
     * exact row; only the exact id may supply the metadata. */
    assert(!agent_model_info(page, strlen(page), "openai/gpt-4o", &info));
    assert(!strcmp(info.name, "GPT-4o") && info.context_length == 131072);
    assert(info.reasoning && info.mandatory && !info.default_enabled);
    assert(!strcmp(info.default_effort, "medium") && info.effort_count == 5);
    assert(!strcmp(info.supported_efforts[0], "high") && !strcmp(info.supported_efforts[4], "none"));
    /* A dated alias row is selected only by its own id and keeps no metadata
     * when the JSON object has no reasoning block. */
    assert(!agent_model_info(page, strlen(page), "openai/gpt-4o-2024-08-06", &info));
    assert(!strcmp(info.name, "GPT-4o (2024-08-06)") && info.context_length == 128000 && !info.reasoning);
    /* Absent default_effort and supported_efforts stay empty; mandatory is
     * read on its own. */
    assert(!agent_model_info(minimal, strlen(minimal), "openai/o3", &info));
    assert(info.reasoning && !info.mandatory && !info.default_enabled);
    assert(!info.default_effort[0] && !info.effort_count);
    assert(!agent_model_info(plain, strlen(plain), "z-ai/glm", &info));
    assert(!info.reasoning && !info.mandatory && info.context_length == 8192);
    /* No exact row, empty id, malformed or truncated pages: quiet -1. */
    assert(agent_model_info(page, strlen(page), "openai/gpt-4o-mini", &info) == -1);
    assert(agent_model_info(page, strlen(page), "", &info) == -1);
    assert(agent_model_info("not json", strlen("not json"), "openai/gpt-4o", &info) == -1);
    assert(agent_model_info("{\"data\":{}}", strlen("{\"data\":{}}"), "openai/gpt-4o", &info) == -1);
    assert(agent_model_info("{\"data\":[]}", strlen("{\"data\":[]}"), "openai/gpt-4o", &info) == -1);
    assert(agent_model_info("{\"error\":{\"message\":\"no\"}}", strlen("{\"error\":{\"message\":\"no\"}}"), "openai/gpt-4o", &info) == -1);
    for (i = 1; i < strlen(page); i++) assert(agent_model_info(page, i, "openai/gpt-4o", &info) == -1);
    /* More row tokens than the shared parser cap: quiet -1. */
    at += (size_t)snprintf(big + at, sizeof(big) - at, "{\"data\":[");
    for (i = 0; i < 1500; i++)
        at += (size_t)snprintf(big + at, sizeof(big) - at, "%s{\"id\":\"m%lu\",\"context_length\":1024}",
                               i ? "," : "", (unsigned long)i);
    at += (size_t)snprintf(big + at, sizeof(big) - at, "]}");
    assert(at < sizeof(big));
    assert(agent_model_info(big, at, "m1", &info) == -1);
}
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
    usage();
    model_metadata();
    puts("PASS agent tools, usage accounting, history, truncation, Stop, persistence barriers, bounds and model metadata");
    return 0;
}
