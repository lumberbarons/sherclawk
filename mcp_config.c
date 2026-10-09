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
/* Reject duplicates even when escaped spellings differ, at every depth. */
static int unique(const char *s, const JsonToken *t, int count)
{
    int i, j, k;
    char a[MCP_CONFIG_CAP + 1], b[MCP_CONFIG_CAP + 1];
    for (i = 0; i < count; i++) if (t[i].type == JSON_OBJECT) {
        for (j = i + 1; j < t[i].next; j = t[j + 1].next) {
            if (json_string(s, t, j, a, sizeof(a)) < 0) return -1;
            for (k = i + 1; k < j; k = t[k + 1].next) {
                if (json_string(s, t, k, b, sizeof(b)) < 0 || !strcmp(a, b)) return -1;
            }
        }
    }
    return 0;
}
int mcp_config_parse(const char *s, size_t len, McpConfig *c, char *err, size_t cap)
{
    static JsonToken t[2048];
    static char url[2048], key[MCP_CONFIG_CAP + 1], val[MCP_CONFIG_CAP + 1];
    int n, root, server, i, j;
    size_t used = 0, k;
    memset(c, 0, sizeof(*c));
    if (cap) err[0] = 0;
    if (len > MCP_CONFIG_CAP) return error(c, err, cap, "8 KiB limit");
    n = json_parse(s, len, t, 2048);
    if (n < 1 || t[0].type != JSON_OBJECT) return error(c, err, cap, "JSON object");
    if (unique(s, t, n)) return error(c, err, cap, "duplicate key");
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
            int wrote, prev;
            char prior[MCP_CONFIG_CAP + 1];
            if (json_string(s, t, j, key, sizeof(key)) < 0 || !header_name(key) || owned_header(key))
                return error(c, err, cap, "headers name (invalid or transport-owned)");
            for (prev = i + 1; prev < j; prev = t[prev + 1].next) {
                if (json_string(s, t, prev, prior, sizeof(prior)) < 0) return error(c, err, cap, "headers name");
                /* lower_equal's second argument is lowercase. */
                for (k = 0; prior[k]; k++) if (prior[k] >= 'A' && prior[k] <= 'Z') prior[k] += 32;
                if (lower_equal(key, prior)) return error(c, err, cap, "duplicate header");
            }
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
    memset(val, 0, sizeof(val)); memset(url, 0, sizeof(url));
    return 0;
}
static void redact_value(char *s, const char *secret)
{
    char *p;
    size_t n = strlen(secret);
    if (!n) return;
    while ((p = strstr(s, secret)) != NULL) { memset(p, '*', n); s = p + n; }
}
void mcp_redact(const McpConfig *c, const char *session, char *s)
{
    size_t at = 0;
    if (session) redact_value(s, session);
    while (at < c->secrets_len) {
        const char *v = c->secrets + at;
        const char *payload = strchr(v, ' ');
        size_t n = strlen(v);
        redact_value(s, v);
        if (payload && payload[1]) redact_value(s, payload + 1);
        at += n + 1;
    }
}
