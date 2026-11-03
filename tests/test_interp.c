#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "interp.h"

/* Snapshot interpolation against a simulated network: a server that sends
 * one snapshot per tick, a network that delays, jitters (and so reorders)
 * and loses them, and a 60 Hz screen. Time is simulated, so the results do
 * not depend on how busy the machine is.
 *
 * One player walks along x at 6 units/s, 0.2 per tick, so wherever it is
 * drawn must match the moment shown: x = 0.2 * (view.tick - FIRST), on
 * every frame that is not a guess. A second player, driven by the game's
 * physics the way a bot plays, is used to score the guesses. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define FIRST 1000u
#define TICK_MS (1000.0 / SIM_TICK_HZ)
#define FRAME_MS (1000.0 / 60)

static struct {
    double at;
    uint32_t tick;
} flight[8192];
static int nflight;
static uint32_t next_tick;
static double now;
static uint32_t jump_at;  /* from this tick on, the player is 50 units further: a teleport */
static int off_line, between;
static uint32_t seed = 12345;
static sim_arena board;  /* every tile there, none ever falls */

#define PATH 1200
static world_body path[PATH];  /* the physics player, tick FIRST + i */
static int use_path;

static double rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0;
}

static world_snapshot snapshot(uint32_t tick)
{
    world_snapshot s = {.tick = tick, .count = 1};
    if (use_path) {
        s.bodies[0] = path[(tick - FIRST) % PATH];
        return s;
    }
    float x = 0.2f * (float)(tick - FIRST) + (jump_at && tick >= jump_at ? 50.0f : 0.0f);
    s.bodies[0] = (world_body){.id = 1, .x = x, .vx = 6, .alive = 1, .grounded = 1};
    return s;
}

static void reset(void)
{
    nflight = 0;
    next_tick = FIRST;
    now = 0;
    jump_at = 0;
    off_line = between = 0;
}

/* `seconds` of a network that adds delay + U(0, jitter) ms and loses `loss`. */
static void play(interp *ip, double seconds, double delay, double jitter, double loss)
{
    for (double end = now + seconds * 1000; now < end; now += FRAME_MS) {
        for (; (next_tick - FIRST) * TICK_MS <= now; next_tick++)
            if (rnd() >= loss && nflight < 8192) {
                flight[nflight].at = (next_tick - FIRST) * TICK_MS + delay + rnd() * jitter;
                flight[nflight++].tick = next_tick;
            }
        for (int i = 0; i < nflight;)
            if (flight[i].at <= now) {
                world_snapshot s = snapshot(flight[i].tick);
                interp_add(ip, &s, (uint64_t)flight[i].at);
                flight[i] = flight[--nflight];
            } else {
                i++;
            }
        interp_frame(ip, (uint64_t)now);

        const interp_view *v = &ip->view;
        if (ip->mode == INTERP_NONE || v->count != 1 || v->guessed || use_path)
            continue;
        double line = 0.2 * (v->tick - FIRST), x = v->bodies[0].x;
        if (fabs(x - line) > 0.02 && fabs(x - line - 50) > 0.02)
            off_line++;
        if (fabs(x - line) > 0.25 && fabs(x - line - 50) > 0.25)
            between++;  /* neither here nor there: slid across a teleport */
    }
}

static double share(uint64_t part, uint64_t whole) { return whole ? (double)part / (double)whole : 0; }

/* A bot's run: one way for a while, then another, sometimes standing,
 * sometimes jumping, kept near the middle. It touches tiles, but on this
 * board none of them ever falls. */
static void make_path(void)
{
    static sim_arena played;
    sim_arena_reset(&played);
    sim_body b;
    sim_body_spawn(&b, 0, 0);
    sim_input in = {0};
    for (int i = 0, turn = 0; i < PATH; i++) {
        if (--turn <= 0) {
            turn = 10 + (int)(rnd() * 30);
            float a = (float)(rnd() * 6.2831853);
            in.mx = rnd() < 0.2 ? 0 : sinf(a);
            in.mz = in.mx == 0 ? 0 : cosf(a);
            if (b.x * b.x + b.z * b.z > 14 * 14) {
                float d = sqrtf(b.x * b.x + b.z * b.z);
                in.mx = -b.x / d;
                in.mz = -b.z / d;
            }
        }
        in.jump = rnd() < 0.04;
        sim_body_step(&b, &in, &played, FIRST + (uint32_t)i, NULL, NULL);
        path[i] = (world_body){.id = 1, .x = b.x, .y = b.y, .z = b.z, .vx = b.vx, .vy = b.vy, .vz = b.vz,
                               .facing = b.facing, .grounded = b.grounded, .alive = b.alive, .layer = b.layer};
    }
}

