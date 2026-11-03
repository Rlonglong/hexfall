#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_client.h"
#include "netsim.h"
#include "protocol.h"
#include "sim.h"
#include "util.h"

/* Headless players. One process runs any number of them, each with its own
 * UDP socket, so it doubles as a load generator: hexfall-bot -n 100. They
 * play from what the server tells them: the board from TILES/TILE_EVENTS and
 * their own position from snapshots. */

#define THINK_TICKS 6
#define TWO_PI 6.2831853f

typedef struct {
    hf_client net;
    char name[HF_NAME_MAX + 1];
    uint64_t now_ms;
    int phase;
    sim_arena arena;
    int have_snap;
    uint32_t snap_tick;
    int have_self, alive, grounded, layer;
    float x, y, z;
    float dir, noise;
    int think;
    uint16_t seq;
    input_sample hist[HF_INPUT_REDUNDANCY];
    int hist_n;
    uint64_t next_ping_ms;
    double rtt_ms;
    int have_rtt;
    int place, rounds, wins;
    int failed;
} bot;

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void send_ready(bot *b)
{
    msg_ready m = {.ready = 1};
    uint8_t buf[16];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_ready(&w, &m);
    hf_client_send(&b->net, buf, w.len);
}

static void apply_tiles(bot *b, const msg_tiles *m)
{
    sim_arena_reset(&b->arena);
    int bit = 0;
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++, bit++)
            if (bit / 8 < m->gone_bytes && (m->gone[bit / 8] & (0x80 >> (bit % 8))))
                b->arena.tiles[layer][i].state = TILE_GONE;
    for (int k = 0; k < m->touched_count; k++) {
        const touched_tile *t = &m->touched[k];
        int i = sim_tile_index(t->t.q, t->t.r);
        if (i < 0 || t->t.layer >= SIM_LAYERS)
            continue;
        b->arena.tiles[t->t.layer][i].state = TILE_TOUCHED;
        b->arena.tiles[t->t.layer][i].touch_tick = t->touch_tick;
    }
}

static void on_msg(hf_client *cl, uint8_t type, rbuf *p, const uint8_t *msg, size_t len, void *user)
{
    bot *b = user;
    (void)msg;
    (void)len;
    switch (type) {
    case MSG_WELCOME:
        send_ready(b);
        break;
    case MSG_ROOM: {
        msg_room m;
        if (!hf_get_room(p, &m))
            break;
        int before = b->phase;
        b->phase = m.phase;
        for (int i = 0; i < m.count; i++) {
            if (m.p[i].id != cl->player_id)
                continue;
            b->place = m.p[i].place;
            if (m.phase == PHASE_LOBBY && !(m.p[i].flags & RP_READY))
                send_ready(b);
        }
        if (before == PHASE_PLAYING && m.phase == PHASE_RESULT) {
            b->rounds++;
            b->wins += b->place == 1;
        }
        if (m.phase == PHASE_PLAYING && before != PHASE_PLAYING)
            b->have_self = 0;
        break;
    }
    case MSG_TILES: {
        msg_tiles m;
        if (hf_get_tiles(p, &m))
            apply_tiles(b, &m);
        break;
    }
    case MSG_TILE_EVENTS: {
        msg_tile_events m;
        if (!hf_get_tile_events(p, &m))
            break;
        for (int k = 0; k < m.count; k++)
            sim_touch(&b->arena, m.t[k].layer, m.t[k].q, m.t[k].r, m.tick, NULL, NULL);
        break;
    }
    case MSG_SNAPSHOT: {
        msg_snapshot m;
        if (!hf_get_snapshot(p, &m))
            break;
        if (b->have_snap && (int32_t)(m.tick - b->snap_tick) <= 0)
            break;  /* arrived after a newer one */
        b->have_snap = 1;
        b->snap_tick = m.tick;
        sim_arena_tick(&b->arena, m.tick);
        for (int i = 0; i < m.count; i++) {
            const snap_player *s = &m.p[i];
            if (s->id != cl->player_id)
                continue;
            b->have_self = 1;
            b->x = s->x / HF_POS_SCALE;
            b->y = s->y / HF_POS_SCALE;
            b->z = s->z / HF_POS_SCALE;
            b->layer = s->layer;
            b->alive = (s->flags & SP_ALIVE) != 0;
            b->grounded = (s->flags & SP_GROUNDED) != 0;
        }
        break;
    }
    case MSG_PONG: {
        msg_pong m;
        if (hf_get_pong(p, &m)) {
            b->rtt_ms = (double)(uint32_t)((uint32_t)b->now_ms - m.client_ms);
            b->have_rtt = 1;
        }
        break;
    }
    case MSG_ERROR: {
        msg_error m;
        if (hf_get_error(p, &m))
            hf_log("%s: server says %s", b->name, m.text);
        b->failed = 1;
        break;
    }
    default:
        break;
    }
}

