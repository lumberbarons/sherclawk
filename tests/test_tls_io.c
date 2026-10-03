/*
 * test_transport_pump.c — deterministic pump latency and partial-I/O checks.
 *
 * Fake non-blocking transport/engine boundaries isolate cooperative work
 * budgets and buffer ownership. They do not test TLS cryptography or OT.
 * EXPECT_BASELINE measures the pre-optimization event-loop delay.
 */
#include "certainly_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXPECT_BASELINE
#define EXPECT_BASELINE 0
#endif

static MacTLS_Context ctx;
static OTTransport transport;
static int send_result, recv_result, sends, recvs, steps, resets;
static int send_acks, recv_acks, mode, prepare_next;
static unsigned engine_state;
static tls13_hs_result step_result;
static unsigned char wire[65536];
static size_t wire_len;
static unsigned char engine_buf[32];
static unsigned char recvapp_data[32];
static size_t recvapp_len;
static int last_sendapp_ack, last_recvapp_ack, flushes;

unsigned long TickCount(void) { return 10; }
OTTransportState ot_transport_pump(OTTransport *t) { return t->state; }
int ot_transport_send(OTTransport *t, const void *buf, size_t len)
{
    int n = send_result;
    (void)t;
    sends++;
    if (n > (int)len) n = (int)len;
    if (n > 0) {
        assert(wire_len + (size_t)n <= sizeof(wire));
        memcpy(wire + wire_len, buf, (size_t)n);
        wire_len += (size_t)n;
    }
    return n;
}
int ot_transport_recv(OTTransport *t, void *buf, size_t len)
{
    (void)t;
    recvs++;
    if (recv_result > 0) {
        assert((size_t)recv_result <= len);
        memset(buf, 0x42, (size_t)recv_result);
    }
    return recv_result;
}
OTTransport *ot_transport_create(const char *host, uint16_t port)
{
    (void)host;
    transport.port = port;
    transport.state = kOTTransport_Connecting;
    return &transport;
}
void ot_transport_close(OTTransport *t) { (void)t; }
void ot_transport_destroy(OTTransport *t) { (void)t; }
tls13_hs_result tls13_handshake_step(tls13_hs_ctx *hs,
    unsigned char *buf, size_t *len, const char *host)
{
    (void)buf; (void)len; (void)host;
    steps++;
    /* The caller must finish sending before the next step reuses msg_buf. */
    assert(hs->msg_offset == hs->msg_len);
    if (prepare_next) {
        memcpy(hs->msg_buf, "next", 4);
        hs->msg_len = 4;
        hs->msg_offset = 0;
    }
    return step_result;
}
tls13_hs_result tls13_handle_post_handshake(tls13_hs_ctx *hs,
    const unsigned char *buf, size_t len)
{ (void)hs; (void)buf; (void)len; abort(); }
int tls13_record_decrypt(tls13_record_ctx *rc,
    const void *ct, size_t clen, void *pt,
    size_t *plen, uint8_t *inner_type)
{
    (void)rc; (void)ct; (void)clen; (void)pt; (void)plen; (void)inner_type;
    assert(clen <= 16384);
    memcpy(pt, ct, clen);
    *plen = clen; *inner_type = TLS13_CT_APPLICATION_DATA;
    return 0;
}
unsigned br_ssl_engine_current_state(const br_ssl_engine_context *cc)
{ (void)cc; return engine_state; }
unsigned char *br_ssl_engine_sendrec_buf(const br_ssl_engine_context *cc,
    size_t *len)
{ (void)cc; *len = 4; return engine_buf; }
unsigned char *br_ssl_engine_recvrec_buf(const br_ssl_engine_context *cc,
    size_t *len)
{ (void)cc; *len = sizeof(engine_buf); return engine_buf; }
void br_ssl_engine_sendrec_ack(br_ssl_engine_context *cc, size_t len)
{
    (void)cc;
    send_acks++;
    if (len == 4) engine_state = mode ? BR_SSL_CLOSED : BR_SSL_RECVREC;
}
void br_ssl_engine_recvrec_ack(br_ssl_engine_context *cc, size_t len)
{ (void)cc; (void)len; recv_acks++; engine_state = BR_SSL_SENDAPP; }
void br_ssl_engine_close(br_ssl_engine_context *cc)
{ (void)cc; engine_state = BR_SSL_CLOSED; }
int br_ssl_client_reset(br_ssl_client_context *cc, const char *host,
    int resume_session)
{ (void)cc; (void)host; (void)resume_session; resets++; return 1; }

static void reset(int tls13)
{
    memset(&ctx, 0, sizeof(ctx));
    memset(&transport, 0, sizeof(transport));
    ctx.transport = &transport;
    transport.state = kOTTransport_Connected;
    transport.port = 443;
    ctx.state = kMacTLS_Handshaking;
    ctx.tls13_started = tls13 != 0;
    ctx.hs13.state = kTLS13_RecvServerHello;
    ctx.handshake_start_ticks = 10;
    memcpy(ctx.hs13.msg_buf, "hello", 5);
    ctx.hs13.msg_len = tls13 ? 5 : 0;
    send_result = 32;
    recv_result = 0;
    sends = recvs = steps = resets = send_acks = recv_acks = mode = 0;
    prepare_next = 0;
    wire_len = 0;
    engine_state = BR_SSL_SENDREC;
    step_result = kTLS13_WantRead;
}

