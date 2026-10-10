/* Exercise actual call/result protocol, persistence barriers, Stop and bounds. */
#include "agent.h"
#include "json.h"
#include "text.h"
#include "chat.h"
#include "base64.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static Agent a, saved, candidate;
static char req[CHAT_REQUEST_CAP], error[256], response[4096];
static int records, fail_record;
static char last_event[64];
static int journal(void *ctx, const char *event, const char *json)
{
    JsonToken tokens[512];
    (void)ctx; snprintf(last_event, sizeof(last_event), "%s", event);
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
/* Every request body the tests build is parsed and pinned by structure, so a
 * wrong model, role, message order or tool schema cannot hide behind the
 * substrings the individual assertions look for. */
static JsonToken wire[65536];
static char decoded[CHAT_REQUEST_CAP];
static const char *const tool_names[] = {
    "get_environment", "list_files", "read_text", "search_text", "write_text", "edit_text",
    "create_folder", "create_project", "build_project", "read_build_log", "run_application",
    "quit_application", "get_file_info", "resolve_alias", "list_processes", "list_fonts",
    "measure_text", "list_resources", "read_resource", "view_image",
};
static int member(int object, const char *key)
{
    int at = json_member(req, wire, object, key);
    assert(at >= 0);
    return at;
}
static int element(int array, int n)
{
    int at = array + 1;
    assert(wire[array].type == JSON_ARRAY);
    while (n-- > 0) { assert(at < wire[array].next); at = wire[at].next; }
    assert(at < wire[array].next);
    return at;
}
static int length_of(int array)
{
    int at, n = 0;
    assert(wire[array].type == JSON_ARRAY);
    for (at = array + 1; at < wire[array].next; at = wire[at].next) n++;
    return n;
}
static int is_text(int at, const char *want)
{
    return json_string(req, wire, at, decoded, sizeof(decoded)) >= 0 && !strcmp(decoded, want);
}
static int is_primitive(int at, const char *want)
{
    return wire[at].type == JSON_PRIMITIVE && (size_t)(wire[at].end - wire[at].start) == strlen(want) &&
           !memcmp(req + wire[at].start, want, strlen(want));
}
static int message(int messages, int n, const char *role)
{
    int at = element(messages, n);
    assert(wire[at].type == JSON_OBJECT && is_text(member(at, "role"), role));
    return at;
}
/* Parses req as a request for `model`; returns its messages array. */
static int parse_request(int length, const char *model)
{
    int messages;
    assert(length > 0 && json_parse(req, (size_t)length, wire, 65536) > 0 && wire[0].type == JSON_OBJECT);
    assert(is_text(member(0, "model"), model) && is_primitive(member(0, "stream"), "false"));
    messages = member(0, "messages");
    assert(wire[messages].type == JSON_ARRAY && length_of(messages) > 0);
    return messages;
}
/* Each registered tool appears exactly once, whatever the order, as a function
 * with a description and an object parameter schema. */
static void check_tool_schemas(int tools)
{
    enum { TOOLS = sizeof(tool_names) / sizeof(*tool_names) };
    int seen[TOOLS] = {0}, i, j;
    assert(wire[tools].type == JSON_ARRAY && length_of(tools) == TOOLS);
    for (i = 0; i < TOOLS; i++) {
        int tool = element(tools, i), function = member(tool, "function"), parameters = member(function, "parameters");
        assert(is_text(member(tool, "type"), "function"));
        assert(json_string(req, wire, member(function, "name"), decoded, sizeof(decoded)) > 0);
        for (j = 0; j < TOOLS && strcmp(decoded, tool_names[j]); j++) {}
        assert(j < TOOLS && !seen[j]);
        seen[j] = 1;
        assert(json_string(req, wire, member(function, "description"), decoded, sizeof(decoded)) > 0 && *decoded);
        assert(is_text(member(parameters, "type"), "object") && wire[member(parameters, "properties")].type == JSON_OBJECT);
    }
}
/* The shape every agent_request / agent_request_image body shares: the system
 * message first, only known roles after it, every tool result tied to a call id
 * and the full tool schema list. Returns `length` so asserts can wrap a build. */
static int pin(int length)
{
    int messages, i, n;
    if (length <= 0) return length;
    messages = parse_request(length, "model");
    assert(is_primitive(member(0, "parallel_tool_calls"), "false") && wire[member(0, "max_tokens")].type == JSON_PRIMITIVE);
    n = length_of(messages);
    assert(n >= 2);
    message(messages, 0, "system");
    assert(json_string(req, wire, member(element(messages, 0), "content"), decoded, sizeof(decoded)) > 0);
    for (i = 1; i < n; i++) {
        int at = element(messages, i), role = member(at, "role");
        assert(!is_text(role, "system"));
        assert(is_text(role, "user") || is_text(role, "assistant") || is_text(role, "tool"));
        if (is_text(role, "tool")) assert(json_string(req, wire, member(at, "tool_call_id"), decoded, sizeof(decoded)) > 0);
    }
    check_tool_schemas(member(0, "tools"));
    return length;
}
/* A handoff request carries the history, no tools, and asks for the summary last. */
static int pin_handoff(int length)
{
    int messages, n;
    if (length <= 0) return length;
    messages = parse_request(length, "model");
    n = length_of(messages);
    assert(n >= 3 && json_member(req, wire, 0, "tools") < 0 && json_member(req, wire, 0, "parallel_tool_calls") < 0);
    message(messages, 0, "system");
    assert(json_string(req, wire, member(element(messages, 0), "content"), decoded, sizeof(decoded)) > 0 &&
           strstr(decoded, "Summarize this conversation"));
    assert(is_text(member(message(messages, n - 1, "user"), "content"), "Write the handoff summary now."));
    return length;
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
static const char *reasoning_only =
    "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"role\":\"assistant\",\"content\":null,\"reasoning\":\"Let me think...\"}}],"
    "\"usage\":{\"prompt_tokens\":900,\"completion_tokens\":3072,\"cost\":0.01,"
    "\"completion_tokens_details\":{\"reasoning_tokens\":3072}}}";
static const char *cut_mid_call =
    "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"role\":\"assistant\",\"content\":\"Editing.\","
    "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}";
static const char *partial_text =
    "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"role\":\"assistant\",\"content\":\"Half an ans\"}}]}";
/* A reply that hits the output limit is discarded and the model is told, so the
 * run continues instead of stopping; nothing in a cut-off reply ever executes. */
static void truncation(void)
{
    size_t used;
    begin(); call("length"); used = a.used;
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(a.truncated && a.limited && a.active && !a.count && !a.next);
    assert(a.rounds == 1 && !a.tool_count && records == 2 && a.messages == 2);
    assert(a.used > used && !strstr(a.history, "get_environment") && strstr(a.history, "cut off"));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0 && strstr(req, "output token limit"));
    /* The retry is an ordinary round and clears the flag. */
    call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!a.truncated && !a.limited && a.count == 1 && a.rounds == 2);
    /* The logged failure: reasoning consumed the budget, so no call or text arrived. */
    begin(); used = a.used;
    assert(!agent_response(&a, reasoning_only, strlen(reasoning_only), 200, error, sizeof(error)));
    assert(a.truncated && a.active && !a.count && a.used > used && !strstr(a.history, "think"));
    assert(a.cost_seen && a.cost_micros == 10000 && a.context_tokens == 900);
    /* A call cut off mid-structure is the truncation, not a protocol error... */
    begin();
    assert(!agent_response(&a, cut_mid_call, strlen(cut_mid_call), 200, error, sizeof(error)));
    assert(a.truncated && !a.count && !*a.text && !strstr(a.history, "Editing"));
    /* ...but only at the limit: the same shape under "stop" is still rejected. */
    begin(); used = a.used;
    {
        char stopped[1024], *at = strstr(strcpy(stopped, cut_mid_call), "length");
        memcpy(at, "stop  ", 6);
        assert(agent_response(&a, stopped, strlen(stopped), 200, error, sizeof(error)) == -1);
        assert(!a.truncated && a.used == used);
    }
    /* Visible text with no call is a partial answer: shown, run ends. */
    begin();
    assert(!agent_response(&a, partial_text, strlen(partial_text), 200, error, sizeof(error)));
    assert(a.limited && !a.truncated && !a.active && !strcmp(a.text, "Half an ans"));
    /* A long complete reply, beyond the old 16 KiB, is kept whole. */
    {
        static char long_reply[AGENT_REPLY_CAP], body[AGENT_REPLY_CAP + 256];
        memset(long_reply, 'x', 30000); long_reply[30000] = 0;
        snprintf(body, sizeof(body), "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}]}", long_reply);
        begin();
        assert(!agent_response(&a, body, strlen(body), 200, error, sizeof(error)) && strlen(a.text) == 30000 && !a.active);
    }
    /* The notice must be journaled; if it cannot be, nothing advances. */
    begin(); call("length"); used = a.used; fail_record = 1;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(a.used == used && !a.truncated && !a.rounds && strstr(error, "retry notice"));
    fail_record = 0;
}
/* A reply whose tool calls cannot be accepted as sent is discarded with a
 * retry notice (like a cut-off reply) when smaller steps can fix it; every
 * other malformation stops the run with the check that failed named. */