int main(void)
{
    static interp ip;
    sim_arena_reset(&board);

    /* A perfect network, fixed delay: always 3 ticks behind, never short. */
    reset();
    interp_init(&ip, INTERP_FIXED, &board);
    play(&ip, 3, 20, 0, 0);
    interp_set_mode(&ip, INTERP_FIXED);
    play(&ip, 5, 20, 0, 0);
    CHECK(ip.frames_guessed == 0 && ip.too_late == 0);
    CHECK(fabs(ip.delay_ms - INTERP_FIXED_MS) < 1e-9);
    CHECK(ip.stutter_ms < 0.5);
    CHECK(off_line == 0);

    /* Adaptive on the same network: just over one tick behind. */
    interp_set_mode(&ip, INTERP_ADAPTIVE);
    play(&ip, 5, 20, 0, 0);
    printf("perfect network: adaptive delay %.1f ms\n", ip.delay_ms);
    CHECK(ip.delay_ms > TICK_MS && ip.delay_ms < TICK_MS + INTERP_MARGIN_MS + 3);
    CHECK(ip.frames_guessed == 0 && ip.stutter_ms < 0.5 && off_line == 0);

    /* No smoothing: 30 snapshots a second on a 60 Hz screen step visibly. */
    interp_set_mode(&ip, INTERP_NONE);
    play(&ip, 3, 20, 0, 0);
    printf("perfect network: none stutters %.1f ms per frame\n", ip.stutter_ms);
    CHECK(ip.stutter_ms > 10);

    /* A bad network: 40-160 ms and 5% lost. The fixed delay runs short
     * often; the adaptive one grows until it rarely does, and every frame
     * still shows the player where it was at the moment shown. */
    double bad[2], bad_delay = 0;
    for (int m = 0; m < 2; m++) {
        reset();
        interp_init(&ip, m ? INTERP_ADAPTIVE : INTERP_FIXED, &board);
        play(&ip, 3, 40, 120, 0.05);
        interp_set_mode(&ip, ip.mode);
        play(&ip, 20, 40, 120, 0.05);
        bad[m] = share(ip.frames_guessed, ip.frames);
        CHECK(off_line == 0);
        if (m)
            bad_delay = ip.delay_ms;
        printf("bad network: %-8s delay %5.1f ms, guessed %4.1f%% of frames, stutter %.2f ms, loss %.1f%%\n",
               interp_mode_name(ip.mode), ip.delay_ms, 100 * bad[m], ip.stutter_ms, 100 * ip.loss);
    }
    CHECK(bad[0] > 0.10);
    CHECK(bad[1] < 0.03);
    CHECK(bad_delay > 120 + TICK_MS && bad_delay < INTERP_MAX_MS);

    /* When the network gets better, the adaptive delay comes down, slowly. */
    play(&ip, 1, 20, 0, 0);
    CHECK(ip.delay_ms > 120);
    play(&ip, 15, 20, 0, 0);
    CHECK(ip.delay_ms < TICK_MS + INTERP_MARGIN_MS + 3);

    /* A teleport is shown as a jump, not a slide across the floor. */
    reset();
    interp_init(&ip, INTERP_ADAPTIVE, &board);
    jump_at = FIRST + 120;
    play(&ip, 8, 20, 30, 0);
    CHECK(between == 0);

    /* An outage: nothing for a second. The screen guesses for a while, then
     * stops and waits; once snapshots come again it carries on. */
    reset();
    interp_init(&ip, INTERP_ADAPTIVE, &board);
    play(&ip, 3, 20, 0, 0);
    play(&ip, 1, 20, 0, 1.0);
    CHECK(ip.view.guessed && ip.waiting_ms > 1000 - INTERP_GUESS_MS - 150);
    play(&ip, 2, 20, 0, 0);
    CHECK(ip.waiting_ms == 0 && !ip.view.guessed && off_line == 0);

    /* Guesses, scored on the physics player: a fixed delay on a bad network
     * runs short often, so there is a lot to guess. */
    make_path();
    double error[GUESS_MODES];
    for (int g = 0; g < GUESS_MODES; g++) {
        reset();
        use_path = 1;
        seed = 777;  /* the same network for each */
        interp_init(&ip, INTERP_FIXED, &board);
        interp_set_guess(&ip, (interp_guess)g);
        play(&ip, 30, 40, 120, 0.05);
        CHECK(ip.guess_errors > 100);
        error[g] = ip.guess_error_sum / (double)ip.guess_errors;
        printf("guess %-10s off by %.2f on average, %.2f at most (%llu guesses)\n", interp_guess_name(ip.guess),
               error[g], ip.guess_error_max, (unsigned long long)ip.guess_errors);
    }
    use_path = 0;
    CHECK(error[GUESS_LET_GO] < error[GUESS_FREEZE] && error[GUESS_KEEP] < error[GUESS_FREEZE]);

    /* The server restarted: its ticks start again from 1, and so does the history. */
    reset();
    interp_init(&ip, INTERP_FIXED, &board);
    play(&ip, 2, 20, 0, 0);
    world_snapshot s = snapshot(5);
    interp_add(&ip, &s, (uint64_t)now);
    interp_frame(&ip, (uint64_t)now + 16);
    CHECK(ip.base == 5 && ip.newest == 5 && ip.view.count == 1);

    /* Tiles, at the moment shown. */
    sim_tile t = {.state = TILE_TOUCHED, .touch_tick = 200};
    CHECK(interp_tile_state(&t, 199.6) == TILE_SOLID);
    CHECK(interp_tile_state(&t, 200.0) == TILE_TOUCHED);
    CHECK(interp_tile_state(&t, 200.0 + SIM_FALL_TICKS - 0.1) == TILE_TOUCHED);
    CHECK(interp_tile_state(&t, 200.0 + SIM_FALL_TICKS) == TILE_GONE);
    t.state = TILE_GONE;
    CHECK(interp_tile_state(&t, 205) == TILE_TOUCHED);
    t.touch_tick = 0;
    CHECK(interp_tile_state(&t, 5) == TILE_GONE);

    printf("interp: all checks passed\n");
    return 0;
}