static void pump(void)
{
    int old_sends = sends, old_recvs = recvs, old_steps = steps;
    MacTLS_Pump(&ctx);
    assert(sends - old_sends <= 1);
    assert(recvs - old_recvs <= 1);
    assert(steps - old_steps <= 1);
}

int pump_regressions(void)
{
    int flush_pumps = 1, transition_pumps = 1;
    reset(1);
    prepare_next = 1;
    step_result = kTLS13_WantWrite;
    pump();
    if (EXPECT_BASELINE) { pump(); flush_pumps++; }
    assert(steps == 1 && sends == 1);
    assert(wire_len == 5 && memcmp(wire, "hello", 5) == 0);
    assert(ctx.hs13.msg_len == 4 && ctx.hs13.msg_offset == 0);

    reset(1);
    send_result = 2;
    pump();
    assert(ctx.hs13.msg_offset == 2 && !steps && !recvs);
    send_result = 0;
    pump();
    assert(ctx.hs13.msg_offset == 2 && !steps && !recvs);
    send_result = 32;
    pump();
    if (EXPECT_BASELINE) pump();
    assert(steps == 1 && wire_len == 5 && !memcmp(wire, "hello", 5));

    reset(1);
    send_result = -1;
    pump();
    assert(ctx.state == kMacTLS_Error && ctx.error == kMacTLS_ErrWrite);
    assert(!steps && !recvs && !wire_len);

    reset(1);
    ctx.hs13.state = kTLS13_Complete;
    pump();
    if (EXPECT_BASELINE) pump();
    assert(ctx.state == kMacTLS_Connected && ctx.tls13_active);

    if (!EXPECT_BASELINE) {
        /* The final client flight must drain fully before completion,
         * even if the server already half-closed its receive direction. */
        reset(1);
        ctx.hs13.state = kTLS13_Complete;
        transport.ordRelReceived = true;
        recv_result = -1;
        send_result = 2;
        pump();
        assert(ctx.state == kMacTLS_Handshaking && !ctx.tls13_active);
        assert(!recvs && !steps);
        send_result = 32;
        pump();
        assert(ctx.state == kMacTLS_Connected && ctx.tls13_active);
        assert(!recvs && !steps && wire_len == 5);
    }

    /* Full flush permits an immediate receive error, never premature step. */
    reset(1);
    recv_result = -1;
    pump();
    if (EXPECT_BASELINE) pump();
    assert(ctx.state == kMacTLS_Error && ctx.error == kMacTLS_ErrRead);
    assert(!steps);

    reset(1);
    step_result = kTLS13_Fallback12;
    pump();
    if (EXPECT_BASELINE) pump();
    assert(resets == 1 && !ctx.tls13_started && !ctx.tls13_active);
    assert(ctx.state == kMacTLS_Connecting && transport.port == 443);

    reset(0);
    recv_result = 3;
    pump();
    if (EXPECT_BASELINE) { pump(); transition_pumps++; }
    assert(sends == 1 && recvs == 1 && send_acks == 1 && recv_acks == 1);
    assert(ctx.state == kMacTLS_Connected);

    reset(0);
    send_result = 2;
    pump();
    assert(send_acks == 1 && !recvs && engine_state == BR_SSL_SENDREC);

    reset(0);
    send_result = 0;
    pump();
    assert(!send_acks && !recvs);

    reset(0);
    send_result = -1;
    pump();
    assert(ctx.state == kMacTLS_Error && ctx.error == kMacTLS_ErrWrite);
    assert(!send_acks && !recvs);

    reset(0);
    mode = 1;
    pump();
    assert(!recvs && engine_state == BR_SSL_CLOSED);

    printf("pump regressions passed; TLS13 full-flush transition: %d pump(s); "
           "TLS12 send-to-receive: %d pump(s)\n", flush_pumps, transition_pumps);
    return 0;
}

/* Application-I/O boundaries: deliberately fragment records and block sends.
 * Fake crypto isolates queue ownership; the existing suite checks AEAD bytes. */