/* -1: no floor there, 0: solid, 1: touched (about to fall). */
static int floor_at(const bot *b, float x, float z)
{
    int state = sim_tile_state_at(&b->arena, b->layer, x, z);
    return state < 0 || state == TILE_GONE ? -1 : state == TILE_TOUCHED;
}

/* Pick the direction with the most untouched floor ahead, with a pull toward
 * the middle, some stubbornness and some noise so bots differ. */
static void think(bot *b)
{
    float best_dir = b->dir, best = -1e9f;
    for (int k = 0; k < 12; k++) {
        float a = (float)k * TWO_PI / 12.0f, dx = sinf(a), dz = cosf(a), score = 0;
        for (int step = 1; step <= 3; step++) {
            int f = floor_at(b, b->x + dx * (float)step * 2.4f, b->z + dz * (float)step * 2.4f);
            score += f < 0 ? -1.6f : f ? 0.08f : 1.2f / (float)step;
        }
        score -= hypotf(b->x + dx * 4.0f, b->z + dz * 4.0f) * 0.025f;
        score += cosf(a - b->dir) * 0.45f + (float)hf_random01() * b->noise;
        if (score > best) {
            best = score;
            best_dir = a;
        }
    }
    b->dir = best_dir;
}

static void bot_tick(bot *b)
{
    if (!b->net.welcomed || b->failed)
        return;
    uint8_t buf[64];
    wbuf w;
    if (b->now_ms >= b->next_ping_ms) {
        msg_ping ping = {.client_ms = (uint32_t)b->now_ms};
        wb_init(&w, buf, sizeof buf);
        hf_put_ping(&w, &ping);
        hf_client_send(&b->net, buf, w.len);
        b->next_ping_ms = b->now_ms + 1000;
    }

    float dx = 0, dz = 0;
    uint8_t buttons = 0;
    if (b->phase == PHASE_PLAYING && b->have_self && b->alive) {
        if (b->grounded && --b->think <= 0) {
            think(b);
            b->think = THINK_TICKS;
        }
        dx = sinf(b->dir);
        dz = cosf(b->dir);
        if (b->grounded) {
            int ahead = floor_at(b, b->x + dx * 1.7f, b->z + dz * 1.7f);
            int beyond = floor_at(b, b->x + dx * 4.4f, b->z + dz * 4.4f);
            if ((ahead < 0 && beyond >= 0) || hf_random01() < 0.006)
                buttons |= BTN_JUMP;
        }
    }
    /* Newest first; the older samples ride along so one lost packet costs nothing. */
    memmove(&b->hist[1], &b->hist[0], sizeof b->hist[0] * (HF_INPUT_REDUNDANCY - 1));
    b->hist[0] = (input_sample){
        .seq = ++b->seq,
        .mx = (int8_t)lrintf(dx * 127.0f),
        .mz = (int8_t)lrintf(dz * 127.0f),
        .buttons = buttons,
    };
    if (b->hist_n < HF_INPUT_REDUNDANCY)
        b->hist_n++;
    msg_input in = {.count = (uint8_t)b->hist_n};
    memcpy(in.s, b->hist, sizeof in.s);
    wb_init(&w, buf, sizeof buf);
    hf_put_input(&w, &in);
    hf_client_send(&b->net, buf, w.len);
}

