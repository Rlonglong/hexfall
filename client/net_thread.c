#include "net_thread.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clock_sync.h"
#include "net_client.h"
#include "util.h"

#define FLUSH_MS 33           /* at least one packet per tick: acks and heartbeat */
#define CONN_TIMEOUT_MS 4000  /* same as the server's */
#define RETRY_MS 3000         /* a handshake that gets no WELCOME starts over */
#define PING_FAST_MS 100      /* a quick burst after connecting so the clock settles fast */
#define PING_FAST_COUNT 5
#define PING_MS 500
#define WAIT_MAX_MS 100       /* wake up at least this often to run the state machine */
#define HELD_SLOTS 256        /* received packets held back by the inbound disturbance */
#define DRAIN_BATCH 32

typedef struct {
    int used;
    uint64_t due_ms;
    size_t len;
    uint8_t data[HF_MAX_PACKET];
} held_packet;

struct net_thread {
    /* Written by net_start before the thread exists; read-only afterwards. */
    struct sockaddr_storage addr;
    socklen_t addr_len;
    char name[HF_NAME_MAX + 1];
    uint8_t kind;
    pthread_t thread;
    int wake[2];  /* self-pipe: the main thread writes, the network thread polls */
    int ready;    /* INIT_* bits: what net_start managed to set up, so teardown undoes exactly that */

    /* Channels; each has its own lock (msg_queue.h). */
    mq_fifo outbox, events;
    mq_ring snapshots;

    /* Under status_lock: what the network thread publishes and what the main
     * thread asks for. Both sides only copy in and out. */
    pthread_mutex_t status_lock;
    net_status published;
    netsim_cfg inbound_request;
    int stop_request;

    /* Network thread only; no other thread reads or writes these. */
    hf_client client;
    net_status st;  /* working copy, published once per loop */
    clock_sync clock;
    netsim_cfg inbound;
    uint64_t state_since_ms, next_flush_ms, next_ping_ms, arrival_ms;
    int pings_sent;
    held_packet held[HELD_SLOTS];
    mq_item batch[DRAIN_BATCH];
};

enum { INIT_OUTBOX = 1, INIT_EVENTS = 2, INIT_RING = 4, INIT_STATUS = 8 };

const char *net_state_name(net_state s)
{
    switch (s) {
    case NET_CONNECTING: return "CONNECTING";
    case NET_CONNECTED: return "CONNECTED";
    case NET_RECONNECTING: return "RECONNECTING";
    case NET_RESYNCING: return "RESYNCING";
    case NET_STOPPED: return "STOPPED";
    default: return "?";
    }
}

/* ---- network thread ---- */

static void set_state(net_thread *n, net_state s, uint64_t now_ms)
{
    if (n->st.state != s)
        hf_log("net[%s]: %s -> %s", n->name, net_state_name(n->st.state), net_state_name(s));
    n->st.state = s;
    n->state_since_ms = now_ms;
}

static void on_message(hf_client *cl, uint8_t type, rbuf *payload, const uint8_t *msg, size_t len, void *user)
{
    net_thread *n = user;
    if (type == MSG_PONG) {
        msg_pong m;
        if (hf_get_pong(payload, &m))
            cs_add(&n->clock, m.client_ms, m.server_ms, (uint32_t)n->arrival_ms);
        return;
    }
    if (type == MSG_SNAPSHOT) {
        mq_ring_push(&n->snapshots, msg, len, n->arrival_ms);
        n->st.snapshots_received++;
        return;
    }
    if (!hf_msg_is_reliable(type) || n->st.state == NET_RESYNCING)
        return;  /* while resyncing, the main thread will get a fresh copy of everything */
    if (type == MSG_WELCOME) {
        n->st.token = cl->token;
        n->st.player_id = cl->player_id;
        n->st.sessions++;
        set_state(n, NET_CONNECTED, n->arrival_ms);
    }
    if (mq_fifo_push(&n->events, msg, len, n->arrival_ms) == 0) {
        n->st.events_queued++;
        return;
    }
    n->st.resyncs++;
    set_state(n, NET_RESYNCING, n->arrival_ms);
}

