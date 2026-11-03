#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_queue.h"
#include "interp.h"
#include "predict.h"

/* Prediction against a server and a network of the test's own. The server
 * plays by the room's rules for one player: the same input queue
 * (input_queue.c), the same physics (sim.c), tiles that fall, and a
 * snapshot after every tick, encoded and decoded as on the wire. The
 * network delays, jitters (so reorders) and loses packets both ways. The
 * client sends an input every tick and draws at 60 Hz. Time is simulated,
 * in steps of 1 ms, so nothing depends on how busy the machine is. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define ME 7
#define TICK_MS (1000.0 / SIM_TICK_HZ)
#define FRAME_MS (1000.0 / 60)
#define FLIGHT 4096

typedef struct {
    double at;
    int kind;  /* 0 snapshot, 1 tile touched, 2 input */
    world_snapshot snap;
    tile_ref tile;
    uint32_t tick;
    msg_input in;
} packet;

static packet up[FLIGHT], down[FLIGHT];
static int nup, ndown;
static double now, delay_ms, jitter_ms, loss;
static uint32_t seed = 4242;

static struct {
    sim_body body;
    inputq q;
    sim_arena arena;
    uint32_t tick;
    double next_ms;
    uint32_t push_at;  /* the tick something bumps the player aside, 0 = never */
    int no_falls;      /* tiles never fall (and nobody hears of touches): standing still is safe */
} S;

static world w;
static predict pr;

/* The client's keys: what the next input will be. */
static input_sample keys, history[HF_INPUT_REDUNDANCY];
static int history_n, wander, turn;
static double next_input_ms, last_input_ms, next_frame_ms;
static uint64_t used_seen;

/* Measured on every frame. */
static float last_x, last_z, largest_step;
static int have_last, frames_on_solid, frames_checked;

static double rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0;
}

static double arrival(void) { return now + delay_ms + rnd() * jitter_ms; }

static int16_t fixed(float v) { return (int16_t)lrintf(v * HF_POS_SCALE); }

static void on_touch(void *ctx, int layer, int q, int r)
{
    (void)ctx;
    /* Tile news travels on the reliable channel: late, maybe, but never lost. */
    if (!S.no_falls && ndown < FLIGHT)
        down[ndown++] = (packet){.at = arrival(), .kind = 1, .tile = {(uint8_t)layer, (int8_t)q, (int8_t)r},
                                 .tick = S.tick};
}

static void server_tick(void)
{
    S.tick++;
    sim_input in = inputq_next(&S.q);
    sim_body_step(&S.body, &in, &S.arena, S.tick, on_touch, NULL);
    if (S.tick == S.push_at)
        S.body.x += 1.5f;  /* as if another player had shoved us */
    if (!S.no_falls)
        sim_arena_tick(&S.arena, S.tick);

    msg_snapshot m = {.tick = S.tick, .ack_input = S.q.applied, .count = 1};
    float turns = S.body.facing / 6.2831853f;
    m.p[0] = (snap_player){
        .id = ME, .x = fixed(S.body.x), .y = fixed(S.body.y), .z = fixed(S.body.z),
        .vx = fixed(S.body.vx), .vy = fixed(S.body.vy), .vz = fixed(S.body.vz),
        .facing = (uint16_t)((uint32_t)lrintf((turns - floorf(turns)) * 65536.0f) & 0xFFFF),
        .flags = (uint8_t)((S.body.grounded ? SP_GROUNDED : 0) | (S.body.alive ? SP_ALIVE : 0)),
        .layer = (uint8_t)S.body.layer,
    };
    uint8_t buf[HF_MAX_MSG];
    wbuf wb;
    wb_init(&wb, buf, sizeof buf);
    hf_put_snapshot(&wb, &m);
    packet p = {.kind = 0};
    CHECK(world_decode_snapshot(buf, wb.len, &p.snap));
    if (rnd() >= loss && ndown < FLIGHT) {
        p.at = arrival();
        down[ndown++] = p;
    }
}

/* The player: a while one way, then another, now and then standing or
 * jumping, kept near the middle of the top floor. */
static void choose_keys(void)
{
    keys.buttons = 0;
    if (!wander)
        return;
    if (--turn <= 0) {
        turn = 10 + (int)(rnd() * 30);
        double a = rnd() * 6.2831853, x = pr.body.x, z = pr.body.z, d = sqrt(x * x + z * z);
        keys.mx = (int8_t)(rnd() < 0.2 ? 0 : lrint(sin(a) * 127));
        keys.mz = (int8_t)(keys.mx == 0 ? 0 : lrint(cos(a) * 127));
        if (d > 12) {
            keys.mx = (int8_t)lrint(-x / d * 127);
            keys.mz = (int8_t)lrint(-z / d * 127);
        }
    }
    if (rnd() < 0.04)
        keys.buttons = BTN_JUMP;
}