static int encryptions;
int tls13_record_encrypt(tls13_record_ctx *rc, const void *pt, size_t plen,
    uint8_t type, void *ct, size_t *clen)
{
    (void)type;
    encryptions++; rc->seq++;
    memcpy(ct, pt, plen); *clen = plen; return 0;
}
unsigned char *br_ssl_engine_sendapp_buf(const br_ssl_engine_context *cc, size_t *len)
{ (void)cc; *len = 3; return engine_buf; }
void br_ssl_engine_sendapp_ack(br_ssl_engine_context *cc, size_t len)
{ (void)cc; send_acks++; last_sendapp_ack = (int)len; }
void br_ssl_engine_flush(br_ssl_engine_context *cc, int force)
{ (void)cc; (void)force; flushes++; }
unsigned char *br_ssl_engine_recvapp_buf(const br_ssl_engine_context *cc, size_t *len)
{ (void)cc; *len = recvapp_len; return recvapp_len ? recvapp_data : NULL; }
void br_ssl_engine_recvapp_ack(br_ssl_engine_context *cc, size_t len)
{ (void)cc; assert(len <= recvapp_len); last_recvapp_ack = (int)len; recvapp_len -= len; }
static void application_reset(void)
{
    reset(1); ctx.state = kMacTLS_Connected; ctx.tls13_active = true;
    encryptions = 0; ctx.hs13.msg_len = 0;
    last_sendapp_ack = last_recvapp_ack = flushes = 0;
    recvapp_len = 0;
}
int main(void)
{
    static char large[20000], received[16384];
    int old, n;
    size_t at;
    pump_regressions();
    application_reset();
    assert(MacTLS_Write(&ctx, "hello", 5) == 5 && encryptions == 1);
    assert(!sends && ctx.hs13.write_ctx.seq == 1);
    send_result = 2; pump(); /* Partial record header. */
    assert(ctx.tls13_send_offset == 2);
    assert(MacTLS_Write(&ctx, "next", 4) == 0 && encryptions == 1);
    send_result = 0; old = sends; pump();
    assert(sends == old + 1 && ctx.tls13_send_offset == 2);
    send_result = 4; pump(); /* End of header and partial ciphertext. */
    assert(ctx.tls13_send_offset == 6);
    send_result = 32; pump();
    assert(ctx.tls13_send_offset == ctx.tls13_send_len && wire_len == 10);
    assert(!memcmp(wire + 5, "hello", 5));
    assert(MacTLS_Write(&ctx, "next", 4) == 4 && encryptions == 2);
    pump(); assert(wire_len == 19 && !memcmp(wire + 15, "next", 4));
    assert(ctx.hs13.write_ctx.seq == 2);

    application_reset(); send_result = -1;
    assert(MacTLS_Write(&ctx, "hello", 5) == 5);
    pump(); assert(ctx.state == kMacTLS_Error && ctx.error == kMacTLS_ErrWrite);
    assert(MacTLS_Write(&ctx, "retry", 5) == -1 && encryptions == 1);

    application_reset(); memset(large, 'x', sizeof(large)); send_result = 65536;
    at = 0;
    while (at < sizeof(large)) {
        n = MacTLS_Write(&ctx, large + at, sizeof(large) - at);
        assert(n > 0); at += (size_t)n; pump();
    }
    assert(encryptions == 2 && wire_len == sizeof(large) + 10);
    assert(!memcmp(wire + 5, large, 16384));
    assert(!memcmp(wire + 16394, large + 16384, sizeof(large) - 16384));

    /* Two coalesced records totaling more than the 16 KB app buffer must be drained separately, without truncation. */
    application_reset();
    ctx.tls13_recv_buf[0] = TLS13_CT_APPLICATION_DATA;
    ctx.tls13_recv_buf[3] = 0x20; ctx.tls13_recv_buf[4] = 0;
    memset(ctx.tls13_recv_buf + 5, 'a', 8192);
    ctx.tls13_recv_buf[8197] = TLS13_CT_APPLICATION_DATA;
    ctx.tls13_recv_buf[8200] = 0x21; ctx.tls13_recv_buf[8201] = 0x34;
    memset(ctx.tls13_recv_buf + 8202, 'b', 8500);
    ctx.tls13_recv_len = 16702;
    pump(); assert(ctx.tls13_app_len == 8192 && ctx.tls13_recv_len == 8505);
    old = recvs; pump(); assert(recvs == old); /* Still undrained. */
    assert(MacTLS_Read(&ctx, received, sizeof(received)) == 8192 && received[0] == 'a' && received[8191] == 'a');
    pump(); assert(MacTLS_Read(&ctx, received, sizeof(received)) == 8500 && received[0] == 'b' && received[8499] == 'b');
    assert(ctx.tls13_recv_len == 0);
    application_reset(); ctx.tls13_active = false; engine_state = BR_SSL_SENDAPP;
    memset(engine_buf, 0xEE, sizeof(engine_buf));
    assert(MacTLS_Write(&ctx, "abcdef", 6) == 3 && send_acks == 1);
    assert(!memcmp(engine_buf, "abcdef", 3) && engine_buf[3] == 0xEE);
    assert(last_sendapp_ack == 3 && flushes == 1);

    /* TLS 1.2 read: plaintext leaves BearSSL's recvapp buffer and is acked. */
    memcpy(recvapp_data, "world", 5); recvapp_len = 5;
    assert(MacTLS_Read(&ctx, received, sizeof(received)) == 5);
    assert(!memcmp(received, "world", 5));
    assert(last_recvapp_ack == 5 && recvapp_len == 0);
    puts("PASS application TLS partial I/O, backpressure, record boundaries, receive draining");
    return 0;
}