static void big_call(char *out, size_t cap, size_t bytes)
{
    size_t at = (size_t)snprintf(out, cap, "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"write_text\","
        "\"arguments\":\"{\\\"text\\\":\\\"");
    memset(out + at, 'x', bytes); at += bytes;
    snprintf(out + at, cap - at, "\\\"}\"}}]}}]}");
}
static void rejections(void)
{
    static char big[AGENT_ARGUMENT_CAP + 512];
    size_t used;
    int i;
    char many[2048] = "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[";
    /* Arguments over the cap: nothing runs, the model is told to split the work. */
    begin(); used = a.used; big_call(big, sizeof(big), AGENT_ARGUMENT_CAP);
    assert(!agent_response(&a, big, strlen(big), 200, error, sizeof(error)));
    assert(a.discarded && strstr(a.discarded, "arguments") && !a.truncated && !a.limited);
    assert(a.active && !a.count && !a.next && a.rounds == 1 && !a.tool_count && records == 2);
    assert(a.used > used && !strstr(a.history, "c1") && strstr(a.history, "split"));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0 && strstr(req, "4096"));
    /* The retry is an ordinary round and clears the flag. */
    call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!a.discarded && a.count == 1 && a.rounds == 2);
    /* Just under the cap is accepted whole. */
    begin(); big_call(big, sizeof(big), AGENT_ARGUMENT_CAP - 64);
    assert(!agent_response(&a, big, strlen(big), 200, error, sizeof(error)) && !a.discarded && a.count == 1);
    /* More calls than one reply may carry. */
    for (i = 0; i <= AGENT_CALL_MAX; i++) {
        char one[160];
        snprintf(one, sizeof(one), "%s{\"id\":\"c%d\",\"type\":\"function\",\"function\":{\"name\":\"get_environment\",\"arguments\":\"{}\"}}", i ? "," : "", i);
        strcat(many, one);
    }
    strcat(many, "]}}]}");
    begin(); used = a.used;
    assert(!agent_response(&a, many, strlen(many), 200, error, sizeof(error)));
    assert(a.discarded && strstr(a.discarded, "calls") && !a.count && a.active && a.rounds == 1 && a.used > used);
    assert(!strstr(a.history, "c0"));
    /* At the output limit an oversized call is still the truncation itself. */
    begin(); big_call(big, sizeof(big), AGENT_ARGUMENT_CAP);
    memcpy(strstr(big, "\"tool_calls\""), "\"length\"    ", 12);
    assert(!agent_response(&a, big, strlen(big), 200, error, sizeof(error)) && a.truncated && !a.discarded);
    /* The notice must be journaled; if it cannot be, nothing advances. */
    begin(); big_call(big, sizeof(big), AGENT_ARGUMENT_CAP); used = a.used; fail_record = 1;
    assert(agent_response(&a, big, strlen(big), 200, error, sizeof(error)) == -1);
    assert(a.used == used && !a.discarded && !a.rounds && strstr(error, "notice"));
    fail_record = 0;
    /* Unrecoverable malformations name the failed check and change nothing. */
    {
        static const struct { const char *from, *to, *expect; } cases[] = {
            { "\"id\":\"c1\"", "\"id\":\"\"  ", "id" },
            { "\"name\":\"get_environment\"", "\"name\":\"\"               ", "name" },
            { "\"type\":\"function\",\"function\"", "\"type\":\"fnctn   \",\"function\"", "type" },
            { "\"arguments\":\"{}\"", "\"arguments\":null", "arguments" },
            { "\"role\":\"assistant\"", "\"role\":\"user\"     ", "role" },
            { "\"finish_reason\":\"tool_calls\"", "\"finish_reason\":\"content_filter\"", "finish_reason" },
        };
        for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
            char body[1024];
            begin(); used = a.used; call("tool_calls");
            snprintf(body, sizeof(body), "%s", response);
            {
                char *at = strstr(body, cases[i].from);
                assert(at && strlen(cases[i].from) <= strlen(cases[i].to) + 1);
                memmove(at + strlen(cases[i].to), at + strlen(cases[i].from), strlen(at + strlen(cases[i].from)) + 1);
                memcpy(at, cases[i].to, strlen(cases[i].to));
            }
            assert(agent_response(&a, body, strlen(body), 200, error, sizeof(error)) == -1);
            assert(a.used == used && !a.discarded && !a.count && strstr(error, cases[i].expect) && strstr(error, "HTTP 200"));
            assert(!strstr(error, "invalid, truncated, or unsupported"));
        }
    }
    {
        static char dup[1024];
        begin(); used = a.used;
        snprintf(dup, sizeof(dup), "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
            "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"get_environment\",\"arguments\":\"{}\"}},"
            "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"get_environment\",\"arguments\":\"{}\"}}]}}]}");
        assert(agent_response(&a, dup, strlen(dup), 200, error, sizeof(error)) == -1);
        assert(a.used == used && !a.discarded && strstr(error, "duplicate"));
    }
    /* Visible text beyond the reply buffer. */
    {
        static char long_reply[AGENT_REPLY_CAP + 16], body[AGENT_REPLY_CAP + 256];
        memset(long_reply, 'x', AGENT_REPLY_CAP); long_reply[AGENT_REPLY_CAP] = 0;
        snprintf(body, sizeof(body), "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}]}", long_reply);
        begin(); used = a.used;
        assert(agent_response(&a, body, strlen(body), 200, error, sizeof(error)) == -1);
        assert(a.used == used && strstr(error, "reply text"));
    }
}
/* OpenRouter rows list input modalities under architecture. A row without the
 * block (or with a malformed one) is unknown, never a guessed "no". */
