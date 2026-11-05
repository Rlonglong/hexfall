#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_queue.h"
#include "interp.h"
#include "predict.h"

/* Prediction against a server and a network of the test's own. The server
 * plays by the room's rules: our player with the same input queue
 * (input_queue.c), maybe one other player holding a fixed stick, the same
 * physics (sim.c) including pushing players apart, tiles that fall, and a
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
#define OTHER 3  /* comes first in the server's order */
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
    int has_other;
    sim_body other;
    sim_input other_in;
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

static snap_player on_wire(uint16_t id, const sim_body *b)
{
    float turns = b->facing / 6.2831853f;
    return (snap_player){
        .id = id, .x = fixed(b->x), .y = fixed(b->y), .z = fixed(b->z),
        .vx = fixed(b->vx), .vy = fixed(b->vy), .vz = fixed(b->vz),
        .facing = (uint16_t)((uint32_t)lrintf((turns - floorf(turns)) * 65536.0f) & 0xFFFF),
        .flags = (uint8_t)((b->grounded ? SP_GROUNDED : 0) | (b->alive ? SP_ALIVE : 0) |
                           (b->move == MOVE_DIVE ? SP_DIVING : 0) | (b->move == MOVE_DOWN ? SP_DOWN : 0) |
                           (b->shove_ticks > 0 ? SP_SHOVING : 0)),
        .layer = (uint8_t)b->layer,
        .move_ticks = (uint8_t)(b->move == MOVE_DOWN ? b->move_ticks : 0),
    };
}

