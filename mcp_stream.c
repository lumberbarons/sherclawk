/* Streaming HTTP framing and SSE lines. No whole-exchange accumulation or
 * close-delimited SSE wait; caller yields after at most 8 KiB fed bytes. */
#include "mcp.h"
#include <string.h>
#include <stdlib.h>
static int fail(McpStream *s) { s->failed = 1; return -1; }
static int equal(const char *a, const char *b)
{
    while (*a && *b) {
        int c = *a++;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != *b++) return 0;
    }
    return !*a && !*b;
}
static int decimal(const char *p, size_t *out)
{
    size_t n = 0;
    if (!*p) return -1;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > (MCP_TRAFFIC_CAP - (size_t)(*p - '0')) / 10) return -1;
        n = n * 10 + (size_t)(*p - '0');
    }
    *out = n; return 0;
}
static int headers(McpStream *s)
{
    char *p = s->headers, *end;
    int type = 0, session = 0, length = 0, encoding = 0, transfer = 0;
    end = strstr(p, "\r\n");
    if (!end) return fail(s);
    *end = 0;
    if (strlen(p) < 12 || (memcmp(p, "HTTP/1.1 ", 9) && memcmp(p, "HTTP/1.0 ", 9)) ||
        p[9] < '0' || p[9] > '9' || p[10] < '0' || p[10] > '9' || p[11] < '0' || p[11] > '9' ||
        (p[12] && p[12] != ' ')) return fail(s);
    s->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + p[11] - '0';
    if (s->status < 200 || s->status >= 300) return fail(s);
    p = end + 2;
    while (*p) {
        char *colon, *v, *tail;
        end = strstr(p, "\r\n");
        if (!end) return fail(s);
        if (end == p) break;
        *end = 0;
        colon = strchr(p, ':');
        if (!colon || colon == p || *p == ' ' || *p == '\t') return fail(s);
        {
            char *name;
            for (name = p; name < colon; name++) {
                unsigned char ch = (unsigned char)*name;
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || strchr("!#$%&'*+-.^_`|~", ch))) return fail(s);
            }
        }
        *colon = 0; v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        tail = v + strlen(v);
        while (tail > v && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (equal(p, "content-type")) {
            char *semi = strchr(v, ';');
            if (semi) *semi = 0;
            if (type++) return fail(s);
            if (equal(v, "text/event-stream")) s->sse = 1;
            else if (!equal(v, "application/json")) return fail(s);
        } else if (equal(p, "content-length")) {
            if (length++ || decimal(v, &s->remaining)) return fail(s);
            s->has_length = 1;
        } else if (equal(p, "transfer-encoding")) {
            if (transfer++ || !equal(v, "chunked")) return fail(s);
            s->chunked = 1;
        } else if (equal(p, "content-encoding")) {
            if (encoding++ || !equal(v, "identity")) return fail(s);
        } else if (equal(p, "mcp-session-id")) {
            size_t i, n = strlen(v);
            if (session++ || !n || n >= sizeof(s->session)) return fail(s);
            for (i = 0; i < n; i++) if ((unsigned char)v[i] < 0x21 || (unsigned char)v[i] > 0x7e) return fail(s);
            strcpy(s->session, v);
        }
        p = end + 2;
    }
    if (length && transfer) return fail(s);
    if (s->acknowledgment) {
        /* Streamable HTTP acknowledges notifications/responses with 202 and
         * no body. 204 is accepted as a bodyless acknowledgment as well. */
        if ((s->status != 202 && s->status != 204) || s->chunked || (length && s->remaining)) return fail(s);
        s->done = 1; return 1;
    }
    if (!type || s->status != 200 || (length && !s->remaining)) return fail(s);
    s->headers_done = 1;
    return 0;
}
void mcp_stream_init(McpStream *s, int ack, McpMessage cb, void *context)
{
    memset(s, 0, sizeof(*s)); s->acknowledgment = ack;
    s->callback = cb; s->context = context;
}
static int dispatch(McpStream *s)
{
    int r;
    if (!s->message_len) return 0; /* SSE stream-priming event. */
    s->message[s->message_len] = 0;
    if (!s->callback) return fail(s);
    r = s->callback(s->context, s->message);
    s->message_len = 0;
    if (r < 0) return fail(s);
    if (r > 0) s->done = 1;
    return r;
}
static int sse_line(McpStream *s)
{
    char *v;
    size_t n;
    if (!s->line_len) return dispatch(s);
    s->line[s->line_len] = 0;
    if (s->line[0] == ':') return 0;
    v = strchr(s->line, ':');
    if (v) { *v++ = 0; if (*v == ' ') v++; }
    else v = s->line + s->line_len;
    if (strcmp(s->line, "data")) return 0;
    n = strlen(v);
    if (n + (s->message_len ? 1 : 0) > MCP_MESSAGE_CAP - s->message_len) return fail(s);
    if (s->message_len) s->message[s->message_len++] = '\n';
    memcpy(s->message + s->message_len, v, n); s->message_len += n;
    return 0;
}
static int body_byte(McpStream *s, unsigned char c)
{
    int r = 0;
    if (!s->sse) {
        if (s->message_len == MCP_MESSAGE_CAP || !c) return fail(s);
        s->message[s->message_len++] = (char)c; return 0;
    }
    /* SSE permits a single UTF-8 BOM, even across reads/chunks. */
    if (s->bom < 3) {
        if (!s->bom && c == 0xef) { s->bom = 1; return 0; }
        if (s->bom == 1) { if (c != 0xbb) return fail(s); s->bom = 2; return 0; }
        if (s->bom == 2) { if (c != 0xbf) return fail(s); s->bom = 3; return 0; }
        s->bom = 3;
    }
    if (s->skip_lf) { s->skip_lf = 0; if (c == '\n') return 0; }
    if (c == '\r' || c == '\n') {
        r = sse_line(s); s->line_len = 0; s->skip_lf = c == '\r'; return r;
    }
    if (!c || s->line_len == MCP_MESSAGE_CAP) return fail(s);
    s->line[s->line_len++] = (char)c; return 0;
}
static int finish(McpStream *s)
{
    if (!s->sse && dispatch(s) > 0) return 1;
    return fail(s); /* A notification or EOF is not a request response. */
}
static int chunk_byte(McpStream *s, unsigned char c)
{
    if (s->chunk_state == 0 || s->chunk_state == 4) {
        if (c == '\n') {
            size_t n = 0, i = 0;
            if (!s->chunk_len || s->chunk_line[s->chunk_len - 1] != '\r') return fail(s);
            s->chunk_line[--s->chunk_len] = 0;
            if (s->chunk_state == 4) {
                if (!s->chunk_len) return finish(s);
                /* Trailer fields cannot alter headers or credential state. */
                if (!strchr(s->chunk_line, ':')) return fail(s);
            } else {
                while (i < s->chunk_len && s->chunk_line[i] != ';') {
                    int d = s->chunk_line[i++];
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
                    else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
                    else return fail(s);
                    if (n > (MCP_TRAFFIC_CAP - (size_t)d) / 16) return fail(s);
                    n = n * 16 + (size_t)d;
                }
                if (!i) return fail(s);
                s->chunk_left = n; s->chunk_state = n ? 1 : 4;
            }
            s->chunk_len = 0; return 0;
        }
        if (s->chunk_len == MCP_HEADER_CAP || (c < 32 && c != '\r' && c != '\t')) return fail(s);
        s->chunk_line[s->chunk_len++] = (char)c; return 0;
    }
    if (s->chunk_state == 1) {
        int r = body_byte(s, c);
        if (!--s->chunk_left) s->chunk_state = 2;
        return r;
    }
    if (s->chunk_state == 2) {
        if (c != '\r') return fail(s);
        s->chunk_state = 3;
    } else {
        if (c != '\n') return fail(s);
        s->chunk_state = 0;
    }
    return 0;
}
int mcp_stream_feed(McpStream *s, const char *bytes, size_t len)
{
    size_t i;
    if (s->failed) return -1;
    if (s->done) return 1;
    if (len > 8192 || len > MCP_TRAFFIC_CAP - s->traffic) return fail(s);
    s->traffic += len;
    for (i = 0; i < len && !s->done && !s->failed; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (!s->headers_done) {
            size_t n;
            if (!c || (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127 || s->header_len == MCP_HEADER_CAP) return fail(s);
            s->headers[s->header_len++] = (char)c;
            n = s->header_len;
            if (n >= 4 && !memcmp(s->headers + n - 4, "\r\n\r\n", 4)) {
                s->headers[n] = 0;
                if (headers(s) < 0) return -1;
            }
        } else if (s->chunked) {
            if (chunk_byte(s, c) < 0) return -1;
        } else {
            if (s->has_length && !s->remaining) return fail(s);
            if (body_byte(s, c) < 0) return -1;
            if (s->has_length && !--s->remaining && !s->done) return finish(s);
        }
    }
    return s->failed ? -1 : s->done;
}
int mcp_stream_eof(McpStream *s)
{
    if (s->failed) return -1;
    if (s->done) return 1;
    if (!s->headers_done || s->chunked || (s->has_length && s->remaining)) return fail(s);
    return finish(s);
}
