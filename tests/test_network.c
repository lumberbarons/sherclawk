/* Fake Certainly boundaries verify request offsets and failure recovery. */
#include "network.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct MacTLS_Context { int unused; };
static MacTLS_Context context;
static ChatNetwork net;
static int pumps, writes, closes, write_result, read_once, create_fail;
static MacTLS_State state;
static size_t wire_len, response_at;
static char wire[128];
static const char *response;
static const char *destination = "openrouter.ai";
static uint16_t destination_port = 443;
MacTLS_Context *MacTLS_Create(const char *host, uint16_t port)
{ assert(!strcmp(host, destination) && port == destination_port); return create_fail ? NULL : &context; }
MacTLS_State MacTLS_Pump(MacTLS_Context *c)
{ assert(c == &context); pumps++; read_once = 0; return state; }
int MacTLS_Write(MacTLS_Context *c, const void *data, size_t len)
{
    int n = write_result;
    assert(c == &context); writes++;
    if (n > (int)len) n = (int)len;
    if (n > 0) { memcpy(wire + wire_len, data, (size_t)n); wire_len += (size_t)n; }
    return n;
}
int MacTLS_Read(MacTLS_Context *c, void *out, size_t len)
{
    size_t n;
    assert(c == &context);
    if (read_once++ || !response || wire_len != net.request_len) return 0;
    n = strlen(response) - response_at;
    if (n > 3) n = 3;
    if (n > len) n = len;
    memcpy(out, response + response_at, n); response_at += n;
    return (int)n;
}
void MacTLS_Close(MacTLS_Context *c) { assert(c == &context); closes++; }
/* Distinguishable sentinels: the formatted error must carry all three fields. */
MacTLS_Error MacTLS_GetError(const MacTLS_Context *c) { (void)c; return kMacTLS_ErrRead; }
int MacTLS_GetBearSSLError(const MacTLS_Context *c) { (void)c; return -4242; }
OSStatus MacTLS_GetOTError(const MacTLS_Context *c) { (void)c; return -3155; }
MacTLS_Version MacTLS_GetVersion(const MacTLS_Context *c) { (void)c; return kMacTLS_Version12; }
static void reset(void)
{
    memset(&net, 0, sizeof(net)); memset(wire, 0, sizeof(wire));
    pumps = writes = closes = response_at = wire_len = create_fail = 0;
    state = kMacTLS_Connected; response = NULL; write_result = 3;
}
int main(void)
{
    int i;
    reset(); assert(network_start(&net, "request", 7) == 0);
    state = kMacTLS_Handshaking;
    assert(network_step(&net) == 0 && !writes && pumps == 1);
    state = kMacTLS_Connected;
    assert(network_step(&net) == 0 && net.sent == 3 && pumps == 2);
    write_result = 0; assert(network_step(&net) == 0 && net.sent == 3);
    write_result = 3; assert(network_step(&net) == 0 && net.sent == 6);
    response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    for (i = 0; i < 100 && !net.result; i++) network_step(&net);
    assert(net.result == 1 && net.sent == 7 && !memcmp(wire, "request", 7));
    assert(net.status == 200 && net.body_len == 2 && !strcmp(net.body, "{}"));
    network_close(&net); assert(closes == 1 && !net.ctx);
    for (i = 0; i < (int)sizeof(net.request); i++) assert(!net.request[i]);
    reset(); assert(network_start(&net, "x", 1) == 0); write_result = -1;
    assert(network_step(&net) == -1 && strstr(net.error, "write") && pumps == 1); network_close(&net);
    assert(network_start(&net, "x", 1) == 0); /* Reuse after failure. */
    state = kMacTLS_Error;
    assert(network_step(&net) == -1 && pumps == 2);
    assert(strstr(net.error, "TLS error: code=6 bssl=-4242 OT=-3155")); network_close(&net);
    reset(); create_fail = 1; assert(network_start(&net, "x", 1) == -1);
    reset(); assert(network_start(&net, "x", 1) == 0);
    state = kMacTLS_Closed; assert(network_step(&net) == -1); network_close(&net);
    reset(); destination = "mcp.example"; destination_port = 8443;
    assert(network_start_at(&net, destination, destination_port, "x", 1) == 0);
    network_close(&net); assert(closes == 1);
    puts("PASS network request offsets, backpressure, response assembly and recovery");
    return 0;
}
