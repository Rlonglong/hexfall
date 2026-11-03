#include "net_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "netsim.h"
#include "util.h"

#define FLUSH_MAX_PACKETS 4

int hf_resolve(const char *host, const char *port, struct sockaddr_storage *out, socklen_t *len)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

int hf_client_open(hf_client *cl, const struct sockaddr *server, socklen_t len, hf_client_msg_fn fn,
                   void *user)
{
    memset(cl, 0, sizeof *cl);
    cl->fd = socket(server->sa_family, SOCK_DGRAM, 0);
    if (cl->fd < 0)
        return -1;
    /* A connected UDP socket: send() needs no address, and recv() only ever
     * returns packets from the server. */
    if (connect(cl->fd, server, len) != 0) {
        close(cl->fd);
        cl->fd = -1;
        return -1;
    }
    fcntl(cl->fd, F_SETFL, fcntl(cl->fd, F_GETFL) | O_NONBLOCK);
    rel_init(&cl->rel);
    cl->on_msg = fn;
    cl->user = user;
    cl->last_recv_ms = hf_now_ms();
    return 0;
}

void hf_client_send(hf_client *cl, const uint8_t *msg, size_t len)
{
    if (len < 3)
        return;
    if (hf_msg_is_reliable(msg[0]))
        rel_queue_reliable(&cl->rel, msg, len);
    else
        rel_queue_unreliable(&cl->rel, msg, len);
}

void hf_client_hello(hf_client *cl, const char *name, uint8_t kind, uint64_t resume_token)
{
    msg_hello h = {.version = HF_PROTOCOL_VERSION, .kind = kind, .resume_token = resume_token};
    snprintf(h.name, sizeof h.name, "%s", name);
    uint8_t buf[64];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_hello(&w, &h);
    if (!w.overflow)
        hf_client_send(cl, buf, w.len);
}

static void deliver(void *ctx, const uint8_t *msg, size_t len)
{
    hf_client *cl = ctx;
    rbuf r, payload;
    uint8_t type;
    rb_init(&r, msg, len);
    if (!hf_next_msg(&r, &type, &payload))
        return;
    if (type == MSG_WELCOME) {
        rbuf copy = payload;
        msg_welcome m;
        if (hf_get_welcome(&copy, &m)) {
            cl->token = m.token;
            cl->player_id = m.player_id;
            cl->welcomed = 1;
        }
    }
    if (cl->on_msg)
        cl->on_msg(cl, type, &payload, msg, len, cl->user);
}

ssize_t hf_client_recv_datagram(hf_client *cl, uint8_t *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(cl->fd, buf, cap, 0);
        if (n >= 0 || errno != EINTR)
            return n;  /* -1: EAGAIN, or ECONNREFUSED while nothing listens on the server port */
    }
}

void hf_client_process(hf_client *cl, const uint8_t *buf, size_t len, uint64_t now_ms)
{
    rbuf r;
    uint64_t session;
    rb_init(&r, buf, len);
    if (!hf_get_udp_header(&r, &session))
        return;
    cl->last_recv_ms = now_ms;
    cl->bytes_in += len;
    rel_read_packet(&cl->rel, now_ms, &r, deliver, cl);
}

void hf_client_receive(hf_client *cl, uint64_t now_ms)
{
    uint8_t buf[2048];
    ssize_t n;
    while ((n = hf_client_recv_datagram(cl, buf, sizeof buf)) >= 0)
        hf_client_process(cl, buf, (size_t)n, now_ms);
}

void hf_client_flush(hf_client *cl, uint64_t now_ms)
{
    for (int k = 0; k < FLUSH_MAX_PACKETS; k++) {
        if (k > 0 && !rel_wants_send(&cl->rel, now_ms))
            break;
        uint8_t pkt[HF_MAX_PACKET];
        wbuf w;
        wb_init(&w, pkt, sizeof pkt);
        hf_put_udp_header(&w, cl->welcomed ? cl->token : 0);
        rel_write_packet(&cl->rel, now_ms, &w);
        if (w.overflow)
            break;
        netsim_send(cl->fd, pkt, w.len, NULL, 0, now_ms);
        cl->bytes_out += w.len;
    }
    rel_drop_unreliable(&cl->rel);
}

void hf_client_close(hf_client *cl)
{
    if (cl->fd >= 0) {
        netsim_forget_fd(cl->fd);
        close(cl->fd);
    }
    cl->fd = -1;
}