static void vision_flag(void)
{
    static const char *page =
        "{\"data\":["
        "{\"id\":\"v/see\",\"name\":\"See\",\"context_length\":1000,"
        "\"architecture\":{\"modality\":\"text+image->text\",\"input_modalities\":[\"text\",\"image\",\"file\"],\"output_modalities\":[\"text\"]}},"
        "{\"id\":\"v/blind\",\"name\":\"Blind\",\"architecture\":{\"input_modalities\":[\"text\"]}},"
        "{\"id\":\"v/lookalike\",\"name\":\"Lookalike\",\"architecture\":{\"input_modalities\":[\"images\",\"text\"]}},"
        "{\"id\":\"v/none\",\"name\":\"None\"},"
        "{\"id\":\"v/odd\",\"name\":\"Odd\",\"architecture\":{\"input_modalities\":\"image\"}},"
        "{\"id\":\"v/empty\",\"name\":\"Empty\",\"architecture\":{\"input_modalities\":[]}}]}";
    static AgentModelRow rows[8];
    AgentModelInfo info;
    assert(agent_model_page(page, strlen(page), rows, 8) == 6);
    assert(rows[0].info.vision == AGENT_VISION_YES);
    assert(rows[1].info.vision == AGENT_VISION_NO);
    assert(rows[2].info.vision == AGENT_VISION_NO);
    assert(rows[3].info.vision == AGENT_VISION_UNKNOWN);
    assert(rows[4].info.vision == AGENT_VISION_UNKNOWN);
    assert(rows[5].info.vision == AGENT_VISION_NO);
    assert(!agent_model_info(page, strlen(page), "v/see", &info) && info.vision == AGENT_VISION_YES);
    assert(!agent_model_info(page, strlen(page), "v/none", &info) && info.vision == AGENT_VISION_UNKNOWN);
}
static unsigned char png_bytes[AGENT_IMAGE_CAP];
static void image_fixture(AgentImage *image, size_t length)
{
    size_t i;
    for (i = 0; i < length; i++) png_bytes[i] = (unsigned char)(i * 7 + 3);
    memset(image, 0, sizeof(*image));
    image->data = png_bytes; image->length = length;
    image->width = 390; image->height = 150;
    strcpy(image->path, "Apps:Putt:frame1.png");
}
/* Tool messages cannot carry pixels. The image rides in one user message that
 * follows the round's tool results, reaches the wire only for the request
 * right after it, and is journalled and summarized as a short note. */
