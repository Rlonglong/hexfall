#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net_thread.h"
#include "protocol.h"
#include "sim.h"
#include "util.h"

/* A headless stand-in for the game's main thread. It starts any number of
 * network threads, one per probe, and drives each the way the game will:
 * drain events and snapshots every frame, send READY in the lobby and an
 * INPUT every tick.
 *
 * With --stall it stops draining for a while once each round is under way,
 * as if the window were being dragged, so the event queue fills up and the
 * resync path runs. It checks what it receives and exits non-zero when
 * something is wrong, which makes it the stress test for net_thread.c. */

#define FRAME_US 16667
#define BATCH 32

typedef struct {
    net_thread *net;
    char name[HF_NAME_MAX + 1];
    int phase;
    uint16_t seq;
    uint64_t next_input_us;
    uint64_t snap_cursor, snapshots, skipped;
    uint64_t events, welcomes, order_errors, missing_board;
    int awaiting_board;  /* after WELCOME, TILES must come before any TILE_EVENTS */
    int have_tick;
    uint32_t last_event_tick;
    uint64_t stall_from_ms, stall_until_ms, stalls;
    int rounds;
    int errors;
} probe;

static volatile sig_atomic_t stop;
static mq_item batch[BATCH];

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void send_ready(probe *p)
{
    msg_ready m = {.ready = 1};
    uint8_t buf[16];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_ready(&w, &m);
    net_send(p->net, buf, w.len);
}

static void send_idle_input(probe *p)
{
    msg_input in = {.count = 1};
    in.s[0] = (input_sample){.seq = ++p->seq};
    uint8_t buf[32];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_input(&w, &in);
    net_send(p->net, buf, w.len);
}

static void handle_event(probe *p, const mq_item *it, uint16_t me, int stall_for_ms, uint64_t now_ms)
{
    rbuf r, payload;
    uint8_t type;
    rb_init(&r, it->data, it->len);
    if (!hf_next_msg(&r, &type, &payload))
        return;
    p->events++;
    switch (type) {
    case MSG_WELCOME:
        p->welcomes++;
        p->awaiting_board = 1;  /* a new session: forget the old view entirely */
        p->have_tick = 0;
        break;
    case MSG_TILES: {
        msg_tiles m;
        if (hf_get_tiles(&payload, &m)) {
            p->awaiting_board = 0;
            p->have_tick = 1;
            p->last_event_tick = m.tick;
        }
        break;
    }
    case MSG_TILE_EVENTS: {
        msg_tile_events m;
        if (!hf_get_tile_events(&payload, &m))
            break;
        if (p->awaiting_board)
            p->missing_board++;
        if (p->have_tick && (int32_t)(m.tick - p->last_event_tick) < 0)
            p->order_errors++;
        p->have_tick = 1;
        p->last_event_tick = m.tick;
        break;
    }
    case MSG_ROOM: {
        msg_room m;
        if (!hf_get_room(&payload, &m))
            break;
        int before = p->phase;
        p->phase = m.phase;
        for (int i = 0; i < m.count; i++)
            if (m.p[i].id == me && m.phase == PHASE_LOBBY && !(m.p[i].flags & RP_READY))
                send_ready(p);
        if (m.phase == PHASE_PLAYING && before != PHASE_PLAYING) {
            p->rounds++;
            if (stall_for_ms > 0) {
                p->stall_from_ms = now_ms + 1000;
                p->stall_until_ms = p->stall_from_ms + (uint64_t)stall_for_ms;
            }
        }
        break;
    }
    case MSG_ERROR:
        p->errors++;
        break;
    default:
        break;
    }
}

static void frame(probe *p, int stall_for_ms, uint64_t now_us)
{
    uint64_t now_ms = now_us / 1000;
    if (now_ms >= p->stall_from_ms && now_ms < p->stall_until_ms) {
        if (now_ms - p->stall_from_ms < 20)
            p->stalls++;
        return;  /* frozen: drains nothing, sends nothing */
    }
    net_status st;
    net_get_status(p->net, &st);
    size_t got;
    while ((got = net_events(p->net, batch, BATCH)) > 0)
        for (size_t i = 0; i < got; i++)
            handle_event(p, &batch[i], st.player_id, stall_for_ms, now_ms);
    while ((got = net_snapshots(p->net, &p->snap_cursor, batch, BATCH, &p->skipped)) > 0)
        p->snapshots += got;
    if (st.state == NET_CONNECTED && now_us >= p->next_input_us) {
        send_idle_input(p);
        p->next_input_us = now_us + 1000000 / SIM_TICK_HZ;
    }
}

