#include "mcp.h"
#include "http.h"
#include <stdio.h>
#include <string.h>

static int error(McpConfig *c, char *out, size_t cap, const char *field)
{
    memset(c, 0, sizeof(*c));
    snprintf(out, cap, "Invalid MCP configuration: %s.", field);
    return -1;
}
static int ascii_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') ||
        (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
static int lower_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int c = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        if (c != *b) return 0;
    }
    return !*a && !*b;
}
static int header_name(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!( (*s >= 'a' && *s <= 'z') ||
        (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
        strchr("!#$%&'*+-.^_`|~", *s))) return 0;
    return 1;
}
static int owned_header(const char *s)
{
    static const char *const names[] = {"host", "content-length", "content-type",
        "accept", "connection", "transfer-encoding", "content-encoding",
        "accept-encoding", "user-agent", "mcp-session-id", "mcp-protocol-version",
        "trailer", "te", "upgrade", "expect", "origin", "last-event-id"};
    size_t i;
    for (i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (lower_equal(s, names[i])) return 1;
    return 0;
}
static int add_secret(McpConfig *c, const char *v, size_t n)
{
    if (n + 1 > sizeof(c->secrets) - c->secrets_len) return -1;
    memcpy(c->secrets + c->secrets_len, v, n); c->secrets[c->secrets_len + n] = 0;
    c->secrets_len += n + 1;
    return 0;
}
/* Query values can carry API keys. Very short values are not registered:
 * redacting them would mangle unrelated text such as "v=1". */
#define QUERY_SECRET_MIN 8
static int query_secrets(McpConfig *c)
{
    const char *p = strchr(c->path, '?');
    if (!p) return 0;
    for (p++; *p;) {
        size_t n = strcspn(p, "&");
        const char *eq = memchr(p, '=', n), *v = eq ? eq + 1 : p;
        size_t vn = (size_t)(p + n - v);
        if (vn >= QUERY_SECRET_MIN && add_secret(c, v, vn)) return -1;
        p += n;
        if (*p == '&') p++;
    }
    return 0;
}
/* Case-insensitive match against the header names already rendered. */
static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int header_present(const char *headers, const char *name)
{
    size_t want = strlen(name);
    for (; *headers; headers = strstr(headers, "\r\n") + 2) {
        size_t i, n = strcspn(headers, ":");
        if (n != want) continue;
        for (i = 0; i < n && fold(headers[i]) == fold(name[i]); i++) {}
        if (i == n) return 1;
    }
    return 0;
}
/* Large scratch lives here rather than on the small Toolbox stack; the
 * wrapper below wipes it on every exit path. */
static JsonToken t[2048];
static char url[2048], key[MCP_CONFIG_CAP + 1], val[MCP_CONFIG_CAP + 1];
static int parse(const char *s, size_t len, McpConfig *c, char *err, size_t cap)
{
    int n, root, server, i, j;
    size_t used = 0, k;
    memset(c, 0, sizeof(*c));
    if (cap) err[0] = 0;
    if (len > MCP_CONFIG_CAP) return error(c, err, cap, "8 KiB limit");
    n = json_parse(s, len, t, 2048);
    if (n < 1 || t[0].type != JSON_OBJECT) return error(c, err, cap, "JSON object");
    if (json_keys_unique(s, t, n)) return error(c, err, cap, "duplicate key");
    root = json_member(s, t, 0, "mcpServers");
    if (root != 2 || t[root].type != JSON_OBJECT || t[0].next != t[root].next)
        return error(c, err, cap, "mcpServers (sole root field)");
    if (t[root].next == root + 1) return 0;
    server = root + 2;
    if (t[server].next != t[root].next || t[server].type != JSON_OBJECT)
        return error(c, err, cap, "one server object");
    if (json_string(s, t, root + 1, c->server, sizeof(c->server)) < 0 || !ascii_name(c->server))
        return error(c, err, cap, "server name (ASCII letters, digits, underscores; 31 bytes)");
    for (i = server + 1; i < t[server].next; i = t[i + 1].next) {
        if (json_string(s, t, i, key, sizeof(key)) < 0 ||
            (strcmp(key, "url") && strcmp(key, "headers") && strcmp(key, "tools")))
            return error(c, err, cap, "unsupported server field");
    }
    i = json_member(s, t, server, "url");
    if (i < 0 || json_string(s, t, i, url, sizeof(url)) < 0)
        return error(c, err, cap, "url");
    for (k = 0; url[k]; k++) if ((unsigned char)url[k] <= 32 ||
        (unsigned char)url[k] >= 127 || url[k] == '#' || url[k] == '\\')
        return error(c, err, cap, "url (HTTPS, ASCII, no controls or fragment)");
    if (http_parse_url(url, strlen(url), c->host, sizeof(c->host), &c->port,
        c->path, sizeof(c->path)) || !c->port || strchr(c->host, '@'))
        return error(c, err, cap, "url (HTTPS without user information)");
    {
        const char *end_authority = url + 8 + strlen(c->host);
        if (*end_authority == ':') {
            end_authority++;
            while (*end_authority >= '0' && *end_authority <= '9') end_authority++;
        }
        if (*end_authority && *end_authority != '/' && *end_authority != '?')
            return error(c, err, cap, "url authority");
    }
    /* Restrict DNS names to the host syntax supported by Certainly; IPv6 is
     * not accepted by this first-release transport. Preserve query strings. */
    for (k = 0; c->host[k]; k++) if (!( (c->host[k] >= 'a' && c->host[k] <= 'z') ||
        (c->host[k] >= 'A' && c->host[k] <= 'Z') ||
        (c->host[k] >= '0' && c->host[k] <= '9') || c->host[k] == '-' || c->host[k] == '.'))
        return error(c, err, cap, "url host");
    /* http_parse_url's existing GET helper defaults a query-only suffix to
     * '/'. MCP must preserve it exactly rather than silently drop it. */
    {
        const char *authority = url + 8;
        const char *suffix = strpbrk(authority, "/?");
        if (suffix && *suffix == '?') {
            int written = snprintf(c->path, sizeof(c->path), "/%s", suffix);
            if (written < 0 || (size_t)written >= sizeof(c->path)) return error(c, err, cap, "url path limit");
        }
    }
    /* The query string (which may carry the key) is not part of the match. */
    k = strcspn(c->path, "?");
    c->official_tavily = lower_equal(c->host, "mcp.tavily.com") && c->port == 443 &&
        ((k == 5 && !memcmp(c->path, "/mcp/", 5)) || (k == 4 && !memcmp(c->path, "/mcp", 4)));
    if (query_secrets(c)) return error(c, err, cap, "url query limit");
    i = json_member(s, t, server, "headers");
    if (i >= 0) {
        if (t[i].type != JSON_OBJECT) return error(c, err, cap, "headers object");
        for (j = i + 1; j < t[i].next; j = t[j + 1].next) {
            int wrote;
            if (json_string(s, t, j, key, sizeof(key)) < 0 || !header_name(key) || owned_header(key))
                return error(c, err, cap, "headers name (invalid or transport-owned)");
            if (header_present(c->headers, key)) return error(c, err, cap, "duplicate header");
            if (json_string(s, t, j + 1, val, sizeof(val)) < 0) return error(c, err, cap, "headers value");
            for (k = 0; val[k]; k++) if ((unsigned char)val[k] < 32 || (unsigned char)val[k] >= 127)
                return error(c, err, cap, "headers value (visible ASCII only)");
            wrote = snprintf(c->headers + used, sizeof(c->headers) - used, "%s: %s\r\n", key, val);
            if (wrote < 0 || (size_t)wrote >= sizeof(c->headers) - used)
                return error(c, err, cap, "headers limit");
            used += (size_t)wrote;
            if (add_secret(c, val, strlen(val))) return error(c, err, cap, "headers limit");
        }
    }
    i = json_member(s, t, server, "tools");
    if (i >= 0) {
        c->selection_present = 1;
        if (t[i].type != JSON_ARRAY) return error(c, err, cap, "tools array");
        for (j = i + 1; j < t[i].next; j = t[j].next) {
            int previous;
            if (c->selected_count == MCP_TOOL_MAX || json_string(s, t, j,
                c->selected[c->selected_count], sizeof(c->selected[0])) < 0 ||
                !ascii_name(c->selected[c->selected_count])) return error(c, err, cap, "tools names/limit");
            for (previous = 0; previous < c->selected_count; previous++)
                if (!strcmp(c->selected[previous], c->selected[c->selected_count]))
                    return error(c, err, cap, "duplicate tools selection");
            c->selected_count++;
        }
    }
    c->enabled = 1;
    return 0;
}
int mcp_config_parse(const char *s, size_t len, McpConfig *c, char *err, size_t cap)
{
    int result = parse(s, len, c, err, cap);
    memset(val, 0, sizeof(val)); memset(url, 0, sizeof(url)); memset(key, 0, sizeof(key));
    return result;
}
/* Resumable redaction: a naive search per needle, one text/needle comparison
 * per unit, so a long description cannot stall the event loop. Needles run in
 * the original order: the session, then each secret and its payload. */
enum { RED_SECRET, RED_MEASURE, RED_MATCH, RED_DONE };
enum { STAGE_SESSION, STAGE_WHOLE, STAGE_PAYLOAD };
static void redact_begin(McpRedact *r, const char *needle, int stage)
{
    r->needle = needle; r->stage = stage; r->pos = 0; r->k = 0; r->state = RED_MATCH;
}
void mcp_redact_init(McpRedact *r, const McpConfig *config, const char *session, char *text)
{
    memset(r, 0, sizeof(*r));
    r->config = config; r->text = text;
    if (session) redact_begin(r, session, STAGE_SESSION);
    else r->state = RED_SECRET;
}
/* The current needle is exhausted: move on to the next one. */
static void redact_next(McpRedact *r)
{
    const char *value = r->config->secrets + r->at;
    if (r->stage == STAGE_SESSION) { r->state = RED_SECRET; return; }
    if (r->stage == STAGE_WHOLE && r->space && value[r->space]) {
        redact_begin(r, value + r->space, STAGE_PAYLOAD);
        return;
    }
    r->at += r->scan + 1;
    r->state = RED_SECRET;
}
int mcp_redact_step(McpRedact *r, size_t budget)
{
    size_t work = 0;
    while (r->state != RED_DONE && work < budget) {
        const char *value = r->config->secrets + r->at;
        work++;
        switch (r->state) {
        case RED_SECRET:
            if (r->at >= r->config->secrets_len) r->state = RED_DONE;
            else { r->scan = 0; r->space = 0; r->state = RED_MEASURE; }
            break;
        case RED_MEASURE:
            /* Length and payload start (after the first space), one character a unit. */
            if (!value[r->scan]) redact_begin(r, value, STAGE_WHOLE);
            else { if (value[r->scan] == ' ' && !r->space) r->space = r->scan + 1; r->scan++; }
            break;
        default:
            if (!r->needle[0] || !r->text[r->pos]) redact_next(r);
            else if (!r->needle[r->k]) { memset(r->text + r->pos, '*', r->k); r->pos += r->k; r->k = 0; }
            else if (r->text[r->pos + r->k] == r->needle[r->k]) r->k++;
            else { r->pos++; r->k = 0; }
        }
    }
    r->work = work;
    return r->state == RED_DONE;
}
void mcp_redact(const McpConfig *c, const char *session, char *s)
{
    McpRedact r;
    mcp_redact_init(&r, c, session, s);
    mcp_redact_step(&r, (size_t)-1);
}