/* A new socket and a new session. With a token, the server hands back the same player. */
static void open_session(net_thread *n, net_state state, uint64_t now_ms)
{
    /* Packets held back from the old socket belong to the old session; fed to
     * the new one, their sequence numbers and acks would corrupt its state. */
    for (int i = 0; i < HELD_SLOTS; i++)
        n->held[i].used = 0;
    if (n->client.fd >= 0)
        hf_client_close(&n->client);
    if (hf_client_open(&n->client, (struct sockaddr *)&n->addr, n->addr_len, on_message, n) != 0) {
        hf_log("net[%s]: socket: %s", n->name, strerror(errno));
        n->client.fd = -1;  /* retried after RETRY_MS */
    } else {
        hf_client_hello(&n->client, n->name, n->kind, n->st.token);
    }
    if (state == NET_RECONNECTING)
        n->st.reconnects++;
    n->pings_sent = 0;
    n->next_ping_ms = now_ms;
    n->next_flush_ms = now_ms;
    set_state(n, state, now_ms);
}

static int hold_packet(net_thread *n, const uint8_t *buf, size_t len, uint64_t due_ms)
{
    for (int i = 0; i < HELD_SLOTS; i++) {
        held_packet *h = &n->held[i];
        if (h->used)
            continue;
        h->used = 1;
        h->due_ms = due_ms;
        h->len = len;
        memcpy(h->data, buf, len);
        return 1;
    }
    return 0;  /* all slots busy: let it through now */
}

static void receive(net_thread *n, uint64_t now_ms)
{
    uint8_t buf[2048];
    ssize_t len;
    while ((len = hf_client_recv_datagram(&n->client, buf, sizeof buf)) >= 0) {
        if ((size_t)len > HF_MAX_PACKET)
            continue;
        if (n->inbound.loss > 0 && hf_random01() < n->inbound.loss)
            continue;
        int delay = n->inbound.delay_ms + (n->inbound.jitter_ms > 0 ? (int)(hf_random01() * n->inbound.jitter_ms) : 0);
        if (delay > 0 && hold_packet(n, buf, (size_t)len, now_ms + (uint64_t)delay))
            continue;
        n->arrival_ms = now_ms;
        hf_client_process(&n->client, buf, (size_t)len, now_ms);
    }
}

static void release_held(net_thread *n, uint64_t now_ms)
{
    for (int i = 0; i < HELD_SLOTS; i++) {
        held_packet *h = &n->held[i];
        if (!h->used || h->due_ms > now_ms)
            continue;
        h->used = 0;
        n->arrival_ms = now_ms;  /* to the rest of the program it arrives now */
        hf_client_process(&n->client, h->data, h->len, now_ms);
    }
}

static int drain_outbox(net_thread *n)
{
    int total = 0;
    size_t got;
    while ((got = mq_fifo_pop(&n->outbox, n->batch, DRAIN_BATCH)) > 0) {
        for (size_t i = 0; i < got; i++)
            hf_client_send(&n->client, n->batch[i].data, n->batch[i].len);
        total += (int)got;
    }
    return total;
}

static void drain_pipe(net_thread *n)
{
    uint8_t buf[64];
    while (read(n->wake[0], buf, sizeof buf) > 0)
        ;
}

static void run_state_machine(net_thread *n, uint64_t now_ms)
{
    switch (n->st.state) {
    case NET_CONNECTING:
    case NET_RECONNECTING:
        if (now_ms - n->state_since_ms > RETRY_MS)
            open_session(n, n->st.state, now_ms);
        break;
    case NET_CONNECTED:
        if (now_ms - n->client.last_recv_ms > CONN_TIMEOUT_MS) {
            hf_log("net[%s]: no packets for %d s", n->name, CONN_TIMEOUT_MS / 1000);
            open_session(n, NET_RECONNECTING, now_ms);
        }
        break;
    case NET_RESYNCING:
        if (mq_fifo_count(&n->events) == 0)
            open_session(n, NET_RECONNECTING, now_ms);
        break;
    case NET_STOPPED:
        break;
    }
}

