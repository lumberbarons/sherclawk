#include "mcp.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    W_PARSE, W_UNIQUE, W_ROOT, W_ROOT_PICKED, W_PICK, W_DECODE, W_REJECT, W_DONE,
    W_JSONRPC_DECODED, W_METHOD, W_REQUEST, W_RESPONSE, W_ERROR_PICKED, W_ERROR_DECODED,
    I_RESULT, I_PICKED, I_VERSION_DECODED, I_INFO_PICKED, I_NAME_DECODED, I_REVISION_DECODED, I_CAPS_PICKED,
    D_START, D_PICKED, D_CURSOR_DECODED, D_TOOL, D_FINISH,
    T_PICKED, T_NAME_DECODED, T_NAME_LONG, T_DUP, T_CHECK, T_ANNOTATION_PICKED, T_ELIGIBLE,
    T_DESCRIPTION_BAD, T_DESCRIPTION_DECODED, T_REDACT, T_QUOTE, T_COMPOSE, T_COPY,
    C_RESULT, C_HINT
};
static const char *const root_keys[] = {"jsonrpc", "id", "method", "result", "error"};
static const char *const error_keys[] = {"code", "message"};
static const char *const result_keys[] = {"protocolVersion", "serverInfo", "capabilities"};
static const char *const info_keys[] = {"name", "version"};
static const char *const tools_keys[] = {"tools"};
static const char *const page_keys[] = {"tools", "nextCursor"};
static const char *const tool_keys[] = {"name", "annotations", "inputSchema", "description"};
static const char *const annotation_keys[] = {"readOnlyHint"};
static const char *const call_keys[] = {"isError"};
enum { ROOT_JSONRPC, ROOT_ID, ROOT_METHOD, ROOT_RESULT, ROOT_ERROR };
#define COPY_CHUNK 256
#define DUP_COST 64      /* one strcmp of two names under 64 bytes */
#define CHECK_COST 768   /* name validation, mapping and the selection list */