static int summarize(probe *probes, int count, int final)
{
    int by_state[NET_STOPPED + 1] = {0}, clock_valid = 0, problems = 0;
    uint64_t sessions = 0, resyncs = 0, reconnects = 0, events = 0, snaps = 0, skipped = 0, stalls = 0;
    uint64_t order = 0, missing = 0, refused = 0;
    double rtt = 0, jitter = 0;
    int32_t off_min = 0, off_max = 0;
    for (int i = 0; i < count; i++) {
        probe *p = &probes[i];
        net_status st;
        net_get_status(p->net, &st);
        by_state[st.state]++;
        sessions += st.sessions;
        resyncs += st.resyncs;
        reconnects += st.reconnects;
        refused += st.outbox_refused;
        events += p->events;
        snaps += p->snapshots;
        skipped += p->skipped;
        stalls += p->stalls;
        order += p->order_errors;
        missing += p->missing_board;
        problems += p->errors;
        if (st.clock_valid) {
            int32_t off = (int32_t)st.clock_offset;  /* same machine: should be about 0 */
            off_min = clock_valid ? (off < off_min ? off : off_min) : off;
            off_max = clock_valid ? (off > off_max ? off : off_max) : off;
            clock_valid++;
            rtt += st.rtt_min_ms;
            jitter += st.jitter_ms;
        }
    }
    hf_log("probes: connected %d, connecting %d, reconnecting %d, resyncing %d | sessions %llu, resyncs %llu, "
           "reconnects %llu, stalls %llu",
           by_state[NET_CONNECTED], by_state[NET_CONNECTING], by_state[NET_RECONNECTING], by_state[NET_RESYNCING],
           (unsigned long long)sessions, (unsigned long long)resyncs, (unsigned long long)reconnects,
           (unsigned long long)stalls);
    hf_log("        events %llu, snapshots %llu (skipped %llu), out of order %llu, events before board %llu, "
           "clock offset %d..%d ms over %d probes, min rtt %.1f ms, jitter %.1f ms",
           (unsigned long long)events, (unsigned long long)snaps, (unsigned long long)skipped,
           (unsigned long long)order, (unsigned long long)missing, off_min, off_max, clock_valid,
           clock_valid ? rtt / clock_valid : 0.0, clock_valid ? jitter / clock_valid : 0.0);
    if (!final)
        return 0;

    if (by_state[NET_CONNECTED] != count) {
        hf_log("FAIL: %d of %d probes are not connected at the end", count - by_state[NET_CONNECTED], count);
        problems++;
    }
    if (order || missing) {
        hf_log("FAIL: reliable events out of order or before the board");
        problems++;
    }
    if (stalls > 0 && resyncs == 0) {
        hf_log("FAIL: probes stalled but no resync happened; raise --stall or lower --events");
        problems++;
    }
    if (sessions < (uint64_t)count + resyncs) {
        hf_log("FAIL: %llu resyncs but only %llu sessions", (unsigned long long)resyncs, (unsigned long long)sessions);
        problems++;
    }
    if (clock_valid && (off_min < -5 || off_max > 5)) {
        hf_log("FAIL: clock offsets %d..%d ms; on one machine they should be within 5 ms of 0", off_min, off_max);
        problems++;
    }
    if (refused) {
        hf_log("FAIL: %llu outgoing messages refused", (unsigned long long)refused);
        problems++;
    }
    hf_log(problems ? "netprobe: FAILED" : "netprobe: all checks passed");
    return problems;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--host H] [--port P] [-n PROBES] [--events CAPACITY] [--stall MS]\n"
            "          [--duration SEC] [--in-loss PERCENT] [--in-delay MS] [--in-jitter MS]\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *port = "8080";
    int count = 8, stall_ms = 0;
    size_t events = 0;
    double duration = 30;
    netsim_cfg inbound = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && v)
            host = argv[++i];
        else if (!strcmp(a, "--port") && v)
            port = argv[++i];
        else if (!strcmp(a, "-n") && v)
            count = atoi(argv[++i]);
        else if (!strcmp(a, "--events") && v)
            events = (size_t)atoi(argv[++i]);
        else if (!strcmp(a, "--stall") && v)
            stall_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--duration") && v)
            duration = atof(argv[++i]);
        else if (!strcmp(a, "--in-loss") && v)
            inbound.loss = atof(argv[++i]) / 100.0;
        else if (!strcmp(a, "--in-delay") && v)
            inbound.delay_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--in-jitter") && v)
            inbound.jitter_ms = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (count < 1 || count > HF_MAX_PLAYERS) {
        usage(argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    probe *probes = calloc((size_t)count, sizeof *probes);
    if (!probes)
        return 1;
    for (int i = 0; i < count; i++) {
        probe *p = &probes[i];
        snprintf(p->name, sizeof p->name, "probe%02d", i + 1);
        net_config cfg = {.host = host, .port = port, .name = p->name, .kind = CLIENT_BOT, .event_capacity = events};
        if (!(p->net = net_start(&cfg))) {
            fprintf(stderr, "cannot start the network thread for %s\n", p->name);
            return 1;
        }
        net_set_inbound(p->net, inbound);
    }
    hf_log("%d probes (%d network threads) on %s:%s, event queue %zu, stall %d ms", count, count, host, port,
           events ? events : (size_t)1024, stall_ms);

    uint64_t start = hf_now_us(), next_report = start + 5000000;
    while (!stop && (double)(hf_now_us() - start) < duration * 1e6) {
        uint64_t now = hf_now_us();
        for (int i = 0; i < count; i++)
            frame(&probes[i], stall_ms, now);
        if (now >= next_report) {
            summarize(probes, count, 0);
            next_report = now + 5000000;
        }
        usleep(FRAME_US);
    }
    int problems = summarize(probes, count, 1);
    for (int i = 0; i < count; i++)
        net_stop(probes[i].net);
    free(probes);
    return problems ? 1 : 0;
}