/* Returns 1 if a ping was queued. It must go out at once: t0 is taken now,
 * and any wait before sending would make the round trip lopsided. */
static int maybe_ping(net_thread *n, uint64_t now_ms)
{
    if (!n->client.welcomed || now_ms < n->next_ping_ms)
        return 0;
    msg_ping ping = {.client_ms = (uint32_t)now_ms};
    uint8_t buf[16];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_ping(&w, &ping);
    hf_client_send(&n->client, buf, w.len);
    n->pings_sent++;
    n->next_ping_ms = now_ms + (n->pings_sent < PING_FAST_COUNT ? PING_FAST_MS : PING_MS);
    return 1;
}

static int wait_ms(net_thread *n, uint64_t now_ms)
{
    uint64_t next = n->next_flush_ms;
    if (n->client.welcomed && n->next_ping_ms < next)
        next = n->next_ping_ms;
    for (int i = 0; i < HELD_SLOTS; i++)
        if (n->held[i].used && n->held[i].due_ms < next)
            next = n->held[i].due_ms;
    int delayed = netsim_wait_ms(now_ms);
    int w = next > now_ms ? (int)(next - now_ms) : 0;
    if (delayed >= 0 && delayed < w)
        w = delayed;
    return w > WAIT_MAX_MS ? WAIT_MAX_MS : w;
}

/* The one point per loop where the two threads meet: publish our status, pick
 * up the main thread's requests. Returns 1 when asked to stop. */
static int sync_with_main(net_thread *n)
{
    n->st.clock_valid = n->clock.valid;
    n->st.clock_offset = n->clock.offset;
    n->st.rtt_ms = n->clock.rtt_ms;
    n->st.rtt_min_ms = n->clock.rtt_min_ms;
    n->st.jitter_ms = n->clock.jitter_ms;
    n->st.packets_in = n->client.rel.packets_received;
    n->st.packets_out = n->client.rel.packets_sent;
    n->st.resends = n->client.rel.resends;
    n->st.outbox_refused = mq_fifo_refused(&n->outbox);  /* its own lock, never nested */
    n->st.inbound = n->inbound;

    pthread_mutex_lock(&n->status_lock);
    n->published = n->st;
    n->inbound = n->inbound_request;
    int stop = n->stop_request;
    pthread_mutex_unlock(&n->status_lock);
    return stop;
}

static void say_goodbye(net_thread *n)
{
    uint64_t now_ms = hf_now_ms();
    if (n->client.fd >= 0) {
        msg_bye bye = {0};
        uint8_t buf[16];
        wbuf w;
        wb_init(&w, buf, sizeof buf);
        hf_put_bye(&w, &bye);
        hf_client_send(&n->client, buf, w.len);
        hf_client_flush(&n->client, now_ms);
        netsim_flush(UINT64_MAX);  /* do not leave the goodbye in a delay queue */
        hf_client_close(&n->client);
    }
    netsim_release();
    set_state(n, NET_STOPPED, now_ms);
    sync_with_main(n);
}

static void *run(void *arg)
{
    net_thread *n = arg;
    open_session(n, NET_CONNECTING, hf_now_ms());
    while (!sync_with_main(n)) {
        uint64_t now_ms = hf_now_ms();
        struct pollfd p[2] = {
            {.fd = n->client.fd, .events = POLLIN},  /* a negative fd is ignored by poll */
            {.fd = n->wake[0], .events = POLLIN},
        };
        if (poll(p, 2, wait_ms(n, now_ms)) < 0 && errno != EINTR) {
            hf_log("net[%s]: poll: %s", n->name, strerror(errno));
            break;
        }
        now_ms = hf_now_ms();
        if (p[1].revents & POLLIN)
            drain_pipe(n);
        if (p[0].revents)
            receive(n, now_ms);
        release_held(n, now_ms);
        int queued = drain_outbox(n);
        run_state_machine(n, now_ms);
        queued += maybe_ping(n, now_ms);
        /* Send at once when something was queued (an input or a ping should
         * not wait for the next tick); otherwise once per tick for acks. */
        if (queued || now_ms >= n->next_flush_ms) {
            hf_client_flush(&n->client, now_ms);
            n->next_flush_ms = now_ms + FLUSH_MS;
        }
        netsim_flush(now_ms);
    }
    say_goodbye(n);
    return NULL;
}