static int signed_integer(const char *s, const JsonToken *t, int index)
{
    int i = t[index].start, end = t[index].end;
    unsigned long n = 0;
    if (t[index].type != JSON_PRIMITIVE) return 0;
    if (s[i] == '-') i++;
    if (i == end) return 0;
    for (; i < end; i++) {
        unsigned long digit;
        if (s[i] < '0' || s[i] > '9') return 0;
        digit = (unsigned long)(s[i] - '0');
        if (n > (2147483648UL - digit) / 10UL) return 0;
        n = n * 10UL + digit;
    }
    return n <= (s[t[index].start] == '-' ? 2147483648UL : 2147483647UL);
}
static int name_valid(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
static void discard(McpRegistry *r, const char *reason)
{
    r->count = 0; r->schema_len = 0; r->schemas[0] = 0; r->cursor[0] = 0;
    snprintf(r->notice, sizeof(r->notice), "MCP unavailable: %s.", reason);
}
static size_t room(size_t work, size_t budget) { return work >= budget ? 0 : budget - work; }
/* Room for an atom of `need` units and the unit every transition costs. */
static int fits(size_t work, size_t budget, size_t need) { return !work || work + need + 1 <= budget; }

void mcp_work_init(McpWork *w, int stages, const McpConfig *config, McpRegistry *registry,
                   const char *text, size_t len, long expected_id,
                   char *reply, size_t reply_cap, char *version, size_t version_cap)
{
    /* The big buffers at the end of the struct are written before they are read. */
    memset(w, 0, offsetof(McpWork, description));
    w->stages = stages; w->active = stages & MCP_STAGE_CLASSIFY ? MCP_STAGE_CLASSIFY : stages;
    w->config = config; w->registry = registry;
    w->text = text; w->len = len; w->expected = expected_id;
    w->reply = reply; w->reply_cap = reply_cap; w->version = version; w->version_cap = version_cap;
    if (reply_cap) reply[0] = 0;
    w->phase = W_PARSE;
    json_parser_init(&w->parser, text, len > MCP_MESSAGE_CAP ? 0 : len, w->tokens, 8192);
}
/* The current stage refuses the message. A refused discovery also forgets
 * what earlier pages registered. */
static int reject(McpWork *w, const char *reason)
{
    if (w->active == MCP_STAGE_CLASSIFY) w->rpc = -1;
    else {
        w->status = -1;
        if (w->active == MCP_STAGE_DISCOVER) discard(w->registry, reason);
    }
    w->phase = W_DONE;
    return 0;
}
static void begin_pick(McpWork *w, int object, const char *const *keys, int count, int next)
{
    json_pick_init(&w->pick, w->text, w->tokens, object, keys, count);
    w->phase = W_PICK; w->next = next;
}
static void begin_decode(McpWork *w, int token, char *out, size_t cap, int next, int fail, const char *reason)
{
    json_decode_init(&w->decode, w->text, w->tokens, token, out, cap);
    w->phase = W_DECODE; w->next = next; w->fail_phase = fail; w->fail_reason = reason;
}
static int object_at(const McpWork *w, int index)
{
    return index >= 0 && w->tokens[index].type == JSON_OBJECT;
}
static int true_at(const McpWork *w, int index)
{
    return index >= 0 && w->tokens[index].type == JSON_PRIMITIVE &&
        w->tokens[index].end - w->tokens[index].start == 4 &&
        !memcmp(w->text + w->tokens[index].start, "true", 4);
}
/* Classification is over: run the requested operation on the matching
 * response, and nothing on any other message. */
static int operation(McpWork *w)
{
    int ops = w->stages & ~MCP_STAGE_CLASSIFY;
    w->active = ops;
    if ((w->stages & MCP_STAGE_CLASSIFY) && w->rpc != 1) ops = 0;
    w->phase = ops & MCP_STAGE_INITIALIZE ? I_RESULT : ops & MCP_STAGE_DISCOVER ? D_START :
        ops & MCP_STAGE_CALL ? C_RESULT : W_DONE;
    return 0;
}
static int skip(McpWork *w, const char *notice)
{
    if (notice) strcpy(w->registry->notice, notice);
    w->tool = w->tokens[w->tool].next;
    w->phase = D_TOOL;
    return 0;
}
static const char *copy_piece(McpWork *w, int piece, size_t *length)
{
    switch (piece) {
    case 0: *length = w->prefix_len; return w->prefix;
    case 1: *length = w->quoted_used; return w->quoted;
    case 2: *length = 14; return ",\"parameters\":";
    case 3: *length = (size_t)(w->tokens[w->held[2]].end - w->tokens[w->held[2]].start);
            return w->text + w->tokens[w->held[2]].start;
    default: *length = 2; return "}}";
    }
}
/* One unit of progress for the current phase. Returns 1 when the budget has
 * no room left and the work must resume in the next step. */
static int advance_phase(McpWork *w, size_t *work, size_t budget)
{
    McpRegistry *r = w->registry;
    const McpConfig *c = w->config;
    const char *s = w->text;
    const JsonToken *t = w->tokens;
    int *root = w->root, i, n;
    size_t amount;
    if (!fits(*work, budget, JSON_ATOM_MAX)) return 1;
    switch (w->phase) {
    case W_PARSE:
        i = json_parser_step(&w->parser, room(*work, budget));
        *work += w->parser.work;
        if (w->len > MCP_MESSAGE_CAP || i < 0 || (i > 0 && (w->parser.used < 1 || t[0].type != JSON_OBJECT)))
            return reject(w, "malformed discovery");
        if (!i) return 1;
        json_unique_init(&w->unique, s, t, w->parser.used, w->slots);
        w->phase = W_UNIQUE;
        return 0;
    case W_UNIQUE:
        i = json_unique_step(&w->unique, room(*work, budget));
        *work += w->unique.work;
        if (i < 0) return reject(w, "malformed discovery"); /* ambiguous keys are protocol failures */
        if (!i) return 1;
        begin_pick(w, 0, root_keys, 5, W_ROOT_PICKED);
        return 0;
    case W_PICK:
        if (!fits(*work, budget, JSON_PICK_NEED)) return 1;
        i = json_pick_step(&w->pick, room(*work, budget));
        *work += w->pick.work;
        if (!i) return 1;
        w->phase = w->next;
        return 0;
    case W_DECODE:
        i = json_decode_step(&w->decode, room(*work, budget));
        *work += w->decode.work;
        if (i < 0) {
            if (w->fail_phase == W_REJECT) return reject(w, w->fail_reason);
            w->phase = w->fail_phase;
            return 0;
        }
        if (!i) return 1;
        w->phase = w->next;
        return 0;
    case W_ROOT_PICKED:
        memcpy(root, w->pick.found, sizeof(w->root));
        if (!(w->stages & MCP_STAGE_CLASSIFY)) return operation(w);
        if (root[ROOT_JSONRPC] < 0) return reject(w, "malformed discovery");
        begin_decode(w, root[ROOT_JSONRPC], w->scratch, 16, W_JSONRPC_DECODED, W_REJECT, "malformed discovery");
        return 0;
    case W_JSONRPC_DECODED:
        if (strcmp(w->scratch, "2.0")) return reject(w, "malformed discovery");
        w->phase = W_METHOD;
        return 0;
    case W_METHOD:
        if (root[ROOT_METHOD] < 0) { w->phase = W_RESPONSE; return 0; }
        if (root[ROOT_RESULT] >= 0 || root[ROOT_ERROR] >= 0) return reject(w, "malformed discovery");
        begin_decode(w, root[ROOT_METHOD], w->scratch, 128, W_REQUEST, W_REJECT, "malformed discovery");
        return 0;
    case W_REQUEST:
        i = root[ROOT_ID];
        if (i < 0) { w->rpc = 0; w->phase = W_DONE; return 0; } /* notification */
        if (t[i].type != JSON_STRING && !signed_integer(s, t, i)) return reject(w, "malformed discovery");
        /* The id is echoed verbatim; one that cannot fit is refused unread. */
        if (!w->reply_cap || (size_t)(t[i].end - t[i].start) >= w->reply_cap) return reject(w, "malformed discovery");
        n = snprintf(w->reply, w->reply_cap, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,%s}",
            t[i].end - t[i].start, s + t[i].start,
            !strcmp(w->scratch, "ping") ? "\"result\":{}" : "\"error\":{\"code\":-32601,\"message\":\"Unsupported server request\"}");
        if (n < 0 || (size_t)n >= w->reply_cap) return reject(w, "malformed discovery");
        *work += (size_t)n;
        w->rpc = 2; w->phase = W_DONE;
        return 0;
    case W_RESPONSE: {
        long number;
        i = root[ROOT_ID];
        if (i < 0 || json_integer(s, t, i, &number) || number != w->expected ||
            ((root[ROOT_RESULT] >= 0) == (root[ROOT_ERROR] >= 0))) return reject(w, "malformed discovery");
        if (root[ROOT_ERROR] < 0) { w->rpc = 1; return operation(w); }
        begin_pick(w, root[ROOT_ERROR], error_keys, 2, W_ERROR_PICKED);
        return 0;
    }
    case W_ERROR_PICKED:
        if (w->pick.found[0] < 0 || !signed_integer(s, t, w->pick.found[0])) return reject(w, "malformed discovery");
        begin_decode(w, w->pick.found[1], w->scratch, 1024, W_ERROR_DECODED, W_REJECT, "malformed discovery");
        return 0;
    case W_ERROR_DECODED:
        w->rpc = 1;
        return operation(w);

    case I_RESULT:
        if (!object_at(w, root[ROOT_RESULT])) return reject(w, "initialize");
        begin_pick(w, root[ROOT_RESULT], result_keys, 3, I_PICKED);
        return 0;
    case I_PICKED:
        memcpy(w->held, w->pick.found, sizeof(w->held));
        begin_decode(w, w->held[0], w->version, w->version_cap, I_VERSION_DECODED, W_REJECT, "initialize");
        return 0;
    case I_VERSION_DECODED:
        if (strcmp(w->version, "2025-11-25") && strcmp(w->version, "2025-06-18") && strcmp(w->version, "2025-03-26"))
            return reject(w, "initialize");
        if (!object_at(w, w->held[1])) return reject(w, "initialize");
        begin_pick(w, w->held[1], info_keys, 2, I_INFO_PICKED);
        return 0;
    case I_INFO_PICKED:
        w->held[3] = w->pick.found[1];
        begin_decode(w, w->pick.found[0], w->scratch, 256, I_NAME_DECODED, W_REJECT, "initialize");
        return 0;
    case I_NAME_DECODED:
        if (!w->scratch[0]) return reject(w, "initialize");
        begin_decode(w, w->held[3], w->scratch, 256, I_REVISION_DECODED, W_REJECT, "initialize");
        return 0;
    case I_REVISION_DECODED:
        if (!object_at(w, w->held[2])) return reject(w, "initialize");
        begin_pick(w, w->held[2], tools_keys, 1, I_CAPS_PICKED);
        return 0;
    case I_CAPS_PICKED:
        if (!object_at(w, w->pick.found[0])) return reject(w, "initialize");
        w->phase = W_DONE;
        return 0;

    case C_RESULT:
        w->call_error = root[ROOT_ERROR] >= 0;
        if (!object_at(w, root[ROOT_RESULT])) { w->phase = W_DONE; return 0; }
        begin_pick(w, root[ROOT_RESULT], call_keys, 1, C_HINT);
        return 0;
    case C_HINT:
        if (true_at(w, w->pick.found[0])) w->call_error = 1;
        w->phase = W_DONE;
        return 0;

    case D_START:
        if (r->pages >= MCP_PAGE_MAX) return reject(w, "discovery page limit");
        r->pages++;
        if (!object_at(w, root[ROOT_RESULT])) return reject(w, "discovery error");
        begin_pick(w, root[ROOT_RESULT], page_keys, 2, D_PICKED);
        return 0;
    case D_PICKED:
        w->list = w->pick.found[0];
        if (w->list < 0 || t[w->list].type != JSON_ARRAY) return reject(w, "missing tool list");
        r->cursor[0] = 0;
        w->tool = w->list + 1;
        if (w->pick.found[1] < 0) { w->phase = D_TOOL; return 0; }
        begin_decode(w, w->pick.found[1], r->cursor, sizeof(r->cursor), D_CURSOR_DECODED, W_REJECT, "invalid discovery cursor");
        return 0;
    case D_CURSOR_DECODED:
        if (!r->cursor[0]) return reject(w, "invalid discovery cursor");
        w->phase = D_TOOL;
        return 0;
    case D_TOOL:
        if (w->tool >= t[w->list].next) { w->phase = D_FINISH; return 0; }
        if (r->entries == MCP_ENTRY_MAX) return reject(w, "discovery entry limit");
        if (!object_at(w, w->tool)) return reject(w, "invalid tool entry");
        begin_pick(w, w->tool, tool_keys, 4, T_PICKED);
        return 0;
    case T_PICKED:
        memcpy(w->held, w->pick.found, sizeof(w->held));
        if (w->held[0] < 0 || t[w->held[0]].type != JSON_STRING) return reject(w, "invalid tool entry");
        begin_decode(w, w->held[0], w->name, sizeof(w->name), T_NAME_DECODED, T_NAME_LONG, NULL);
        return 0;
    case T_NAME_LONG:
        /* An over-long name only disqualifies that tool. */
        r->seen[r->entries++][0] = 0;
        return skip(w, "Skipped MCP tool: unsupported name.");
    case T_NAME_DECODED:
        w->dup = 0;
        w->phase = T_DUP;
        return 0;
    case T_DUP:
        while (w->dup < r->entries) {
            if (!fits(*work, budget, DUP_COST)) return 1;
            *work += DUP_COST;
            if (!strcmp(r->seen[w->dup], w->name)) return reject(w, "duplicate tool name");
            w->dup++;
        }
        strcpy(r->seen[r->entries++], w->name);
        w->phase = T_CHECK;
        return 0;
    case T_CHECK: {
        int selected = !c->selection_present;
        if (!fits(*work, budget, CHECK_COST)) return 1;
        *work += CHECK_COST;
        if (!name_valid(w->name)) return skip(w, "Skipped MCP tool: unsupported name.");
        n = snprintf(w->mapped, sizeof(w->mapped), "mcp_%s_%s", c->server, w->name);
        if (n < 0 || (size_t)n >= sizeof(w->mapped)) return skip(w, "Skipped MCP tool: mapped name exceeds 63 bytes.");
        for (i = 0; i < c->selected_count; i++) if (!strcmp(c->selected[i], w->name)) selected = 1;
        if (!selected) return skip(w, NULL);
        w->read_only = 0;
        if (w->held[1] < 0) { w->phase = T_ELIGIBLE; return 0; }
        if (!object_at(w, w->held[1])) return reject(w, "invalid annotations");
        begin_pick(w, w->held[1], annotation_keys, 1, T_ANNOTATION_PICKED);
        return 0;
    }
    case T_ANNOTATION_PICKED:
        w->read_only = true_at(w, w->pick.found[0]);
        w->phase = T_ELIGIBLE;
        return 0;
    case T_ELIGIBLE:
        /* At Tavily only the two audited tools are eligible. Elsewhere this
         * is explicitly a server claim, not a security sandbox. */
        if (c->official_tavily) w->read_only = !strcmp(w->name, "tavily_search") || !strcmp(w->name, "tavily_extract");
        if (!w->read_only) return skip(w, "Skipped MCP tool: no eligible read-only contract.");
        if (w->held[2] < 0 || t[w->held[2]].type != JSON_OBJECT) return skip(w, "Skipped MCP tool: unsupported input schema.");
        w->description[0] = 0;
        w->desc_len = 0;
        if (w->held[3] < 0) { mcp_redact_init(&w->redact, c, NULL, w->description); w->phase = T_REDACT; return 0; }
        begin_decode(w, w->held[3], w->description, sizeof(w->description), T_DESCRIPTION_DECODED, T_DESCRIPTION_BAD, NULL);
        return 0;
    case T_DESCRIPTION_BAD:
        return skip(w, "Skipped MCP tool: unsupported description.");
    case T_DESCRIPTION_DECODED:
        w->desc_len = w->decode.used;
        mcp_redact_init(&w->redact, c, NULL, w->description);
        w->phase = T_REDACT;
        return 0;
    case T_REDACT:
        i = mcp_redact_step(&w->redact, room(*work, budget));
        *work += w->redact.work;
        if (!i) return 1;
        w->quoted[0] = '"'; w->quoted_used = 1; w->quoted_at = 0;
        w->phase = T_QUOTE;
        return 0;
    case T_QUOTE: {
        /* json_quote, one character a unit. */
        const size_t cap = sizeof(w->quoted);
        for (;;) {
            unsigned long cp;
            char escaped[7];
            const char *bytes;
            size_t start = w->quoted_at, k;
            if (w->quoted_at >= w->desc_len) break;
            if (!fits(*work, budget, JSON_ATOM_MAX)) return 1;
            if (utf8_next(w->description, w->desc_len, &w->quoted_at, &cp)) return reject(w, "description limit");
            k = w->quoted_at - start; bytes = w->description + start;
            *work += k;
            if (cp == '"' || cp == '\\') { escaped[0] = '\\'; escaped[1] = (char)cp; bytes = escaped; k = 2; }
            else if (cp < 32) { snprintf(escaped, sizeof(escaped), "\\u%04lx", cp); bytes = escaped; k = 6; }
            if (w->quoted_used >= cap || k > cap - w->quoted_used - 1) return reject(w, "description limit");
            memcpy(w->quoted + w->quoted_used, bytes, k); w->quoted_used += k;
        }
        if (cap - w->quoted_used < 2) return reject(w, "description limit");
        w->quoted[w->quoted_used++] = '"'; w->quoted[w->quoted_used] = 0;
        w->phase = T_COMPOSE;
        return 0;
    }
    case T_COMPOSE: {
        /* Sizes are known before a byte is copied, so a schema that cannot
         * fit is refused without reading it. */
        size_t available = sizeof(r->schemas) - r->schema_len;
        if (r->count == MCP_TOOL_MAX) return reject(w, "exposed tool limit");
        n = snprintf(w->prefix, sizeof(w->prefix), "%s{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":",
            r->count ? "," : "", w->mapped);
        w->prefix_len = (size_t)n;
        w->total = w->prefix_len + w->quoted_used + 14 + (size_t)(t[w->held[2]].end - t[w->held[2]].start) + 2;
        if (w->total >= available) return reject(w, "schema limit");
        w->piece = 0; w->piece_at = 0; w->copied = 0;
        w->phase = T_COPY;
        return 0;
    }
    case T_COPY: {
        size_t length;
        const char *piece;
        if (w->piece == 5) {
            r->schemas[r->schema_len + w->total] = 0;
            r->schema_len += w->total;
            strcpy(r->tools[r->count].name, w->mapped);
            strcpy(r->tools[r->count].original, w->name);
            r->count++;
            return skip(w, NULL);
        }
        if (!fits(*work, budget, COPY_CHUNK)) return 1;
        piece = copy_piece(w, w->piece, &length);
        amount = length - w->piece_at < COPY_CHUNK ? length - w->piece_at : COPY_CHUNK;
        memcpy(r->schemas + r->schema_len + w->copied, piece + w->piece_at, amount);
        w->copied += amount; w->piece_at += amount; *work += amount;
        if (w->piece_at == length) { w->piece++; w->piece_at = 0; }
        return 0;
    }
    case D_FINISH:
        if (r->cursor[0] && (r->pages == MCP_PAGE_MAX || r->entries == MCP_ENTRY_MAX))
            return reject(w, "discovery budget exhausted");
        w->more = r->cursor[0] != 0;
        w->phase = W_DONE;
        return 0;
    default:
        w->phase = W_DONE;
        return 0;
    }
}
/* Every transition costs a unit, so phases that read nothing still end. */
static int advance(McpWork *w, size_t *work, size_t budget)
{
    int yield = advance_phase(w, work, budget);
    if (!yield) *work += 1;
    return yield;
}
int mcp_work_step(McpWork *w, size_t budget)
{
    size_t work = 0;
    while (w->phase != W_DONE) if (advance(w, &work, budget)) break;
    w->work = work; w->units += work; w->steps++;
    if (work > w->peak) w->peak = work;
    return w->phase == W_DONE;
}
int mcp_post(const McpConfig *c, const char *session, const char *version,
             const char *body, char *out, size_t cap)
{
    char authority[280];
    size_t at;
    int n, h;
    if (!c->enabled || strlen(body) > MCP_MESSAGE_CAP) return -1;
    if (version && !*version) version = NULL; /* empty means no header */
    if (session) for (at = 0; session[at]; at++)
        if ((unsigned char)session[at] < 0x21 || (unsigned char)session[at] > 0x7e) return -1;
    if (version && strcmp(version, "2025-11-25") && strcmp(version, "2025-06-18") && strcmp(version, "2025-03-26")) return -1;
    if (c->port == 443) snprintf(authority, sizeof(authority), "%s", c->host);
    else snprintf(authority, sizeof(authority), "%s:%u", c->host, (unsigned)c->port);
    h = snprintf(out, cap,
        "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Sherclawk/1.0\r\n"
        "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\nContent-Length: %lu\r\n%s%s%s%s%s%s%s\r\n",
        c->path, authority, (unsigned long)strlen(body), c->headers,
        session && *session ? "Mcp-Session-Id: " : "", session && *session ? session : "", session && *session ? "\r\n" : "",
        version && *version ? "MCP-Protocol-Version: " : "", version && *version ? version : "", version && *version ? "\r\n" : "");
    if (h < 0 || h > MCP_HEADER_CAP || (size_t)h >= cap) return -1;
    n = snprintf(out + h, cap - (size_t)h, "%s", body);
    return n < 0 || (size_t)n >= cap - (size_t)h ? -1 : h + n;
}
