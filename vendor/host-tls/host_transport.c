/*
 * host_transport.c — BSD-socket implementation of Certainly's OTTransport
 * interface, for running the TLS stack on the host.
 *
 * Semantics are kept close to src/ot_transport.c:
 *   - send/recv are non-blocking; recv returns 0 when no data is
 *     available, -1 when the peer has closed or an error occurred
 *   - a clean peer FIN sets ordRelReceived (OT's T_ORDREL)
 *   - a reset sets disconnectReceived (OT's T_DISCONNECT)
 *   - send on a non-connected transport returns -1 without touching
 *     lastError, matching the original
 *
 * Set HOST_TLS_DUMP=1 in the environment to hex-dump every send/recv.
 */
#include "ot_transport.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

typedef struct {
    OTTransport *t;
    int          fd;
    char         host[256];
    uint16_t     port;
    struct addrinfo *res;
} HostConn;

#define MAX_CONNS 8
static HostConn g_conns[MAX_CONNS];

static HostConn *conn_for(OTTransport *t)
{
    int i;
    for (i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].t == t) return &g_conns[i];
    }
    for (i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].t == NULL) {
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].t = t;
            g_conns[i].fd = -1;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void dump(const char *dir, const unsigned char *buf, size_t len)
{
    size_t i, n = len < 32 ? len : 32;
    if (!getenv("HOST_TLS_DUMP")) return;
    fprintf(stderr, "%s %zu bytes:", dir, len);
    for (i = 0; i < n; i++) fprintf(stderr, " %02x", buf[i]);
    if (len > n) fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

OTTransport *ot_transport_create(const char *host, uint16_t port)
{
    OTTransport *t = (OTTransport *)calloc(1, sizeof(OTTransport));
    HostConn    *c;

    if (t == NULL) return NULL;
    t->state = kOTTransport_Idle;
    t->port  = port;
    t->connect_start_ticks = (uint32_t)TickCount();

    c = conn_for(t);
    if (c == NULL) { free(t); return NULL; }
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;

    t->state = kOTTransport_ResolvingDNS;
    return t;
}

OTTransportState ot_transport_pump(OTTransport *t)
{
    HostConn *c = conn_for(t);

    if (c == NULL) { t->state = kOTTransport_Error; return t->state; }

    switch (t->state) {
    case kOTTransport_ResolvingDNS: {
        struct addrinfo hints;
        char portstr[16];
        int  rc, flags, on = 1;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)t->port);

        rc = getaddrinfo(c->host, portstr, &hints, &c->res);
        if (rc != 0 || c->res == NULL) {
            t->lastError = -3201;   /* fake kOTNotFoundErr-ish */
            t->state = kOTTransport_Error;
            break;
        }
        t->hostInfo.addrs[0] = 0;   /* not used by this shim */

        c->fd = socket(c->res->ai_family, SOCK_STREAM, 0);
        if (c->fd < 0) { t->lastError = -3155; t->state = kOTTransport_Error; break; }

        flags = fcntl(c->fd, F_GETFL, 0);
        fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
        setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_NOSIGPIPE
        setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

        t->connect_start_ticks = (uint32_t)TickCount();
        if (connect(c->fd, c->res->ai_addr, c->res->ai_addrlen) == 0) {
            t->connectComplete = true;
            t->state = kOTTransport_Connected;
        } else if (errno == EINPROGRESS) {
            t->state = kOTTransport_Connecting;
        } else {
            t->lastError = -3155;
            t->state = kOTTransport_Error;
        }
        break;
    }

    case kOTTransport_Connecting: {
        struct pollfd pfd = { c->fd, POLLOUT, 0 };
        if (poll(&pfd, 1, 0) > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err == 0) {
                t->connectComplete = true;
                t->state = kOTTransport_Connected;
            } else {
                t->lastError = -3155;
                t->state = kOTTransport_Error;
            }
        }
        break;
    }

    case kOTTransport_Connected: {
        /* OT delivers T_ORDREL when the peer sends FIN; emulate by
         * peeking, without consuming data. */
        char tmp;
        ssize_t n = recv(c->fd, &tmp, 1, MSG_PEEK);
        if (n == 0) {
            t->ordRelReceived = true;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            t->disconnectReceived = true;
            t->lastError = -3155;
        }
        break;
    }

    case kOTTransport_Closing: {
        char tmp;
        ssize_t n = recv(c->fd, &tmp, 1, MSG_PEEK);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            t->state = kOTTransport_Closed;
        }
        break;
    }

    default:
        break;
    }

    return t->state;
}

int ot_transport_send(OTTransport *t, const void *buf, size_t len)
{
    HostConn *c = conn_for(t);
    ssize_t   n;

    if (c == NULL || t->state != kOTTransport_Connected) return -1;

    dump("SEND", (const unsigned char *)buf, len);

    n = send(c->fd, buf, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;   /* like kOTFlowErr */
    if (errno == ECONNRESET || errno == EPIPE) {
        t->disconnectReceived = true;
        t->lastError = -3155;
        return -1;
    }
    t->lastError = -3155;
    return -1;
}

int ot_transport_recv(OTTransport *t, void *buf, size_t len)
{
    HostConn *c = conn_for(t);
    ssize_t   n;

    if (c == NULL) return -1;
    if (t->state != kOTTransport_Connected &&
        t->state != kOTTransport_Closing) return -1;

    n = recv(c->fd, buf, len, 0);
    if (n > 0) {
        dump("RECV", (const unsigned char *)buf, (size_t)n);
        t->dataAvailable = true;
        return (int)n;
    }
    if (n == 0) {
        t->ordRelReceived = true;
        return -1;               /* peer closed; no more data */
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        t->dataAvailable = false;
        if (t->ordRelReceived) return -1;
        return 0;                /* nothing available right now */
    }
    t->disconnectReceived = true;
    t->lastError = -3155;
    return -1;
}

void ot_transport_close(OTTransport *t)
{
    HostConn *c = conn_for(t);

    if (c == NULL || c->fd < 0) return;
    if (t->state == kOTTransport_Connected) {
        shutdown(c->fd, SHUT_WR);
        t->state = kOTTransport_Closing;
    }
}

void ot_transport_destroy(OTTransport *t)
{
    HostConn *c = conn_for(t);

    if (c != NULL) {
        if (c->fd >= 0) close(c->fd);
        if (c->res) freeaddrinfo(c->res);
        memset(c, 0, sizeof(*c));
        c->fd = -1;
    }
    free(t);
}