/* ---- main thread side ---- */

static void wake_up(net_thread *n)
{
    uint8_t b = 1;
    ssize_t r = write(n->wake[1], &b, 1);  /* EAGAIN: a wake-up is already pending */
    (void)r;
}

static void destroy(net_thread *n)
{
    if (n->ready & INIT_OUTBOX)
        mq_fifo_destroy(&n->outbox);
    if (n->ready & INIT_EVENTS)
        mq_fifo_destroy(&n->events);
    if (n->ready & INIT_RING)
        mq_ring_destroy(&n->snapshots);
    if (n->ready & INIT_STATUS)
        pthread_mutex_destroy(&n->status_lock);
    for (int i = 0; i < 2; i++)
        if (n->wake[i] >= 0)
            close(n->wake[i]);
    free(n);
}

net_thread *net_start(const net_config *cfg)
{
    net_thread *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->client.fd = -1;
    n->wake[0] = n->wake[1] = -1;
    if (hf_resolve(cfg->host, cfg->port, &n->addr, &n->addr_len) != 0) {
        hf_log("net: cannot resolve %s:%s", cfg->host, cfg->port);
        free(n);
        return NULL;
    }
    snprintf(n->name, sizeof n->name, "%s", cfg->name);
    n->kind = cfg->kind;
    cs_init(&n->clock);
    n->st.state = NET_CONNECTING;
    n->published = n->st;

    if (pipe(n->wake) != 0) {
        n->wake[0] = n->wake[1] = -1;
        destroy(n);
        return NULL;
    }
    for (int i = 0; i < 2; i++)
        fcntl(n->wake[i], F_SETFL, fcntl(n->wake[i], F_GETFL) | O_NONBLOCK);
    size_t events = cfg->event_capacity ? cfg->event_capacity : 1024;
    if (mq_fifo_init(&n->outbox, 256) == 0)
        n->ready |= INIT_OUTBOX;
    if (mq_fifo_init(&n->events, events) == 0)
        n->ready |= INIT_EVENTS;
    if (mq_ring_init(&n->snapshots) == 0)
        n->ready |= INIT_RING;
    if (pthread_mutex_init(&n->status_lock, NULL) == 0)
        n->ready |= INIT_STATUS;
    if (n->ready != (INIT_OUTBOX | INIT_EVENTS | INIT_RING | INIT_STATUS)) {
        destroy(n);
        return NULL;
    }
    if (pthread_create(&n->thread, NULL, run, n) != 0) {
        destroy(n);
        return NULL;
    }
    return n;
}

void net_stop(net_thread *n)
{
    if (!n)
        return;
    pthread_mutex_lock(&n->status_lock);
    n->stop_request = 1;
    pthread_mutex_unlock(&n->status_lock);
    wake_up(n);
    pthread_join(n->thread, NULL);
    destroy(n);
}

void net_send(net_thread *n, const uint8_t *msg, size_t len)
{
    if (mq_fifo_push(&n->outbox, msg, len, 0) == 0)
        wake_up(n);
}

size_t net_events(net_thread *n, mq_item *out, size_t max) { return mq_fifo_pop(&n->events, out, max); }

size_t net_snapshots(net_thread *n, uint64_t *cursor, mq_item *out, size_t max, uint64_t *skipped)
{
    return mq_ring_read(&n->snapshots, cursor, out, max, skipped);
}

void net_get_status(net_thread *n, net_status *out)
{
    pthread_mutex_lock(&n->status_lock);
    *out = n->published;
    pthread_mutex_unlock(&n->status_lock);
}

void net_set_inbound(net_thread *n, netsim_cfg cfg)
{
    pthread_mutex_lock(&n->status_lock);
    n->inbound_request = cfg;
    pthread_mutex_unlock(&n->status_lock);
}