static void server_tick(void)
{
    S.tick++;
    sim_body *alive[2];
    int n = 0;
    if (S.has_other && S.other.alive) {
        sim_body_step(&S.other, &S.other_in, &S.arena, S.tick, on_touch, NULL);
        if (S.other.alive)
            alive[n++] = &S.other;
    }
    sim_input in = inputq_next(&S.q);
    sim_body_step(&S.body, &in, &S.arena, S.tick, on_touch, NULL);
    if (S.body.alive)
        alive[n++] = &S.body;
    sim_shoves(alive, n);
    sim_separate(alive, n);
    if (S.tick == S.push_at)
        S.body.x += 1.5f;  /* as if something we cannot know of had shoved us */
    if (!S.no_falls)
        sim_arena_tick(&S.arena, S.tick);

    msg_snapshot m = {.tick = S.tick, .ack_input = S.q.applied};
    if (S.has_other)
        m.p[m.count++] = on_wire(OTHER, &S.other);
    m.p[m.count++] = on_wire(ME, &S.body);
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

/* The player: a while one way, then another, now and then standing,
 * jumping, diving or shoving, kept near the middle of the top floor. */
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
    double r = rnd();
    keys.buttons = r < 0.04 ? BTN_JUMP : r < 0.05 ? BTN_DIVE : r < 0.06 ? BTN_SHOVE : 0;
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
    S.has_other = 0;
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

/* Another player, holding the stick one way all along. */
static void add_other(float x, float z, float mx, float mz)
{
    S.has_other = 1;
    sim_body_spawn(&S.other, x, z);
    S.other_in = (sim_input){.mx = mx, .mz = mz};
}

static float gap_to_other(void)
{
    float dx = S.other.x - S.body.x, dz = S.other.z - S.body.z;
    return sqrtf(dx * dx + dz * dz);
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
    CHECK(largest_step < SIM_SPRINT_SPEED * FRAME_MS / 1000 * 1.25f);

    /* A bad network: inputs come late or bunched up, so the server
     * sometimes has to reuse the old stick or skip inputs, and the
     * prediction is off; if that happens as we jump, for the whole jump,
     * since nothing steers in the air. Corrections are smoothed out: the
     * bean never jumps on screen. */
    rounds(8, 10, 30, 60, 0.05);
    report("30-90 ms each way, 5% lost:");
    CHECK(total.snapshots > 1000 && total.corrections > 0);
    CHECK(total.sum / (double)total.corrections < 0.5);
    CHECK(largest_step < 0.4f);

    /* A shove the client could not know about: one big correction, shown
     * as a quick slide, not a jump. */
    start(25, 0, 0, 0, 0);
    run(1);
    S.push_at = S.tick + 10;
    run(1);
    printf("%-30s %.2f\n", "a teleport of 1.5, corrected:", pr.correction_max);
    CHECK(pr.corrections == 1 && pr.correction_max > 1.4 && pr.correction_max < 1.6);
    CHECK(largest_step < 0.4f && fabsf(pr.shown.x - S.body.x) < 0.05f);

    /* Running into another player. The client plays the bump itself, the
     * other player included, so what the server does next was foreseen:
     * no correction, though we are pushed and push. */
    start(25, 0, 0, 0, 0);
    add_other(0, -6, 0, 0);
    run(0.5);
    keys.mz = -127;
    float closest = 99;
    for (int i = 0; i < 1500; i++) {
        run(0.001);
        closest = fminf(closest, gap_to_other());
    }
    printf("%-30s %llu corrections, %.2f at most; closest %.2f\n", "running into a player:",
           (unsigned long long)pr.corrections, pr.correction_max, closest);
    CHECK(closest < 2 * SIM_BODY_RADIUS + 0.05f && S.other.z < -7);
    CHECK(pr.correction_max < 0.25);
    CHECK(pr.other_count == 1 && fabsf(pr.others[0].body.z - S.other.z) < 0.5f);

    /* Head-on into a player running at us: the guess has it keep running,
     * which it does, so this bump is foreseen too. */
    start(25, 0, 0, 0, 0);
    add_other(0, -12, 0, 1);
    run(0.3);
    keys.mz = -127;
    closest = 99;
    for (int i = 0; i < 2000; i++) {
        run(0.001);
        closest = fminf(closest, gap_to_other());
    }
    printf("%-30s %llu corrections, %.2f at most; closest %.2f\n", "head-on into a runner:",
           (unsigned long long)pr.corrections, pr.correction_max, closest);
    CHECK(closest < 2 * SIM_BODY_RADIUS + 0.05f);
    CHECK(pr.correction_max < 0.5);

    /* A dive of ours, on a perfect network: under way on screen before the
     * server has the input, and then the server does exactly what was
     * predicted, the flight, the slide on the belly and getting up. */
    start(25, 0, 0, 0, 0);
    run(0.5);
    keys.mz = 127;
    run(0.3);
    uint64_t before_dive = pr.corrections;
    float server_z = S.body.z;
    keys.buttons = BTN_DIVE;
    int dived_first = 0, saw_down = 0;
    for (int i = 0; i < 1500; i++) {
        run(0.001);
        dived_first |= pr.shown.move == MOVE_DIVE && S.body.move == MOVE_NONE;
        saw_down |= S.body.move == MOVE_DOWN;
    }
    keys.mz = 0;
    run(0.5);
    printf("%-30s %llu corrections; dived %.1f units\n", "a dive of ours:",
           (unsigned long long)(pr.corrections - before_dive), S.body.z - server_z);
    CHECK(dived_first && saw_down && S.body.move == MOVE_NONE);
    CHECK(pr.corrections == before_dive);

    /* Our shove: the player in front flies on our screen before the server
     * has even heard of it, and lands where the server puts it. */
    start(25, 0, 0, 0, 0);
    add_other(0, 2.4f, 0, 0);
    run(0.5);
    keys.mz = 127;  /* aimed with the stick; let go once the shove is sent */
    keys.buttons = BTN_SHOVE;
    float other_z = S.other.z;
    int flew_first = 0;
    for (int i = 0; i < 1000; i++) {
        run(0.001);
        if (!keys.buttons)
            keys.mz = 0;
        flew_first |= pr.other_count == 1 && pr.shown_others[0].z > other_z + 0.3f && S.other.z == other_z;
    }
    printf("%-30s %.1f units, off by %.2f\n", "a shove of ours:", S.other.z - other_z,
           fabsf(pr.others[0].body.z - S.other.z));
    CHECK(flew_first && S.other.z > other_z + 2.5f);
    CHECK(fabsf(pr.others[0].body.z - S.other.z) < 0.05f && fabsf(pr.others[0].body.x - S.other.x) < 0.05f);

    /* Shoved by somebody else: nobody could have seen it coming, so it is
     * corrected, smoothly, never a jump on screen. */
    start(25, 0, 0, 0, 0);
    add_other(0, -2.4f, 0, 0);
    run(0.5);
    float my_z = S.body.z;
    S.other_in = (sim_input){.mz = 0.01f, .shove = 1};
    uint32_t shoved_at = S.tick + 1;
    while (S.tick < shoved_at)
        run(0.001);
    S.other_in = (sim_input){0};
    largest_step = 0;
    run(1.0);
    printf("%-30s %.1f units, corrected by %.2f, largest step %.2f\n", "shoved by another player:",
           S.body.z - my_z, pr.correction_max, largest_step);
    CHECK(S.body.z > my_z + 2.5f);
    CHECK(largest_step < 0.45f);

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

    /* The floor holds at the start of a round: standing still, the tile
     * under our bean is not drawn lit while our present is before the
     * board is armed (the server says when), and is lit from the tick it
     * is, as on the server. Our present is ahead of the server's, so it is
     * lit on screen a little before the server gets there. */
    start(60, 0, 0, 0, 1);
    uint32_t armed = S.tick + 45;
    S.arena.armed_tick = w.arena.armed_tick = armed;
    const sim_tile *served = &S.arena.tiles[0][sim_tile_index(q, r)];
    int lit_early = 0, lit_ahead = 0;
    while (S.tick < armed - 1) {
        run(0.001);
        sim_tile t;
        double at;
        predict_tile(&pr, &w, 0, sim_tile_index(q, r), 0, &t, &at);
        int lit = pr.active && interp_tile_state(&t, at) != TILE_SOLID;
        lit_early += lit && pr.shown_tick < armed - 1;
        lit_ahead += lit;
    }
    CHECK(pr.active && lit_early == 0 && lit_ahead > 0 && served->state == TILE_SOLID);
    run(0.3);
    {
        sim_tile t;
        double at;
        predict_tile(&pr, &w, 0, sim_tile_index(q, r), 0, &t, &at);
        CHECK(served->state == TILE_TOUCHED && served->touch_tick == armed);
        CHECK(t.state == TILE_TOUCHED && t.touch_tick == armed);
    }

    printf("predict: all checks passed\n");
    return 0;
}