static void print_stats(bot *bots, int count)
{
    int welcomed = 0, alive = 0, rtt_n = 0, rounds = 0, wins = 0;
    double rtt = 0;
    unsigned long long resends = 0;
    for (int i = 0; i < count; i++) {
        bot *b = &bots[i];
        welcomed += b->net.welcomed;
        alive += b->phase == PHASE_PLAYING && b->have_self && b->alive;
        if (b->have_rtt) {
            rtt += b->rtt_ms;
            rtt_n++;
        }
        resends += b->net.rel.resends;
        rounds += b->rounds;
        wins += b->wins;
    }
    static const char *names[] = {"LOBBY", "COUNTDOWN", "PLAYING", "RESULT"};
    hf_log("bots: %d/%d in, phase %s, %d alive, rtt %.0f ms, resends %llu, rounds seen %d, wins %d",
           welcomed, count, names[bots[0].phase & 3], alive, rtt_n ? rtt / rtt_n : 0.0, resends,
           count ? rounds / count : 0, wins);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--host H] [--port P] [-n COUNT] [--name PREFIX] [--duration SEC]\n"
            "          [--loss PERCENT] [--delay MS] [--jitter MS]\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *port = "8080", *prefix = "bot";
    int count = 8;
    double duration = 0;
    netsim_cfg ns = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && v)
            host = argv[++i];
        else if (!strcmp(a, "--port") && v)
            port = argv[++i];
        else if ((!strcmp(a, "-n") || !strcmp(a, "--count")) && v)
            count = atoi(argv[++i]);
        else if (!strcmp(a, "--name") && v)
            prefix = argv[++i];
        else if (!strcmp(a, "--duration") && v)
            duration = atof(argv[++i]);
        else if (!strcmp(a, "--loss") && v)
            ns.loss = atof(argv[++i]) / 100.0;
        else if (!strcmp(a, "--delay") && v)
            ns.delay_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--jitter") && v)
            ns.jitter_ms = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (count < 1 || count > 1000) {
        usage(argv[0]);
        return 2;
    }

    struct sockaddr_storage addr;
    socklen_t addr_len;
    if (hf_resolve(host, port, &addr, &addr_len) != 0) {
        fprintf(stderr, "cannot resolve %s:%s\n", host, port);
        return 1;
    }
    netsim_set(ns);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    bot *bots = calloc((size_t)count, sizeof *bots);
    struct pollfd *pfds = calloc((size_t)count, sizeof *pfds);
    if (!bots || !pfds) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        bot *b = &bots[i];
        if (hf_client_open(&b->net, (struct sockaddr *)&addr, addr_len, on_msg, b) != 0) {
            perror("socket");
            return 1;
        }
        snprintf(b->name, sizeof b->name, "%s%02d", prefix, i + 1);
        hf_client_hello(&b->net, b->name, CLIENT_BOT, 0);
        b->noise = 0.2f + (float)hf_random01() * 1.1f;
        b->dir = (float)hf_random01() * TWO_PI;
        b->think = 1;
        pfds[i].fd = b->net.fd;
        pfds[i].events = POLLIN;
    }
    hf_log("%d bots playing on %s:%s", count, host, port);

    const uint64_t tick_us = 1000000u / SIM_TICK_HZ;
    uint64_t start_ms = hf_now_ms(), next_tick = hf_now_us() + tick_us, next_stats = start_ms + 5000;
    while (!stop) {
        uint64_t now_us = hf_now_us();
        int timeout = now_us >= next_tick ? 0 : (int)((next_tick - now_us + 999) / 1000);
        int delayed = netsim_wait_ms(now_us / 1000);
        if (delayed >= 0 && delayed < timeout)
            timeout = delayed;
        if (poll(pfds, (nfds_t)count, timeout) > 0) {
            uint64_t now_ms = hf_now_ms();
            for (int i = 0; i < count; i++)
                if (pfds[i].revents) {
                    bots[i].now_ms = now_ms;
                    hf_client_receive(&bots[i].net, now_ms);
                }
        }
        now_us = hf_now_us();
        if (now_us >= next_tick) {
            uint64_t now_ms = now_us / 1000;
            for (int i = 0; i < count; i++) {
                bots[i].now_ms = now_ms;
                bot_tick(&bots[i]);
                hf_client_flush(&bots[i].net, now_ms);
            }
            next_tick += tick_us;
            if (now_us >= next_tick)
                next_tick = now_us + tick_us;
        }
        uint64_t now_ms = hf_now_ms();
        netsim_flush(now_ms);
        if (now_ms >= next_stats) {
            print_stats(bots, count);
            next_stats = now_ms + 5000;
        }
        if (duration > 0 && (double)(now_ms - start_ms) >= duration * 1000.0)
            break;
    }

    /* Say goodbye so the server frees the slots right away. */
    uint64_t now_ms = hf_now_ms();
    for (int i = 0; i < count; i++) {
        msg_bye bye = {0};
        uint8_t buf[16];
        wbuf w;
        wb_init(&w, buf, sizeof buf);
        hf_put_bye(&w, &bye);
        hf_client_send(&bots[i].net, buf, w.len);
        netsim_set((netsim_cfg){0});
        hf_client_flush(&bots[i].net, now_ms);
        hf_client_close(&bots[i].net);
    }
    print_stats(bots, count);
    free(pfds);
    free(bots);
    return 0;
}
