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
 * drawn must match the moment shown: x = 0.2 * (view.tick - FIRST). */

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

static double rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0;
}

static world_snapshot snapshot(uint32_t tick)
{
    world_snapshot s = {.tick = tick, .count = 1};
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
        if (ip->mode == INTERP_NONE || v->count != 1)
            continue;
        double line = 0.2 * (v->tick - FIRST), x = v->bodies[0].x;
        if (fabs(x - line) > 0.02 && fabs(x - line - 50) > 0.02)
            off_line++;
        if (fabs(x - line) > 0.25 && fabs(x - line - 50) > 0.25)
            between++;  /* neither here nor there: slid across a teleport */
    }
}

static double share(uint64_t part, uint64_t whole) { return whole ? (double)part / (double)whole : 0; }

int main(void)
{
    static interp ip;

    /* A perfect network, fixed delay: always 3 ticks behind, never short. */
    reset();
    interp_init(&ip, INTERP_FIXED);
    play(&ip, 3, 20, 0, 0);
    interp_set_mode(&ip, INTERP_FIXED);
    play(&ip, 5, 20, 0, 0);
    CHECK(ip.frames_extrapolated == 0 && ip.too_late == 0);
    CHECK(fabs(ip.delay_ms - INTERP_FIXED_MS) < 1e-9);
    CHECK(ip.stutter_ms < 0.5);
    CHECK(off_line == 0);

    /* Adaptive on the same network: just over one tick behind. */
    interp_set_mode(&ip, INTERP_ADAPTIVE);
    play(&ip, 5, 20, 0, 0);
    printf("perfect network: adaptive delay %.1f ms\n", ip.delay_ms);
    CHECK(ip.delay_ms > TICK_MS && ip.delay_ms < TICK_MS + INTERP_MARGIN_MS + 3);
    CHECK(ip.frames_extrapolated == 0 && ip.stutter_ms < 0.5 && off_line == 0);

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
        interp_init(&ip, m ? INTERP_ADAPTIVE : INTERP_FIXED);
        play(&ip, 3, 40, 120, 0.05);
        interp_set_mode(&ip, ip.mode);
        play(&ip, 20, 40, 120, 0.05);
        bad[m] = share(ip.frames_extrapolated, ip.frames);
        CHECK(off_line == 0);
        if (m)
            bad_delay = ip.delay_ms;
        printf("bad network: %-8s delay %5.1f ms, extrapolated %4.1f%% of frames, stutter %.2f ms, loss %.1f%%\n",
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
    interp_init(&ip, INTERP_ADAPTIVE);
    jump_at = FIRST + 120;
    play(&ip, 8, 20, 30, 0);
    CHECK(between == 0);

    /* The server restarted: its ticks start again from 1, and so does the history. */
    reset();
    interp_init(&ip, INTERP_FIXED);
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
