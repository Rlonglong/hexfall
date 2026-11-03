#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "netsim.h"
#include "server.h"
#include "util.h"

#define FLUSH_MAX_PACKETS 4  /* per connection per tick, when there is a backlog */

static conn conns[MAX_CONNS];
static struct {
    uint64_t rx, tx, malformed, stale, refused;
} stats;

int udp_open(uint16_t port)
{
    /* Dual-stack when the OS allows it: IPv6 clients directly, IPv4 ones as
     * ::ffff:a.b.c.d mapped addresses. Falls back to plain IPv4. */
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd >= 0) {
        int off = 0;
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(port);
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off) != 0 ||
            bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return -1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return -1;
        }
    }
    int size = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family)
        return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a, *y = (const struct sockaddr_in *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a, *y = (const struct sockaddr_in6 *)b;
        return x->sin6_port == y->sin6_port && memcmp(&x->sin6_addr, &y->sin6_addr, 16) == 0;
    }
    return 0;
}

const char *conn_addr_str(const conn *c)
{
    static char ring[4][64];
    static int next;
    char *out = ring[next++ % 4];
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (c->addr.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&c->addr;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        port = ntohs(a->sin_port);
    } else if (c->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&c->addr;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
            inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], host, sizeof host);
        else
            inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        port = ntohs(a->sin6_port);
    }
    snprintf(out, 64, "%s:%u", host, port);
    return out;
}

static conn *find_by_token(uint64_t token)
{
    if (token == 0)
        return NULL;
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].token == token)
            return &conns[i];
    return NULL;
}

/* A client that has not received WELCOME yet sends session 0, so until then
 * its packets are matched by address. */
static conn *find_pending(const struct sockaddr_storage *from)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && !conns[i].token_confirmed && same_addr(&conns[i].addr, from))
            return &conns[i];
    return NULL;
}

static conn *conn_alloc(const struct sockaddr_storage *from, socklen_t from_len, uint64_t now_ms)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        conn *c = &conns[i];
        if (c->used)
            continue;
        uint64_t token;
        do
            token = hf_random_u64();
        while (token == 0 || find_by_token(token));
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->transport = TRANSPORT_UDP;
        c->token = token;
        c->player = -1;
        memcpy(&c->addr, from, from_len);
        c->addr_len = from_len;
        c->created_ms = c->last_recv_ms = now_ms;
        rel_init(&c->rel);
        return c;
    }
    return NULL;
}

void conn_send(conn *c, const uint8_t *msg, size_t len)
{
    if (!c || !c->used || len < 3)
        return;
    if (hf_msg_is_reliable(msg[0])) {
        if (rel_queue_reliable(&c->rel, msg, len) != 0 && !c->kill) {
            hf_log("%s stopped acknowledging; dropping it", conn_addr_str(c));
            c->kill = 1;
        }
    } else {
        rel_queue_unreliable(&c->rel, msg, len);
    }
}

void conn_close_soon(conn *c, uint64_t now_ms)
{
    if (c->used && !c->close_at_ms)
        c->close_at_ms = now_ms + 300;
}

typedef struct {
    conn *c;
    uint64_t now_ms;
} delivery;

static void deliver(void *ctx, const uint8_t *msg, size_t len)
{
    delivery *d = ctx;
    if (d->c->close_at_ms || d->c->kill)
        return;
    rbuf r;
    uint8_t type;
    rbuf payload;
    rb_init(&r, msg, len);
    if (hf_next_msg(&r, &type, &payload))
        room_on_message(d->c, type, &payload, d->now_ms);
}

static void handle_datagram(const uint8_t *buf, size_t n, const struct sockaddr_storage *from,
                            socklen_t from_len, uint64_t now_ms)
{
    rbuf r;
    uint64_t session;
    rb_init(&r, buf, n);
    if (n > HF_MAX_PACKET || !hf_get_udp_header(&r, &session)) {
        stats.malformed++;
        return;
    }
    conn *c;
    if (session == 0) {
        c = find_pending(from);
        if (!c) {
            c = conn_alloc(from, from_len, now_ms);
            if (!c) {
                stats.refused++;
                return;
            }
            hf_log("%s connected", conn_addr_str(c));
        }
    } else {
        c = find_by_token(session);
        if (!c) {
            stats.stale++;  /* a session we no longer know: the client must start over */
            return;
        }
        c->token_confirmed = 1;
        if (!same_addr(&c->addr, from)) {
            /* NAT rebinding or a network switch: the token proves who it is. */
            char before[64];
            snprintf(before, sizeof before, "%s", conn_addr_str(c));
            memcpy(&c->addr, from, from_len);
            c->addr_len = from_len;
            hf_log("%s moved to %s", before, conn_addr_str(c));
        }
    }
    c->last_recv_ms = now_ms;
    delivery d = {c, now_ms};
    rel_read_packet(&c->rel, now_ms, &r, deliver, &d);
}

void udp_receive(int fd, uint64_t now_ms)
{
    uint8_t buf[2048];
    for (;;) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;  /* EAGAIN: drained */
        }
        stats.rx++;
        handle_datagram(buf, (size_t)n, &from, from_len, now_ms);
    }
}

void udp_flush(int fd, uint64_t now_ms)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        conn *c = &conns[i];
        if (!c->used)
            continue;
        /* Always one packet per tick: it carries acks and doubles as a heartbeat. */
        for (int k = 0; k < FLUSH_MAX_PACKETS; k++) {
            if (k > 0 && !rel_wants_send(&c->rel, now_ms))
                break;
            uint8_t pkt[HF_MAX_PACKET];
            wbuf w;
            wb_init(&w, pkt, sizeof pkt);
            hf_put_udp_header(&w, c->token);
            rel_write_packet(&c->rel, now_ms, &w);
            if (w.overflow)
                break;
            netsim_send(fd, pkt, w.len, (struct sockaddr *)&c->addr, c->addr_len, now_ms);
            stats.tx++;
        }
        rel_drop_unreliable(&c->rel);
    }
}

void udp_maintain(uint64_t now_ms)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        conn *c = &conns[i];
        if (!c->used)
            continue;
        const char *why = NULL;
        if (c->kill)
            why = "dropped";
        else if (c->close_at_ms && now_ms >= c->close_at_ms)
            why = "closed";
        else if (c->player < 0 && !c->close_at_ms && now_ms - c->created_ms > HANDSHAKE_TIMEOUT_MS)
            why = "never said HELLO";
        else if (now_ms - c->last_recv_ms > CONN_TIMEOUT_MS)
            why = "timed out";
        if (!why)
            continue;
        if (c->player >= 0)
            room_on_conn_lost(c, now_ms);
        hf_log("%s %s", conn_addr_str(c), why);
        c->used = 0;
    }
}

void udp_stats(char *out, size_t cap)
{
    int n = 0;
    double rtt = 0;
    int rtt_n = 0;
    uint64_t resends = 0;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!conns[i].used)
            continue;
        n++;
        resends += conns[i].rel.resends;
        if (conns[i].rel.rtt_valid) {
            rtt += conns[i].rel.srtt_ms;
            rtt_n++;
        }
    }
    snprintf(out, cap, "udp conns=%d rtt=%.0fms rx=%llu tx=%llu resends=%llu bad=%llu stale=%llu",
             n, rtt_n ? rtt / rtt_n : 0.0, (unsigned long long)stats.rx, (unsigned long long)stats.tx,
             (unsigned long long)resends, (unsigned long long)stats.malformed,
             (unsigned long long)stats.stale);
}
