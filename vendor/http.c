/*
 * http.c — minimal HTTP/1.1 helpers for "Hello, HTTPS".
 *
 * Adapted from the Postman example in the Certainly project
 * (https://github.com/minorbug/certainly), MIT License,
 * Copyright (c) 2026 Matt Baker. Trimmed to the GET-only path this
 * app needs; http_expected_total is new (used to stop reading as
 * soon as Content-Length bytes have arrived, instead of waiting for
 * the server to close).
 */
#include "http.h"
#include <stdio.h>
#include <string.h>

/* Copy src_len bytes into dst, NUL-terminated. 0 on success, -1 if
 * src doesn't fit in dst_cap including the terminator. */
static int copy_bounded(char *dst, size_t dst_cap,
                        const char *src, size_t src_len)
{
    if (src_len + 1 > dst_cap) return -1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return 0;
}

int http_parse_url(const char *url, size_t url_len,
                   char *host_out, size_t host_cap,
                   uint16_t *port_out,
                   char *path_out, size_t path_cap)
{
    const char *end = url + url_len;
    const char *p   = url;

    /* Certainly speaks TLS only. */
    if (url_len < 8 || memcmp(p, "https://", 8) != 0) return -1;
    p += 8;

    /* Authority: host[:port]. Ends at '/', '?', '#', or end-of-string. */
    const char *host_start = p;
    const char *host_end   = p;
    while (host_end < end && *host_end != ':' && *host_end != '/'
           && *host_end != '?' && *host_end != '#') {
        host_end++;
    }
    if (host_end == host_start) return -1;  /* empty host */

    if (copy_bounded(host_out, host_cap, host_start,
                     (size_t)(host_end - host_start)) != 0) {
        return -1;
    }

    /* Port */
    uint16_t port = 443;
    p = host_end;
    if (p < end && *p == ':') {
        p++;
        unsigned long n = 0;
        const char *digits_start = p;
        while (p < end && *p >= '0' && *p <= '9') {
            n = n * 10 + (unsigned long)(*p - '0');
            if (n > 65535) return -1;
            p++;
        }
        if (p == digits_start) return -1;  /* ':' with no digits */
        port = (uint16_t)n;
    }
    *port_out = port;

    /* Path (from '/' onward, minus fragment) */
    const char *path_start = p;
    const char *path_end   = end;
    for (const char *q = path_start; q < path_end; q++) {
        if (*q == '#') { path_end = q; break; }
    }

    if (path_start == path_end || *path_start != '/') {
        return copy_bounded(path_out, path_cap, "/", 1);
    }

    return copy_bounded(path_out, path_cap, path_start,
                        (size_t)(path_end - path_start));
}

int http_build_get(const char *host, const char *path,
                   char *out_buf, size_t out_cap)
{
    int n = snprintf(out_buf, out_cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: HelloHTTPS/1.0 (Certainly; classic Mac OS 9)\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host);
    if (n < 0 || (size_t)n >= out_cap) return -1;
    return n;
}

int http_split_response(const char *raw, size_t raw_len,
                        const char **status_line_out, size_t *status_len_out,
                        const char **headers_out,     size_t *headers_len_out,
                        const char **body_out,        size_t *body_len_out)
{
    if (raw == NULL || raw_len < 4) return -1;

    /* End of status line: first CRLF. */
    size_t i = 0;
    while (i + 1 < raw_len && !(raw[i] == '\r' && raw[i + 1] == '\n'))
        i++;
    if (i + 1 >= raw_len) return -1;

    const char *status     = raw;
    size_t      status_len = i;

    /* End of headers: first CRLFCRLF at or after the status line. */
    size_t j = i;
    while (j + 3 < raw_len &&
           !(raw[j]     == '\r' && raw[j + 1] == '\n' &&
             raw[j + 2] == '\r' && raw[j + 3] == '\n'))
        j++;
    if (j + 3 >= raw_len) return -1;

    *status_line_out = status;
    *status_len_out  = status_len;
    *headers_out     = raw + (i + 2);
    *headers_len_out = (j > (i + 2)) ? (j - (i + 2)) : 0;
    *body_out        = raw + (j + 4);
    *body_len_out    = raw_len - (j + 4);
    return 0;
}

