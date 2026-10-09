#include "mcp.h"
#include <stdio.h>
#include <string.h>

static JsonToken tokens[8192];
/* Large scratch lives here rather than on the small Toolbox stack. */
static char description[8192], quoted[16384];
static int object(const char *s)
{
    size_t len = strlen(s);
    int n;
    if (len > MCP_MESSAGE_CAP) return -1;
    n = json_parse(s, len, tokens, 8192);
    if (n < 1 || tokens[0].type != JSON_OBJECT) return -1;
    /* Ambiguous keys are protocol failures. */
    return json_keys_unique(s, tokens, n);
}
static int string(const char *s, int ob, const char *key, char *out, size_t cap)
{
    int i = json_member(s, tokens, ob, key);
    return i < 0 ? -1 : json_string(s, tokens, i, out, cap);
}
static int signed_integer(const char *s, int index)
{
    int i = tokens[index].start, end = tokens[index].end;
    unsigned long n = 0;
    if (tokens[index].type != JSON_PRIMITIVE) return 0;
    if (s[i] == '-') i++;
    if (i == end) return 0;
    for (; i < end; i++) {
        unsigned long digit;
        if (s[i] < '0' || s[i] > '9') return 0;
        digit = (unsigned long)(s[i] - '0');
        if (n > (2147483648UL - digit) / 10UL) return 0;
        n = n * 10UL + digit;
    }
    return n <= (s[tokens[index].start] == '-' ? 2147483648UL : 2147483647UL);
}
int mcp_rpc(const char *s, long expected, char *reply, size_t cap)
{
    int id, method, result, err, count;
    long number;
    char version[16], name[128];
    if (cap) reply[0] = 0;
    if (object(s) || string(s, 0, "jsonrpc", version, sizeof(version)) < 0 || strcmp(version, "2.0")) return -1;
    id = json_member(s, tokens, 0, "id");
    method = json_member(s, tokens, 0, "method");
    result = json_member(s, tokens, 0, "result");
    err = json_member(s, tokens, 0, "error");
    if (method >= 0) {
        if (result >= 0 || err >= 0 || json_string(s, tokens, method, name, sizeof(name)) < 0) return -1;
        if (id < 0) return 0;
        if (tokens[id].type != JSON_STRING && !signed_integer(s, id)) return -1;
        count = snprintf(reply, cap, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,%s}",
            tokens[id].end - tokens[id].start, s + tokens[id].start,
            !strcmp(name, "ping") ? "\"result\":{}" : "\"error\":{\"code\":-32601,\"message\":\"Unsupported server request\"}");
        return count < 0 || (size_t)count >= cap ? -1 : 2;
    }
    if (id < 0 || json_integer(s, tokens, id, &number) || number != expected ||
        ((result >= 0) == (err >= 0))) return -1;
    if (err >= 0) {
        int code;
        char message[1024];
        code = json_member(s, tokens, err, "code");
        if (tokens[err].type != JSON_OBJECT || code < 0 || !signed_integer(s, code) ||
            string(s, err, "message", message, sizeof(message)) < 0) return -1;
    }
    return 1;
}
int mcp_initialize_result(const char *s, char *version, size_t cap)
{
    int r, c, info;
    char name[256], revision[256];
    if (object(s)) return -1;
    r = json_member(s, tokens, 0, "result");
    if (r < 0 || tokens[r].type != JSON_OBJECT || string(s, r, "protocolVersion", version, cap) < 0 ||
        (strcmp(version, "2025-11-25") && strcmp(version, "2025-06-18") && strcmp(version, "2025-03-26"))) return -1;
    info = json_member(s, tokens, r, "serverInfo");
    if (info < 0 || tokens[info].type != JSON_OBJECT ||
        string(s, info, "name", name, sizeof(name)) < 0 || !name[0] ||
        string(s, info, "version", revision, sizeof(revision)) < 0) return -1;
    c = json_member(s, tokens, r, "capabilities");
    if (c < 0 || tokens[c].type != JSON_OBJECT) return -1;
    c = json_member(s, tokens, c, "tools");
    return c < 0 || tokens[c].type != JSON_OBJECT ? -1 : 0;
}
static int name_valid(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
static int discard(McpRegistry *r, const char *reason)
{
    r->count = 0; r->schema_len = 0; r->schemas[0] = 0; r->cursor[0] = 0;
    snprintf(r->notice, sizeof(r->notice), "MCP unavailable: %s.", reason);
    return -1;
}
int mcp_discover_page(const McpConfig *c, McpRegistry *r, const char *s)
{
    int result, list, i, cursor;
    if (r->pages >= MCP_PAGE_MAX) return discard(r, "discovery page limit");
    r->pages++;
    if (object(s)) return discard(r, "malformed discovery");
    result = json_member(s, tokens, 0, "result");
    if (result < 0 || tokens[result].type != JSON_OBJECT) return discard(r, "discovery error");
    list = json_member(s, tokens, result, "tools");
    if (list < 0 || tokens[list].type != JSON_ARRAY) return discard(r, "missing tool list");
    cursor = json_member(s, tokens, result, "nextCursor");
    r->cursor[0] = 0;
    if (cursor >= 0 && (json_string(s, tokens, cursor, r->cursor, sizeof(r->cursor)) < 0 || !r->cursor[0]))
        return discard(r, "invalid discovery cursor");
    for (i = list + 1; i < tokens[list].next; i = tokens[i].next) {
        char name[64], mapped[64];
        int j, schema, annotation, read_only = 0, selected = !c->selection_present, n;
        size_t room;
        if (r->entries == MCP_ENTRY_MAX) return discard(r, "discovery entry limit");
        if (tokens[i].type != JSON_OBJECT || (j = json_member(s, tokens, i, "name")) < 0 ||
            tokens[j].type != JSON_STRING) return discard(r, "invalid tool entry");
        if (json_string(s, tokens, j, name, sizeof(name)) < 0) {
            /* An over-long name only disqualifies that tool. */
            r->seen[r->entries++][0] = 0;
            strcpy(r->notice, "Skipped MCP tool: unsupported name.");
            continue;
        }
        for (j = 0; j < r->entries; j++) if (!strcmp(r->seen[j], name))
            return discard(r, "duplicate tool name");
        strcpy(r->seen[r->entries++], name);
        if (!name_valid(name)) { strcpy(r->notice, "Skipped MCP tool: unsupported name."); continue; }
        n = snprintf(mapped, sizeof(mapped), "mcp_%s_%s", c->server, name);
        if (n < 0 || (size_t)n >= sizeof(mapped)) { strcpy(r->notice, "Skipped MCP tool: mapped name exceeds 63 bytes."); continue; }
        for (j = 0; j < c->selected_count; j++) if (!strcmp(c->selected[j], name)) selected = 1;
        if (!selected) continue;
        annotation = json_member(s, tokens, i, "annotations");
        if (annotation >= 0) {
            int hint;
            if (tokens[annotation].type != JSON_OBJECT) return discard(r, "invalid annotations");
            hint = json_member(s, tokens, annotation, "readOnlyHint");
            read_only = hint >= 0 && tokens[hint].type == JSON_PRIMITIVE &&
                tokens[hint].end - tokens[hint].start == 4 && !memcmp(s + tokens[hint].start, "true", 4);
        }
        /* At Tavily only the two audited tools are eligible. Elsewhere this
         * is explicitly a server claim, not a security sandbox. */
        if (c->official_tavily) read_only = !strcmp(name, "tavily_search") || !strcmp(name, "tavily_extract");
        if (!read_only) { strcpy(r->notice, "Skipped MCP tool: no eligible read-only contract."); continue; }
        schema = json_member(s, tokens, i, "inputSchema");
        if (schema < 0 || tokens[schema].type != JSON_OBJECT) { strcpy(r->notice, "Skipped MCP tool: unsupported input schema."); continue; }
        description[0] = 0;
        j = json_member(s, tokens, i, "description");
        if (j >= 0 && json_string(s, tokens, j, description, sizeof(description)) < 0) {
            strcpy(r->notice, "Skipped MCP tool: unsupported description."); continue;
        }
        mcp_redact(c, NULL, description);
        if (json_quote(description, quoted, sizeof(quoted)) < 0) return discard(r, "description limit");
        if (r->count == MCP_TOOL_MAX) return discard(r, "exposed tool limit");
        room = sizeof(r->schemas) - r->schema_len;
        n = snprintf(r->schemas + r->schema_len, room,
            "%s{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":%s,\"parameters\":%.*s}}",
            r->count ? "," : "", mapped, quoted, tokens[schema].end - tokens[schema].start, s + tokens[schema].start);
        if (n < 0 || (size_t)n >= room) return discard(r, "schema limit");
        r->schema_len += (size_t)n;
        strcpy(r->tools[r->count].name, mapped); strcpy(r->tools[r->count].original, name); r->count++;
    }
    if (r->cursor[0] && (r->pages == MCP_PAGE_MAX || r->entries == MCP_ENTRY_MAX)) return discard(r, "discovery budget exhausted");
    return r->cursor[0] ? 0 : 1;
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