static void client_input(void)
{
    keys.seq++;
    memmove(&history[1], &history[0], sizeof history[0] * (HF_INPUT_REDUNDANCY - 1));
    history[0] = keys;
    if (history_n < HF_INPUT_REDUNDANCY)
        history_n++;
    msg_input m = {.count = (uint8_t)history_n};
    memcpy(m.s, history, sizeof m.s);
    if (rnd() >= loss && nup < FLIGHT)
        up[nup++] = (packet){.at = arrival(), .kind = 2, .in = m};
    predict_input(&pr, &w, &keys);
    last_input_ms = now;
    choose_keys();
}

/* What a player would see go wrong: a tile under our bean still drawn
 * as never touched, or the bean moving further in one frame than it can. */
static void client_frame(void)
{
    predict_frame(&pr, &w, &keys, (now - last_input_ms) / TICK_MS, FRAME_MS);
    if (!pr.shown_valid || !pr.shown.alive) {
        have_last = 0;
        return;
    }
    if (have_last) {
        float dx = pr.shown.x - last_x, dz = pr.shown.z - last_z, d = sqrtf(dx * dx + dz * dz);
        largest_step = fmaxf(largest_step, d);
    }
    last_x = pr.shown.x;
    last_z = pr.shown.z;
    have_last = 1;

    int q, r;
    if (pr.active && pr.body.grounded && sim_world_tile(pr.body.x, pr.body.z, &q, &r)) {
        sim_tile t;
        double at;
        predict_tile(&pr, &w, pr.body.layer, sim_tile_index(q, r), 0, &t, &at);
        frames_checked++;
        frames_on_solid += interp_tile_state(&t, at) == TILE_SOLID;
    }
}

static void deliver(packet *box, int *n, int to_server)
{
    for (int i = 0; i < *n;) {
        packet *p = &box[i];
        if (p->at > now) {
            i++;
            continue;
        }
        if (to_server)
            inputq_add(&S.q, &p->in);
        else if (p->kind == 0)
            world_apply_snapshot(&w, &p->snap, (uint64_t)now);
        else
            sim_touch(&w.arena, p->tile.layer, p->tile.q, p->tile.r, p->tick, NULL, NULL);
        *p = box[--*n];
    }
    if (!to_server && w.snapshots_used != used_seen) {
        used_seen = w.snapshots_used;
        predict_snapshot(&pr, &w);
    }
}

static void run(double seconds)
{
    for (double end = now + seconds * 1000; now < end; now += 1) {
        deliver(up, &nup, 1);
        deliver(down, &ndown, 0);
        while (S.next_ms <= now) {
            server_tick();
            S.next_ms += TICK_MS;
        }
        if (now >= next_input_ms) {
            client_input();
            next_input_ms += TICK_MS;
        }
        if (now >= next_frame_ms) {
            client_frame();
            next_frame_ms += FRAME_MS;
        }
    }
}

/* A round on a fresh board, the player in the middle of the top floor.
 * The lobby before it took a while, so the server's ack is from long ago. */
static void start(double delay, double jitter, double lost, int moving, int falls)
{
    nup = ndown = 0;
    delay_ms = delay;
    jitter_ms = jitter;
    loss = lost;
    sim_arena_reset(&S.arena);
    sim_body_spawn(&S.body, 0, 0);
    inputq_clear(&S.q);
    S.push_at = 0;
    S.no_falls = !falls;
    world_init(&w);
    w.welcomed = 1;
    w.me = ME;
    w.have_board = 1;
    w.phase = PHASE_PLAYING;
    used_seen = 0;
    uint16_t seq = (uint16_t)(keys.seq + 300);
    predict_init(&pr);
    keys = (input_sample){.seq = seq};
    history_n = 0;
    wander = moving;
    turn = 0;
    S.next_ms = now + 3;
    next_input_ms = now + 14;  /* the client's ticks are not the server's */
    next_frame_ms = now;
    largest_step = 0;
    have_last = frames_on_solid = frames_checked = 0;
}

/* Over several rounds: standing still is not an option in this game, so
 * a round ends with a fall into the slime sooner or later. */