/* Case-insensitive "prefix:" match against a header line. */
static int header_line_is(const char *line, size_t line_len,
                          const char *prefix)
{
    size_t plen = strlen(prefix);
    if (line_len < plen + 1) return 0;
    for (size_t i = 0; i < plen; i++) {
        char a = line[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return line[plen] == ':';
}

int http_headers_is_chunked(const char *headers, size_t headers_len)
{
    if (!headers || headers_len == 0) return 0;

    const char *p = headers;
    const char *e = headers + headers_len;
    while (p < e) {
        const char *line_end = p;
        while (line_end < e && *line_end != '\r' && *line_end != '\n')
            line_end++;
        size_t line_len = (size_t)(line_end - p);

        if (header_line_is(p, line_len, "Transfer-Encoding")) {
            const char *v = p + 18;   /* past "Transfer-Encoding:" */
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            if ((size_t)(line_end - v) >= 7) {
                const char *want = "chunked";
                int match = 1;
                for (int i = 0; i < 7; i++) {
                    char a = v[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (a != want[i]) { match = 0; break; }
                }
                if (match) return 1;
            }
        }

        if (line_end < e && *line_end == '\r') line_end++;
        if (line_end < e && *line_end == '\n') line_end++;
        p = line_end;
    }
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_decode_chunked(const char *in, size_t in_len,
                        char *out, size_t out_cap)
{
    const char *p = in;
    const char *e = in + in_len;
    size_t written = 0;

    while (p < e) {
        /* Hex chunk length. */
        size_t chunk_len = 0;
        int any_digits = 0;
        while (p < e) {
            int d = hex_digit(*p);
            if (d < 0) break;
            chunk_len = (chunk_len << 4) | (size_t)d;
            any_digits = 1;
            p++;
        }
        if (!any_digits) return -1;

        /* Skip optional chunk extension ("; foo=bar"). */
        while (p < e && *p != '\r' && *p != '\n') p++;

        /* Consume CRLF after the length line. */
        if (p < e && *p == '\r') p++;
        if (p < e && *p == '\n') p++;
        else return -1;

        if (chunk_len == 0) {
            /* Terminator chunk; trailers are ignored. */
            return (int)written;
        }

        if ((size_t)(e - p) < chunk_len) return -1;
        if (written + chunk_len > out_cap) return -1;

        memmove(out + written, p, chunk_len);
        written += chunk_len;
        p += chunk_len;

        /* Consume CRLF after the chunk data. */
        if (p < e && *p == '\r') p++;
        if (p < e && *p == '\n') p++;
    }

    /* End of input without a terminator chunk. */
    return -1;
}

long http_expected_total(const char *raw, size_t raw_len)
{
    /* Find the end of the headers. */
    int found = 0;
    size_t header_end = 0;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i]     == '\r' && raw[i + 1] == '\n' &&
            raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            header_end = i;
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    if (http_headers_is_chunked(raw, header_end)) return -1;

    /* Scan for Content-Length. */
    const char *p = raw;
    const char *e = raw + header_end;
    while (p < e) {
        const char *line_end = p;
        while (line_end < e && *line_end != '\r' && *line_end != '\n')
            line_end++;
        size_t line_len = (size_t)(line_end - p);

        if (header_line_is(p, line_len, "Content-Length")) {
            const char *v = p + 15;   /* past "Content-Length:" */
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            unsigned long cl = 0;
            int any = 0;
            while (v < line_end && *v >= '0' && *v <= '9') {
                cl = cl * 10 + (unsigned long)(*v - '0');
                if (cl > 100000000UL) return -1;
                any = 1;
                v++;
            }
            if (!any) return -1;
            return (long)(header_end + 4 + cl);
        }

        if (line_end < e && *line_end == '\r') line_end++;
        if (line_end < e && *line_end == '\n') line_end++;
        p = line_end;
    }
    return -1;
}
/* Chat clients send their own product token. A Sherclawk build defines
 * SHERCLAWK_APP; without it the shared builder keeps HelloChat's exact
 * request bytes. */
#ifdef SHERCLAWK_APP
#define HTTP_CHAT_USER_AGENT "Sherclawk/1.0 (Certainly; Mac OS 9)"
#else
#define HTTP_CHAT_USER_AGENT "HelloChat/1.0 (Certainly; Mac OS 9)"
#endif
/* Extra header lines must be complete CRLF-terminated lines (or NULL/empty)
 * so a caller-composed block cannot smuggle bare controls into the request. */
static int http_extra_headers_ok(const char *s)
{
    if (!s) return 1;
    while (*s) {
        if (*s == '\r') {
            if (s[1] != '\n') return 0;
            s += 2; continue;
        }
        if (*s == '\n' || ((unsigned char)*s < 0x20 && *s != '\t')) return 0;
        s++;
    }
    return 1;
}
/* Chat requests need a byte-exact Content-Length and safe header values. */
int http_build_post_with_headers(const char *host, const char *path, const char *key,
                                 const char *extra_headers,
                                 const char *body, size_t body_len, char *out, size_t cap)
{
    int n;
    if (!host || !path || !key || strpbrk(host, "\r\n") ||
        strpbrk(path, "\r\n ") || strpbrk(key, "\r\n") ||
        !http_extra_headers_ok(extra_headers)) return -1;
    n = snprintf(out, cap,
        "POST %s HTTP/1.1\r\nHost: %s\r\n%s"
        "User-Agent: " HTTP_CHAT_USER_AGENT "\r\n"
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
        "Accept: application/json\r\nAccept-Encoding: identity\r\n"
        "Connection: close\r\nContent-Length: %lu\r\n\r\n",
        path, host, extra_headers ? extra_headers : "",
        key, (unsigned long)body_len);
    if (n < 0 || (size_t)n >= cap || body_len >= cap - (size_t)n) return -1;
    memcpy(out + n, body, body_len);
    out[n + body_len] = 0;
    return n + (int)body_len;
}
int http_build_post(const char *host, const char *path, const char *key,
                    const char *body, size_t body_len, char *out, size_t cap)
{
    return http_build_post_with_headers(host, path, key, NULL, body, body_len, out, cap);
}

static int http_value_is(const char *p, size_t n, const char *want)
{
    size_t i;
    if (n != strlen(want)) return 0;
    for (i = 0; i < n; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != want[i]) return 0;
    }
    return 1;
}

static size_t http_crlf(const char *p, size_t n, size_t start)
{
    size_t i;
    for (i = start; i + 1 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n') return i;
    return n;
}

/* Walk chunks without copying until the whole response has been validated. */
static int http_chunks(const char *p, size_t n, size_t limit,
                       char *out, size_t *decoded)
{
    size_t at = 0, used = 0;
    for (;;) {
        size_t e = http_crlf(p, n, at), q, count = 0;
        int digits = 0;
        if (e == n) return 0;
        for (q = at; q < e && p[q] != ';'; q++) {
            int d = hex_digit(p[q]);
            if (d < 0 || count > limit / 16 ||
                count * 16 + (size_t)d > limit) return -1;
            count = count * 16 + (size_t)d;
            digits++;
        }
        if (!digits || used > limit - count) return -1;
        at = e + 2;
        if (!count) {
            /* Trailer section ends at an empty CRLF line. */
            for (;;) {
                e = http_crlf(p, n, at);
                if (e == n) return 0;
                if (e == at) { *decoded = used; return 1; }
                if (!memchr(p + at, ':', e - at)) return -1;
                at = e + 2;
            }
        }
        if (count > n - at || n - at - count < 2) return 0;
        if (p[at + count] != '\r' || p[at + count + 1] != '\n') return -1;
        if (out) memcpy(out + used, p + at, count);
        used += count;
        at += count + 2;
    }
}

int http_response(const char *raw, size_t len, int closed, size_t max_body,
                  int *status, char *out, size_t cap, size_t *body_len)
{
    const char *line, *headers, *body;
    size_t slen, hlen, blen, at = 0, content_len = 0, n = 0;
    int has_length = 0, chunked = 0, r;
    if (http_split_response(raw, len, &line, &slen, &headers, &hlen,
                            &body, &blen)) return closed ? -1 : 0;
    if (slen < 12 || (memcmp(line, "HTTP/1.1 ", 9) &&
                      memcmp(line, "HTTP/1.0 ", 9)) ||
        line[9] < '1' || line[9] > '5' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (slen > 12 && line[12] != ' ')) return -1;
    *status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    /* We never request Expect: 100-continue; interim responses are unsupported. */
    if (*status < 200) return -1;
    while (at < hlen) {
        size_t e = http_crlf(headers, hlen, at), v, ve;
        const char *colon;
        if (e == hlen) e = hlen;
        colon = memchr(headers + at, ':', e - at);
        if (!colon || colon == headers + at) return -1;
        v = (size_t)(colon - headers) + 1; ve = e;
        while (v < ve && (headers[v] == ' ' || headers[v] == '\t')) v++;
        while (ve > v && (headers[ve - 1] == ' ' || headers[ve - 1] == '\t')) ve--;
        if (header_line_is(headers + at, e - at, "Content-Length")) {
            size_t value = 0, i;
            if (v == ve) return -1;
            for (i = v; i < ve; i++) {
                int d = headers[i] - '0';
                if (d < 0 || d > 9 || value > max_body / 10 ||
                    value * 10 + (size_t)d > max_body) return -1;
                value = value * 10 + (size_t)d;
            }
            if (has_length && value != content_len) return -1;
            has_length = 1; content_len = value;
        } else if (header_line_is(headers + at, e - at, "Transfer-Encoding")) {
            if (chunked || !http_value_is(headers + v, ve - v, "chunked")) return -1;
            chunked = 1;
        } else if (header_line_is(headers + at, e - at, "Content-Encoding")) {
            if (!http_value_is(headers + v, ve - v, "identity")) return -1;
        }
        at = e + 2;
    }
    if (chunked && has_length) return -1;
    if (chunked) {
        r = http_chunks(body, blen, max_body, NULL, &n);
        if (r <= 0) return closed && r == 0 ? -1 : r;
    } else if (has_length) {
        if (blen < content_len) return closed ? -1 : 0;
        n = content_len;
    } else {
        if (blen > max_body) return -1;
        if (!closed) return 0;
        n = blen;
    }
    if (out) {
        if (n >= cap) return -1;
        if (chunked) http_chunks(body, blen, max_body, out, &n);
        else memcpy(out, body, n);
        out[n] = 0;
    }
    *body_len = n;
    return 1;
}
