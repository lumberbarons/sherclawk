/*
 * http.h — minimal HTTP/1.1 helpers for "Hello, HTTPS".
 *
 * URL parsing, request building, response splitting and chunked
 * decoding are adapted from the Postman example in the Certainly
 * project (https://github.com/minorbug/certainly), MIT License,
 * Copyright (c) 2026 Matt Baker. The certainly-based app needs only
 * GET, so the method/body machinery is left out.
 */
#ifndef HELLO_HTTPS_HTTP_H
#define HELLO_HTTPS_HTTP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Parse https://host[:port]/path[?query][#fragment] into components.
 * Only the https scheme is accepted (Certainly is a TLS library).
 * Default port is 443; the fragment is stripped; a missing or empty
 * path is normalized to "/". host_out and path_out are written
 * NUL-terminated on success.
 *
 * Returns 0 on success, -1 on any failure. On failure the outputs
 * are unspecified — check the return code first.
 */
int http_parse_url(const char *url, size_t url_len,
                   char *host_out, size_t host_cap,
                   uint16_t *port_out,
                   char *path_out, size_t path_cap);

/*
 * Build a complete HTTP/1.1 GET request into out_buf. Emits Host:,
 * User-Agent:, Accept: and Connection: close. Returns the number of
 * bytes written, or -1 on overflow.
 */
int http_build_get(const char *host, const char *path,
                   char *out_buf, size_t out_cap);

/*
 * Split a raw HTTP/1.1 response into status line, headers and body
 * regions. All outputs point into `raw`; no copies are made.
 *
 * Returns 0 on success, -1 on failure (no status-line CRLF, or no
 * blank-line terminator yet).
 */
int http_split_response(const char *raw, size_t raw_len,
                        const char **status_line_out, size_t *status_len_out,
                        const char **headers_out,     size_t *headers_len_out,
                        const char **body_out,        size_t *body_len_out);

/*
 * Non-zero if the headers block carries "Transfer-Encoding: chunked"
 * (bare, no compound chains).
 */
int http_headers_is_chunked(const char *headers, size_t headers_len);

/*
 * Decode an HTTP/1.1 chunked body into out. Returns the number of
 * decoded bytes, or -1 on malformed input or overflow. Supports
 * out == in (in-place) since output is always shorter than input.
 */
int http_decode_chunked(const char *in, size_t in_len,
                        char *out, size_t out_cap);

/*
 * If the response headers are complete and carry a Content-Length,
 * return the total number of bytes the whole response will occupy
 * (status line + headers + body). Return -1 when the total cannot be
 * determined yet — headers incomplete, chunked transfer, or no
 * Content-Length — in which case the caller waits for server close.
 */
long http_expected_total(const char *raw, size_t raw_len);

/* Bounded POST builder. Rejects CR/LF in header values; body_len is bytes.
 * The User-Agent is HelloChat's unless the build defines SHERCLAWK_APP.
 * The with_headers variant inserts extra_headers (CRLF-terminated lines, or
 * NULL) after Host:; http_build_post sends none. */
int http_build_post(const char *host, const char *path, const char *key,
                    const char *body, size_t body_len, char *out, size_t cap);
int http_build_post_with_headers(const char *host, const char *path, const char *key,
                                 const char *extra_headers,
                                 const char *body, size_t body_len, char *out, size_t cap);
/* Incremental strict framing: 1 complete, 0 incomplete, -1 malformed.
 * closed permits close-delimited responses. max_body bounds declared sizes.
 * On completion, decode/copy the body into out (optional) and report status.
 * No compressed bodies or compound transfer encodings are supported. */
int http_response(const char *raw, size_t len, int closed, size_t max_body,
                  int *status, char *out, size_t cap, size_t *body_len);

#endif /* HELLO_HTTPS_HTTP_H */
