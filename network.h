/* Cooperative HTTPS exchange, shared by the app and live diagnostic rig. */
#ifndef SHERCLAWK_NETWORK_H
#define SHERCLAWK_NETWORK_H
#include "certainly.h"
#include "chat.h"
typedef struct {
    MacTLS_Context *ctx;
    char request[CHAT_REQUEST_CAP + 2048];
    char raw[CHAT_RESPONSE_CAP + 1], body[CHAT_RESPONSE_CAP + 1];
    size_t request_len, sent, received, body_len;
    int status, result; /* 0 active, 1 complete, -1 failed */
    MacTLS_Version version;
    char error[256];
} ChatNetwork;
int network_start(ChatNetwork *n, const char *request, size_t len);
int network_start_at(ChatNetwork *n, const char *host, unsigned short port,
                     const char *request, size_t len);
int network_step(ChatNetwork *n);
void network_close(ChatNetwork *n);
#endif
