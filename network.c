/* Do bounded I/O per Toolbox tick, retaining request offsets and framing state.
 * Open Transport lifecycle and the wall-clock deadline belong to the caller. */
#include "network.h"
#include "http.h"
#include <stdio.h>
#include <string.h>
void network_close(ChatNetwork *n)
{
    if (n->ctx) MacTLS_Close(n->ctx);
    n->ctx = NULL;
    /* Discard the embedded authorization header when the exchange ends. */
    memset(n->request, 0, sizeof(n->request));
}
static int fail(ChatNetwork *n, const char *message)
{
    snprintf(n->error, sizeof(n->error), "%s", message);
    n->result = -1; return -1;
}
int network_start(ChatNetwork *n, const char *request, size_t len)
{
    if (n->ctx || len >= sizeof(n->request)) return -1;
    /* request may already be n->request; don't zero it until it is copied. */
    memmove(n->request, request, len);
    n->request_len = len; n->sent = n->received = n->body_len = 0;
    n->status = n->result = 0; n->error[0] = 0; n->version = kMacTLS_VersionUnknown;
    n->ctx = MacTLS_Create("openrouter.ai", 443);
    if (!n->ctx) return fail(n, "TLS context allocation failed.");
    return 0;
}
int network_step(ChatNetwork *n)
{
    MacTLS_State state;
    int i, framing;
    if (n->result) return n->result;
    if (!n->ctx) return fail(n, "No active TLS context.");
    state = MacTLS_Pump(n->ctx);
    if (state == kMacTLS_Error) {
        snprintf(n->error, sizeof(n->error), "TLS error: code=%d bssl=%d OT=%ld",
            MacTLS_GetError(n->ctx), MacTLS_GetBearSSLError(n->ctx), (long)MacTLS_GetOTError(n->ctx));
        n->result = -1; return -1;
    }
    if (state == kMacTLS_Connected) {
        n->version = MacTLS_GetVersion(n->ctx);
        if (n->sent < n->request_len) {
            int written = MacTLS_Write(n->ctx, n->request + n->sent, n->request_len - n->sent);
            if (written < 0) return fail(n, "TLS request write failed.");
            n->sent += (size_t)written;
        }
    }
    if (state != kMacTLS_Connected && state != kMacTLS_Closed) return 0;
    /* A small work budget keeps the cooperative UI responsive. */
    for (i = 0; i < 4; i++) {
        size_t space = CHAT_RESPONSE_CAP - n->received;
        int read;
        if (!space) break;
        read = MacTLS_Read(n->ctx, n->raw + n->received, space);
        if (read <= 0) break;
        n->received += (size_t)read;
    }
    framing = http_response(n->raw, n->received, state == kMacTLS_Closed,
        CHAT_RESPONSE_CAP, &n->status, n->body, sizeof(n->body), &n->body_len);
    if (framing < 0) return fail(n, "Malformed, incomplete, compressed, or oversized HTTP response.");
    if (framing > 0) {
        if (n->sent != n->request_len) return fail(n, "The server replied before the request was sent.");
        n->result = 1; return 1;
    }
    if (n->received == CHAT_RESPONSE_CAP) return fail(n, "HTTP response exceeds 64 KiB.");
    return 0;
}