static struct {
    uint64_t snapshots, corrections;
    double sum, max;
} total;

static void rounds(int n, double seconds, double delay, double jitter, double lost)
{
    memset(&total, 0, sizeof total);
    float step = 0;
    int checked = 0, on_solid = 0;
    for (int i = 0; i < n; i++) {
        start(delay, jitter, lost, 1, 1);
        run(seconds);
        total.snapshots += pr.snapshots;
        total.corrections += pr.corrections;
        total.sum += pr.correction_sum;
        total.max = fmax(total.max, pr.correction_max);
        step = fmaxf(step, largest_step);
        checked += frames_checked;
        on_solid += frames_on_solid;
    }
    largest_step = step;
    frames_checked = checked;
    frames_on_solid = on_solid;
}

static void report(const char *what)
{
    printf("%-30s %5llu snapshots, %4llu corrections (%.2f on average, %.2f at most), largest step %.2f\n", what,
           (unsigned long long)total.snapshots, (unsigned long long)total.corrections,
           total.corrections ? total.sum / (double)total.corrections : 0, total.max, largest_step);
}

int main(void)
{
    /* The round starts with an ack from before it: no predicting from that.
     * Once the server uses the first input of the round, it starts. */
    start(25, 0, 0, 0, 0);
    run(0.04);
    CHECK(w.have_snapshot && !pr.active && !pr.shown_valid);
    run(0.3);
    CHECK(pr.active && pr.shown_valid && pr.ahead >= 1 && pr.ahead <= 4);

    /* A press shows on the very next frame, long before the server could
     * say so: standing still, then W halfway between two input ticks. */
    run(0.5);
    while (next_input_ms - now > TICK_MS / 2)
        run(0.001);
    float x0 = pr.shown.x, z0 = pr.shown.z;
    keys.mz = -127;
    next_frame_ms = now;
    run(0.001);
    CHECK(pr.shown.z < z0 - 0.001f && pr.shown.x == x0);
    run(1);
    CHECK(pr.corrections == 0);

    /* A perfect network: the server does what was predicted, through runs,
     * turns, jumps and falls. Positions travel rounded to 1/100, so now and
     * then one of the two beans is just over the edge of a hole and the
     * other is not, and one starts to fall a tick before the other; nothing
     * else is ever corrected. Tiles under our bean are lit at once. */
    rounds(6, 10, 25, 0, 0);
    report("perfect network, 50 ms RTT:");
    CHECK(total.snapshots > 1000);
    CHECK(total.corrections <= 5 && total.max < 0.25);
    CHECK(frames_checked > 1000 && frames_on_solid == 0);
    CHECK(largest_step < SIM_MAX_SPEED * FRAME_MS / 1000 * 1.6f);

    /* A bad network: inputs come late or bunched up, so the server
     * sometimes has to reuse the old stick or skip inputs, and the
     * prediction is off. Corrections are small and smoothed out: the bean
     * never jumps on screen. */
    rounds(6, 10, 30, 60, 0.05);
    report("30-90 ms each way, 5% lost:");
    CHECK(total.snapshots > 1000 && total.corrections > 0);
    CHECK(total.sum / (double)total.corrections < 0.3);
    CHECK(largest_step < 0.4f);

    /* A shove the client could not know about: one big correction, shown
     * as a quick slide, not a jump. */
    start(25, 0, 0, 0, 0);
    run(1);
    S.push_at = S.tick + 10;
    run(1);
    printf("%-30s %.2f\n", "a shove of 1.5, corrected by:", pr.correction_max);
    CHECK(pr.corrections == 1 && pr.correction_max > 1.4 && pr.correction_max < 1.6);
    CHECK(largest_step < 0.4f && fabsf(pr.shown.x - S.body.x) < 0.05f);

    /* Standing still until the tile gives way: the tile is drawn gone on
     * the very frame our bean starts to fall, not `delay` ms later. */
    start(60, 0, 0, 0, 1);
    int q, r, fell_on_gone = -1;
    sim_world_tile(0, 0, &q, &r);
    for (int i = 0; i < 3000 && fell_on_gone < 0; i++) {
        run(0.001);
        if (pr.active && !pr.body.grounded && pr.body.y < 0) {
            sim_tile t;
            double at;
            predict_tile(&pr, &w, 0, sim_tile_index(q, r), 0, &t, &at);
            fell_on_gone = interp_tile_state(&t, at) == TILE_GONE;
        }
    }
    CHECK(fell_on_gone == 1);

    printf("predict: all checks passed\n");
    return 0;
}