static void images(void)
{
    static char encoded[AGENT_IMAGE_CAP * 2], expected[256];
    AgentImage image;
    const char *tool, *picture, *note;
    size_t used, before, i, free_image = 0, free_plain = 0;
    int parsed;
    image_fixture(&image, 100);
    begin(); named_call("tool_calls", "view_image", "{\"path\":\"Apps:Putt:frame1.png\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    /* Not before the round's results are in, and never an unusable image. */
    assert(agent_attach_image(&a, &image) == -1 && !a.image_pending);
    assert(!agent_tool_result(&a, "{\"status\":\"ok\"}", error, sizeof(error)));
    before = a.used;
    image.length = 0; assert(agent_attach_image(&a, &image) == -1);
    image.length = AGENT_IMAGE_CAP + 1; assert(agent_attach_image(&a, &image) == -1);
    assert(a.used == before && !a.image_pending);
    image_fixture(&image, 100); records = 0;
    assert(!agent_attach_image(&a, &image) && a.image_pending && records == 1);
    /* History and journal carry only the note: no pixels, no base64. */
    assert(strstr(a.history + before, "view_image: Apps:Putt:frame1.png (390x150 PNG, 100 bytes)"));
    assert(!strstr(a.history, "image_url") && !strstr(a.history, "base64"));
    assert(a.image_length == a.used - before - 1);
    /* The request after the attach splices the real image in place of the note. */
    parsed = pin(agent_request_image(&a, "model", &image, req, sizeof(req)));
    assert(parsed > 0);
    assert(base64_encode(png_bytes, 100, encoded, sizeof(encoded)) == BASE64_LENGTH(100));
    snprintf(expected, sizeof(expected), "\"url\":\"data:image/png;base64,%.8s", encoded);
    picture = strstr(req, "\"type\":\"image_url\"");
    assert(picture && strstr(req, expected) && strstr(picture, encoded));
    tool = strstr(req, "\"tool_call_id\":\"c1\"");
    note = strstr(req, "\"type\":\"text\",\"text\":\"view_image: Apps:Putt:frame1.png");
    assert(tool && note && tool < note && note < picture);
    assert(strstr(req, "\"role\":\"user\",\"content\":[{\"type\":\"text\""));
    {
        int messages = member(0, "messages"), count = length_of(messages);
        int content = member(message(messages, count - 1, "user"), "content"), text = element(content, 0), image_part = element(content, 1);
        assert(count == 5 && length_of(content) == 2);
        message(messages, count - 2, "tool");
        assert(is_text(member(text, "type"), "text") && json_string(req, wire, member(text, "text"), decoded, sizeof(decoded)) > 0);
        assert(!strncmp(decoded, "view_image: Apps:Putt:frame1.png", 32));
        assert(is_text(member(image_part, "type"), "image_url") &&
               json_string(req, wire, member(member(image_part, "image_url"), "url"), decoded, sizeof(decoded)) > 0);
        assert(!strncmp(decoded, "data:image/png;base64,", 22) && !strcmp(decoded + 22, encoded));
    }
    /* Only one image per request and it is the last message. */
    assert(!strstr(picture + 10, "\"type\":\"image_url\"") && strstr(picture, "\"}}]}],\"tools\":"));
    /* Without the pixels at hand the note is sent as plain text. */
    assert(pin(agent_request_image(&a, "model", NULL, req, sizeof(req))) > 0 && !strstr(req, "image_url"));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0 && !strstr(req, "image_url") && strstr(req, "frame1.png"));
    /* A request too small for the encoded image fails rather than truncating. */
    parsed = pin(agent_request(&a, "model", req, sizeof(req)));
    assert(parsed > 0 && agent_request_image(&a, "model", &image, req, (size_t)parsed + 10) == -1);
    /* Once the model has answered, later requests keep the note only. */
    assert(!agent_response(&a, "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"A green.\"}}]}",
                           strlen("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"A green.\"}}]}"),
                           200, error, sizeof(error)));
    assert(!a.image_pending);
    assert(pin_handoff(agent_handoff_request(&a, "model", req, sizeof(req))) > 0);
    assert(!strstr(req, "image_url") && !strstr(req, "base64") && strstr(req, "frame1.png"));
    assert(!agent_begin(&a, "And now?", error, sizeof(error)));
    assert(pin(agent_request_image(&a, "model", &image, req, sizeof(req))) > 0 && !strstr(req, "image_url") && strstr(req, "frame1.png"));
    /* Stop and a fresh user turn both drop a pending image. */
    begin(); named_call("tool_calls", "view_image", "{\"path\":\"x\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "{\"status\":\"ok\"}", error, sizeof(error)));
    assert(!agent_attach_image(&a, &image) && a.image_pending);
    assert(!agent_stop(&a, "user stop") && !a.image_pending);
    /* An image the request cannot hold is dropped with an explicit note. */
    begin(); named_call("tool_calls", "view_image", "{\"path\":\"x\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "{\"status\":\"ok\"}", error, sizeof(error)));
    image_fixture(&image, AGENT_IMAGE_CAP);
    a.used = CHAT_REQUEST_CAP - (size_t)BASE64_LENGTH(AGENT_IMAGE_CAP) - 8192;
    assert(a.used + 1024 < sizeof(a.history));
    used = a.used;
    assert(!agent_attach_image(&a, &image) && !a.image_pending);
    assert(a.used > used && strstr(a.history + used, "NOT attached") && strstr(a.history + used, "frame1.png"));
    /* A call to view_image reserves room for its note before anything runs:
     * find the fullest history each kind of call still fits in. */
    for (i = 0; i < 2; i++) {
        size_t step = 16, at = sizeof(a.history) - 4 * AGENT_RESULT_WIRE_CAP;
        begin();
        if (i) named_call("tool_calls", "view_image", "{\"path\":\"Apps:Putt:frame1.png\"}"); else call("tool_calls");
        saved = a;
        for (;; at += step) {
            a = saved; a.used = at;
            if (agent_response(&a, response, strlen(response), 200, error, sizeof(error))) break;
            assert(at < sizeof(a.history));
        }
        if (i) free_image = at; else free_plain = at;
    }
    assert(free_plain > free_image && free_plain - free_image >= AGENT_IMAGE_NOTE_CAP);
    a = saved;
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
/* The model chooser lists a whole page, filters it locally and resolves a
 * typed id only by exact match. */
static void model_page(void)
{
    static const char *page =
        "{\"total_count\":3,\"data\":["
        "{\"id\":\"openai/gpt-4o-pro\",\"name\":\"GPT-4o Pro\",\"context_length\":262144},"
        "{\"id\":\"openai/gpt-4o\",\"name\":\"GPT-4o\",\"context_length\":131072,"
        "\"reasoning\":{\"mandatory\":true,\"default_enabled\":false,\"default_effort\":\"medium\","
        "\"supported_efforts\":[\"high\",\"medium\",\"low\",\"minimal\",\"none\"]}},"
        "{\"name\":\"No id\",\"context_length\":1},"
        "{\"id\":\"\",\"name\":\"Empty id\",\"context_length\":2},"
        "{\"id\":\"z-ai/glm\",\"name\":\"GLM\",\"context_length\":8192}]}";
    static char big[65536], query[64];
    AgentModelRow rows[AGENT_MODEL_ROWS_MAX];
    size_t i, at = 0;
    int count;
    /* The popular page the dialog loads on open is pinned. */
    assert(agent_popular_query(query, sizeof(query)) ==
           (int)strlen("/api/v1/models?limit=10&sort=most-popular"));
    assert(!strcmp(query, "/api/v1/models?limit=10&sort=most-popular"));
    assert(agent_popular_query(query, 8) == -1);
    /* Rows keep page order and carry id, name, context and reasoning; rows
     * without a usable id are skipped, not counted. */
    count = agent_model_page(page, strlen(page), rows, AGENT_MODEL_ROWS_MAX);
    assert(count == 3);
    assert(!strcmp(rows[0].id, "openai/gpt-4o-pro") && !strcmp(rows[0].info.name, "GPT-4o Pro"));
    assert(rows[0].info.context_length == 262144 && !rows[0].info.reasoning);
    assert(!strcmp(rows[1].id, "openai/gpt-4o") && !strcmp(rows[1].info.name, "GPT-4o"));
    assert(rows[1].info.reasoning && rows[1].info.mandatory && rows[1].info.effort_count == 5);
    assert(!strcmp(rows[1].info.default_effort, "medium"));
    assert(!strcmp(rows[1].info.supported_efforts[0], "high") &&
           !strcmp(rows[1].info.supported_efforts[4], "none"));
    assert(!strcmp(rows[2].id, "z-ai/glm") && rows[2].info.context_length == 8192);
    /* Typing filters locally: empty matches all, id and name match
     * case-insensitively, an unrelated query matches nothing. */
    assert(agent_model_row_match(&rows[0], ""));
    assert(agent_model_row_match(&rows[0], "gpt"));
    assert(agent_model_row_match(&rows[0], "4O-PRO"));
    assert(agent_model_row_match(&rows[0], "pro"));
    assert(!agent_model_row_match(&rows[0], "glm"));
    assert(agent_model_row_match(&rows[2], "z-ai"));
    assert(!agent_model_row_match(&rows[2], "gpt"));
    /* Only an exact id resolves a typed model; substrings do not. */
    assert(agent_model_row_find(rows, 3, "openai/gpt-4o") == 1);
    assert(agent_model_row_find(rows, 3, "openai/gpt-4o-pro") == 0);
    assert(agent_model_row_find(rows, 3, "gpt-4o") == -1);
    assert(agent_model_row_find(rows, 3, "openai/gpt-4o-mini") == -1);
    assert(agent_model_row_find(rows, 3, "") == -1);
    assert(agent_model_row_find(rows, 0, "openai/gpt-4o") == -1);
    /* The display cap trims a longer page; a zero cap reads nothing. */
    assert(agent_model_page(page, strlen(page), rows, 2) == 2);
    assert(!strcmp(rows[1].id, "openai/gpt-4o"));
    assert(agent_model_page(page, strlen(page), rows, 0) == 0);
    /* Malformed, empty and truncated pages read as no rows. */
    assert(agent_model_page("not json", strlen("not json"), rows, 10) == 0);
    assert(agent_model_page("{\"data\":{}}", strlen("{\"data\":{}}"), rows, 10) == 0);
    assert(agent_model_page("{\"data\":[]}", strlen("{\"data\":[]}"), rows, 10) == 0);
    for (i = 1; i < strlen(page); i++) assert(agent_model_page(page, i, rows, 10) == 0);
    /* More rows than the shared parser cap reads as no rows, never partial. */
    at += (size_t)snprintf(big + at, sizeof(big) - at, "{\"data\":[");
    for (i = 0; i < 1500; i++)
        at += (size_t)snprintf(big + at, sizeof(big) - at, "%s{\"id\":\"m%lu\",\"context_length\":1024}",
                               i ? "," : "", (unsigned long)i);
    at += (size_t)snprintf(big + at, sizeof(big) - at, "]}");
    assert(at < sizeof(big));
    assert(agent_model_page(big, at, rows, 10) == 0);
}
/* AGENTS.md: the workspace-root text rides in the system message of every
 * model request; a project's text follows the round that first touched it. */
static void instructions(void)
{
    static char text[AGENT_INSTRUCTIONS_CAP + 8];
    const char *rules = "Prefer tabs over spaces.\nKeep functions short.", *system, *first, *tool, *note;
    int baseline, with;
    char name[AGENT_PROJECT_NAME_CAP + 8];
    int i;
    begin();
    agent_set_instructions(NULL);
    baseline = pin(agent_request(&a, "model", req, sizeof(req)));
    assert(baseline > 0 && !strstr(req, "Prefer tabs") && !strstr(req, "Workspace instructions"));
    /* Set text lands inside the one system message, JSON-escaped, before any history. */
    assert(agent_set_instructions(rules) == 0);
    with = pin(agent_request(&a, "model", req, sizeof(req)));
    assert(with > baseline);
    system = strstr(req, "\"role\":\"system\""); first = strstr(req, "Inspect my files");
    assert(system && first && strstr(req, "Workspace instructions") && strstr(req, "Prefer tabs over spaces.\\u000aKeep functions short."));
    assert(strstr(req, "Prefer tabs") > system && strstr(req, "Prefer tabs") < first && strstr(req, "\"tools\":"));
    { int systems = 0; const char *p = req; while ((p = strstr(p, "\"role\":\"system\""))) { systems++; p++; } assert(systems == 1); }
    /* Never in history or the journal, so a handoff summary request does not carry it. */
    assert(!strstr(a.history, "Prefer tabs"));
    assert(agent_request(&a, "model", req, with - 1) == -1);
    /* It survives New Chat (agent_reset) and handoff; the app reloads it per chat. */
    begin();
    assert(pin(agent_request(&a, "model", req, sizeof(req))) == with);
    assert(!agent_stop(&a, "done") && !a.active);
    assert(pin_handoff(agent_handoff_request(&a, "model", req, sizeof(req))) > 0 && !strstr(req, "Prefer tabs"));
    /* Empty and NULL clear; an over-long text is refused and leaves the old one. */
    memset(text, 'x', AGENT_INSTRUCTIONS_CAP); text[AGENT_INSTRUCTIONS_CAP] = 0;
    assert(agent_set_instructions(text) == 0);
    text[AGENT_INSTRUCTIONS_CAP] = 'x'; text[AGENT_INSTRUCTIONS_CAP + 1] = 0;
    assert(agent_set_instructions(text) == -1);
    begin(); assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0 && strstr(req, "xxxxxxxx") && !strstr(req, "Prefer tabs"));
    assert(agent_set_instructions("") == 0);
    assert(pin(agent_request(&a, "model", req, sizeof(req))) == baseline);
    /* The policy says what the file may not do. */
    assert(strstr(req, "cannot override these rules") && strstr(req, "AGENTS.md"));
    /* ...and tells the model to keep an existing file current without inventing one. */
    assert(strstr(req, "update it with edit_text") && strstr(req, "do not create one unless asked"));

    /* Project text: recorded once, only after the round's tool results, as a user message. */
    begin(); named_call("tool_calls", "read_text", "{\"path\":\"Putt:main.c\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(agent_project_seen(&a, "Putt") == 0);
    assert(agent_project_note(&a, "Putt", "Use Dialog Manager.") == -1 && agent_project_seen(&a, "Putt") == 0);
    assert(!agent_tool_result(&a, "{\"status\":\"ok\"}", error, sizeof(error)));
    records = 0;
    assert(agent_project_note(&a, "Putt", "Use Dialog Manager.\nNo globals.") == 0 && records == 1);
    assert(!strcmp(last_event, "project_instructions") && agent_project_seen(&a, "Putt") == 1);
    assert(agent_project_note(&a, "Putt", "Other text.") == 0 && records == 1 && !strstr(a.history, "Other text."));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0);
    tool = strstr(req, "\"tool_call_id\":\"c1\""); note = strstr(req, "Putt:AGENTS.md");
    assert(tool && note && tool < note && strstr(req, "Use Dialog Manager.\\u000aNo globals."));
    assert(strstr(req, "{\"role\":\"user\",\"content\":\"Project instructions from Putt:AGENTS.md"));
    assert(strstr(req, "outrank") && !strstr(req, "Other text."));
    /* A project with no file is remembered without a history message. */
    records = 0;
    assert(agent_project_note(&a, "Bare", NULL) == 0 && records == 0 && agent_project_seen(&a, "Bare") == 1);
    assert(agent_project_note(&a, "Bare2", "") == 0 && records == 0 && agent_project_seen(&a, "Bare2") == 1);
    /* A failed recording leaves the project unmarked so the caller stops the run. */
    fail_record = 1;
    assert(agent_project_note(&a, "Later", "More rules.") == -1 && agent_project_seen(&a, "Later") == 0);
    fail_record = 0;
    /* The summary sees project text (it is history); a new chat or handoff forgets delivery. */
    assert(strstr(a.history, "Use Dialog Manager."));
    begin(); assert(agent_project_seen(&a, "Putt") == 0);
    /* Eight projects are tracked; more are reported as seen so none is re-read each round. */
    begin(); named_call("tool_calls", "read_text", "{\"path\":\"P0:x\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "ok", error, sizeof(error)));
    for (i = 0; i < AGENT_PROJECT_MAX; i++) {
        snprintf(name, sizeof(name), "P%d", i);
        assert(agent_project_seen(&a, name) == 0 && agent_project_note(&a, name, NULL) == 0 && agent_project_seen(&a, name) == 1);
    }
    assert(agent_project_seen(&a, "Ninth") == 1 && agent_project_note(&a, "Ninth", "Rules.") == 0 && !strstr(a.history, "Ninth"));
    memset(name, 'n', sizeof(name) - 1); name[sizeof(name) - 1] = 0;
    begin(); named_call("tool_calls", "read_text", "{\"path\":\"P0:x\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "ok", error, sizeof(error)));
    assert(agent_project_seen(&a, name) == 1 && agent_project_seen(&a, "") == 1);
}
/* One tool round, read back from the parsed wire body: the literal model, the
 * roles in order, the call id echoed by the tool result, and the schemas. */
static void wire_shape(void)
{
    const char *final = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Inspected.\"}}]}";
    int messages, assistant, tool_calls, function;
    begin(); call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!agent_tool_result(&a, "{\"status\":\"ok\",\"text\":\"caf\xc3\xa9\"}", error, sizeof(error)));
    messages = parse_request(agent_request(&a, "vendor/other-model", req, sizeof(req)), "vendor/other-model");
    check_tool_schemas(member(0, "tools"));
    assert(length_of(messages) == 4);
    assert(json_string(req, wire, member(message(messages, 0, "system"), "content"), decoded, sizeof(decoded)) > 0 && *decoded);
    assert(is_text(member(message(messages, 1, "user"), "content"), "Inspect my files"));
    /* The assistant turn is sent back as received: no text, one call with its id. */
    assistant = message(messages, 2, "assistant");
    assert(is_primitive(member(assistant, "content"), "null"));
    tool_calls = member(assistant, "tool_calls");
    assert(length_of(tool_calls) == 1);
    function = member(element(tool_calls, 0), "function");
    assert(is_text(member(element(tool_calls, 0), "id"), "c1") && is_text(member(function, "name"), "get_environment"));
    assert(is_text(member(function, "arguments"), "{}"));
    /* The result answers that id as a tool message, never a user one. */
    assert(is_text(member(message(messages, 3, "tool"), "tool_call_id"), "c1"));
    assert(is_text(member(element(messages, 3), "content"), "{\"status\":\"ok\",\"text\":\"caf\xc3\xa9\"}"));
    /* The next turn follows the answer, in order. */
    assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
    assert(!agent_begin(&a, "What did you find?", error, sizeof(error)));
    messages = parse_request(agent_request(&a, "model", req, sizeof(req)), "model");
    assert(length_of(messages) == 6);
    message(messages, 0, "system"); message(messages, 1, "user"); message(messages, 2, "assistant");
    message(messages, 3, "tool"); message(messages, 4, "assistant"); message(messages, 5, "user");
    assert(is_text(member(element(messages, 4), "content"), "Inspected."));
    assert(is_text(member(element(messages, 5), "content"), "What did you find?"));
    /* A handoff request replays that history without tools, then asks for the summary. */
    assert(!agent_stop(&a, "done"));
    messages = parse_request(agent_handoff_request(&a, "vendor/other-model", req, sizeof(req)), "vendor/other-model");
    assert(length_of(messages) == 7 && json_member(req, wire, 0, "tools") < 0);
    message(messages, 0, "system"); message(messages, 3, "tool");
    assert(is_text(member(message(messages, 6, "user"), "content"), "Write the handoff summary now."));
}
int main(void)
{
    const char *final = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Inspected.\"}}]}";
    size_t used, i;
    instructions();
    wire_shape();
    begin();
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0);
    assert(strstr(req, "\"tools\"") && strstr(req, "\"role\":\"system\"") && strstr(req, "write_text") && strstr(req, "create_project") && strstr(req, "create_folder") && strstr(req, "edit_text") && strstr(req, "search_text") && strstr(req,"build_project") && strstr(req,"read_build_log") && strstr(req,"view_image") && strstr(req,"at most 131072 bytes;") && !strstr(req,"131072L"));
    call("tool_calls");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(a.count == 1 && !a.next && a.active && records == 2);
    assert(!strcmp(a.calls[0].id, "c1") && !strcmp(a.calls[0].arguments, "{}"));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) == -1);
    assert(!agent_tool_result(&a, "{\"status\":\"ok\",\"text\":\"caf\xc3\xa9\"}", error, sizeof(error)));
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0);
    assert(strstr(req, "\"tool_call_id\":\"c1\"") && strstr(req, "caf\xc3\xa9"));
    assert(!agent_response(&a, final, strlen(final), 200, error, sizeof(error)));
    assert(!a.active && a.messages == 4 && records == 4);
    assert(!agent_begin(&a, "What did you find?", error, sizeof(error)));
    assert(strstr(a.history, "Inspected."));
    used = a.used;
    assert(agent_response(&a, "{}", 2, 503, error, sizeof(error)) == -1 && a.used == used);
    /* A well-formed completion is still rejected on a non-2xx status... */
    assert(agent_response(&a, final, strlen(final), 503, error, sizeof(error)) == -1);
    assert(strstr(error, "503") && a.used == used);
    /* ...and an "error" object is rejected on a 2xx status, message preserved. */
    assert(agent_response(&a, "{\"error\":{\"code\":429,\"message\":\"Upstream rate limited\"}}",
                          strlen("{\"error\":{\"code\":429,\"message\":\"Upstream rate limited\"}}"),
                          200, error, sizeof(error)) == -1);
    assert(strstr(error, "200") && strstr(error, "Upstream rate limited") && a.used == used);
    assert(!agent_stop(&a, "network failed") && a.used == used);
    truncation();
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
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)) && !a.count && a.truncated);
    assert(!strstr(a.history, "new.c") && !strstr(a.history, "\"tool_calls\""));
    named_call("tool_calls", "write_text", "{\"path\":\"new.c\",\"text\":\"source\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)));
    assert(!strcmp(a.calls[0].name, "write_text") && !agent_stop(&a, "Stop before create"));
    assert(a.next == a.count && strstr(a.history, "interrupted") && !a.active);
    begin(); named_call("length", "edit_text", "{\"path\":\"old.c\",\"expected_revision\":\"full-x\",\"old_text\":\"a\",\"new_text\":\"b\"}");
    assert(!agent_response(&a, response, strlen(response), 200, error, sizeof(error)) && !a.count && a.truncated);
    assert(!strstr(a.history, "full-x") && !strstr(a.history, "\"tool_calls\""));
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
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > AGENT_HISTORY_CAP);
    assert(strstr(req, "\"tools\"") && strstr(req, "read_build_log"));
    call("tool_calls"); used = a.used;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(a.used == used && !a.count && strstr(error, "History capacity"));
    /* Handoff does not append to a full history or use tools. It is allowed
     * only between runs, and failed candidate persistence cannot clear it. */
    assert(!agent_stop(&a, "history full"));
    saved = a;
    assert(pin_handoff(agent_handoff_request(&a, "model", req, sizeof(req))) > AGENT_HISTORY_CAP);
    assert(!strstr(req, "\"tools\":") && strstr(req, "Write the handoff summary now"));
    {
        JsonToken request_tokens[4096];
        long max_tokens;
        assert(json_parse(req, strlen(req), request_tokens, 4096) > 0);
        assert(!json_integer(req, request_tokens,
            json_member(req, request_tokens, 0, "max_tokens"), &max_tokens));
        assert(max_tokens == 6000);
    }
    assert(!memcmp(&a, &saved, sizeof(a)));
    assert(!agent_handoff_response(final, strlen(final), 200, req, sizeof(req), error, sizeof(error)));
    assert(!strcmp(req, "Inspected."));
    assert(agent_handoff_response(final, strlen(final), 500, req, sizeof(req), error, sizeof(error)) == -1);
    call("tool_calls");
    assert(agent_handoff_response(response, strlen(response), 200, req, sizeof(req), error, sizeof(error)) == -1);
    call("length");
    assert(agent_handoff_response(response, strlen(response), 200, req, sizeof(req), error, sizeof(error)) == -1);
    assert(!*req && strstr(error, "output token limit") && strstr(error, "Conversation retained"));
    assert(agent_handoff_response(reasoning_only, strlen(reasoning_only), 200, req, sizeof(req), error, sizeof(error)) == -1);
    assert(!*req && strstr(error, "output token limit") && !memcmp(&a, &saved, sizeof(a)));
    for (i = 0; i < strlen(final); i++)
        assert(agent_handoff_response(final, i, 200, req, sizeof(req), error, sizeof(error)) == -1);
    agent_reset(&candidate, journal, NULL); fail_record = 1;
    assert(agent_handoff_seed(&candidate, "# Goal\nBuild a prototype.", "old.jsonl", "handoff.md") == -1);
    assert(!candidate.messages && !candidate.used && !memcmp(&a, &saved, sizeof(a)));
    fail_record = 0;
    assert(!agent_handoff_seed(&candidate, "# Goal\nBuild a prototype.", "old.jsonl", "handoff.md"));
    assert(candidate.messages == 1 && !candidate.active && strstr(candidate.history, "old.jsonl") && strstr(candidate.history, "handoff.md"));
    assert(!agent_begin(&candidate, "Continue", error, sizeof(error)));
    assert(pin_handoff(agent_handoff_request(&candidate, "model", req, sizeof(req))) == -1);
    assert(pin(agent_request(&candidate, "model", req, sizeof(req))) > 0 && strstr(req, "Build a prototype"));
    begin(); call("tool_calls");
    a.used = sizeof(a.history) - 10;
    assert(agent_response(&a, response, strlen(response), 200, error, sizeof(error)) == -1);
    assert(text_to_macroman_strict("\xf0\x9f\xa6\x80", req, sizeof(req)) == -1);
    assert(text_to_macroman_strict("caf\xc3\xa9", req, sizeof(req)) == 4);
    usage();
    {
        long completion, reasoning;
        const char *with_details = "{\"usage\":{\"completion_tokens\":1685,\"completion_tokens_details\":{\"reasoning_tokens\":1500}}}";
        const char *plain = "{\"usage\":{\"completion_tokens\":30}}";
        assert(!agent_usage_completion(with_details, strlen(with_details), &completion, &reasoning) && completion == 1685 && reasoning == 1500);
        assert(!agent_usage_completion(plain, strlen(plain), &completion, &reasoning) && completion == 30 && reasoning == -1);
        assert(agent_usage_completion("{\"usage\":{}}", 13, &completion, &reasoning) == -1 && completion == -1);
        assert(agent_usage_completion("{}", 2, &completion, &reasoning) == -1);
        assert(agent_usage_completion("nope", 4, &completion, &reasoning) == -1);
    }
    /* The wire request carries the named cap, so docs and code share one value. */
    begin();
    assert(pin(agent_request(&a, "model", req, sizeof(req))) > 0 && strstr(req, "\"max_tokens\":6000,"));
    rejections();
    model_metadata();
    vision_flag();
    images();
    model_page();
    puts("PASS agent tools, usage accounting, history, truncation, Stop, persistence barriers, bounds and model metadata");
    return 0;
}
