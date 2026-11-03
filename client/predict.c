#include "predict.h"

#include <math.h>
#include <string.h>

#include "input_queue.h"
#include "interp.h"

/* See predict.h for the why; this file is the how. */

void predict_init(predict *pr)
{
    memset(pr, 0, sizeof *pr);
    pr->enabled = 1;
    pr->phase = -1;
}

void predict_set_enabled(predict *pr, int on)
{
    pr->enabled = on;
    pr->active = 0;
    pr->shown_valid = 0;
    pr->off_x = pr->off_y = pr->off_z = 0;
    pr->snapshots = pr->corrections = 0;
    pr->correction_sum = pr->correction_max = 0;
}

static predict_step *step_of(predict *pr, uint16_t seq)
{
    predict_step *st = &pr->hist[seq % PREDICT_HISTORY];
    return st->used && st->seq == seq ? st : NULL;
}

/* The tile under our bean, if the bean is at the height of its floor. */
static void mark(predict *pr, const sim_body *b, uint32_t tick)
{
    int q, r;
    if (!b->alive || b->layer < 0 || b->layer >= SIM_LAYERS || fabsf(b->y - SIM_LAYER_Y[b->layer]) > 1.0f ||
        !sim_world_tile(b->x, b->z, &q, &r))
        return;
    int i = sim_tile_index(q, r);
    if (!pr->mine[b->layer][i])
        pr->mine[b->layer][i] = tick;
    if (b->grounded)
        pr->stood[b->layer][i] = 1;
}

/* Tick `tick` of the server's physics for our bean. The server moves
 * everybody first and lets tiles fall after, so the board is as it was at
 * the end of the tick before. Our own touches are not on it: tiles fall on
 * the server's word only. */
static void step(predict *pr, const world *w, sim_body *b, const sim_input *in, uint32_t tick, int remember)
{
    static sim_arena board;  /* main thread only */
    interp_board_at(&board, &w->arena, tick - 1);
    sim_body_step(b, in, &board, tick, NULL, NULL);
    if (remember)
        mark(pr, b, tick);
}

/* A new round starts on a new board; outside a round there is nothing to predict. */
static void watch_phase(predict *pr, const world *w)
{
    if (w->phase == pr->phase)
        return;
    if (w->phase == PHASE_PLAYING) {
        memset(pr->mine, 0, sizeof pr->mine);
        memset(pr->stood, 0, sizeof pr->stood);
    }
    if (w->phase != PHASE_PLAYING && w->phase != PHASE_RESULT) {
        pr->active = 0;
        pr->shown_valid = 0;
    }
    pr->phase = w->phase;
}

static float dist(const sim_body *a, const sim_body *b)
{
    float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* From the old prediction to the new one without a jump on screen. */
static void carry_over(predict *pr, const sim_body *old, const sim_body *now, int count)
{
    float d = dist(old, now);
    if (count && d > PREDICT_NOISE) {
        pr->corrections++;
        pr->correction_sum += d;
        pr->correction_max = fmax(pr->correction_max, d);
    }
    if (d < PREDICT_SNAP && old->alive && now->alive) {
        pr->off_x += old->x - now->x;
        pr->off_y += old->y - now->y;
        pr->off_z += old->z - now->z;
    } else {
        pr->off_x = pr->off_y = pr->off_z = 0;
    }
}

static void forget_unconfirmed(predict *pr, const world *w)
{
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++)
            if (pr->stood[layer][i] && w->arena.tiles[layer][i].state == TILE_SOLID &&
                (int32_t)(pr->tick - pr->mine[layer][i]) > PREDICT_UNCONFIRMED)
                pr->mine[layer][i] = pr->stood[layer][i] = 0;
}

void predict_snapshot(predict *pr, const world *w)
{
    watch_phase(pr, w);
    const world_body *b = world_body_by_id(w, w->me);
    int was_active = pr->active;
    uint16_t unused = (uint16_t)(pr->seq - w->ack_input);
    int fresh = pr->have_seq && unused <= PREDICT_MAX_AHEAD;
    if (b && b->alive && w->phase == PHASE_PLAYING && fresh)
        pr->ahead = unused;  /* measured with prediction off too: it is what prediction saves */
    sim_body old = pr->body;
    pr->active = 0;

    if (!pr->enabled || !b || (w->phase != PHASE_PLAYING && w->phase != PHASE_RESULT)) {
        pr->shown_valid = 0;  /* not in this round: drawn like everybody else */
    } else if (!b->alive) {
        pr->shown = *b;  /* out: stays out, whatever the past on screen still shows */
        pr->shown_valid = 1;
    } else if (w->phase == PHASE_RESULT) {
        /* The round is over and nobody moves any more: where the server
         * stopped us, reached smoothly. */
        pr->body = world_body_to_sim(b);
        if (was_active)
            carry_over(pr, &old, &pr->body, 0);
        pr->active = 1;
    } else if (fresh) {
        /* Where the server says we were after input A, then our inputs
         * after A, one tick each, as the server will use them. */
        sim_body body = world_body_to_sim(b);
        const predict_step *acked = step_of(pr, w->ack_input);
        body.jump_cooldown = acked ? acked->after.jump_cooldown : 0;
        uint32_t tick = w->tick;
        for (uint16_t seq = (uint16_t)(w->ack_input + 1); seq != (uint16_t)(pr->seq + 1); seq++) {
            predict_step *st = step_of(pr, seq);
            if (!st)
                break;  /* cannot happen with unused <= PREDICT_MAX_AHEAD < PREDICT_HISTORY */
            step(pr, w, &body, &st->in, ++tick, 1);
            st->after = body;
        }
        if (was_active)
            carry_over(pr, &old, &body, 1);
        pr->body = body;
        pr->snapshots++;
        pr->active = 1;
    } else {
        pr->shown_valid = 0;  /* the ack is from before this round: wait for one that is not */
    }
    pr->tick = w->tick + (uint32_t)(pr->active && w->phase == PHASE_PLAYING ? unused : pr->ahead);
    forget_unconfirmed(pr, w);
}

void predict_input(predict *pr, const world *w, const input_sample *s)
{
    predict_step *st = &pr->hist[s->seq % PREDICT_HISTORY];
    st->used = 1;
    st->seq = s->seq;
    st->in = inputq_to_sim(s);
    pr->seq = s->seq;
    pr->have_seq = 1;
    pr->tick++;
    if (pr->active && w->phase == PHASE_PLAYING)
        step(pr, w, &pr->body, &st->in, pr->tick, 1);
    st->after = pr->body;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

void predict_frame(predict *pr, const world *w, const input_sample *keys, double frac, double dt_ms)
{
    watch_phase(pr, w);
    float k = (float)exp(-dt_ms / PREDICT_SMOOTH_MS);
    pr->off_x *= k;
    pr->off_y *= k;
    pr->off_z *= k;
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    pr->shown_tick = (double)pr->tick + frac;
    if (!pr->active)
        return;

    /* Part of the way to the next step, taken with the keys as they are now. */
    sim_body next = pr->body;
    if (w->phase == PHASE_PLAYING) {
        sim_input in = inputq_to_sim(keys);
        step(pr, w, &next, &in, pr->tick + 1, 0);
    }
    const sim_body *a = &pr->body, *near = frac < 0.5 ? a : &next;
    float t = (float)frac;
    world_body *o = &pr->shown;
    o->id = w->me;
    o->x = lerpf(a->x, next.x, t) + pr->off_x;
    o->y = lerpf(a->y, next.y, t) + pr->off_y;
    o->z = lerpf(a->z, next.z, t) + pr->off_z;
    o->vx = lerpf(a->vx, next.vx, t);
    o->vy = lerpf(a->vy, next.vy, t);
    o->vz = lerpf(a->vz, next.vz, t);
    o->facing = a->facing + atan2f(sinf(next.facing - a->facing), cosf(next.facing - a->facing)) * t;
    o->grounded = near->grounded;
    o->layer = near->layer;
    o->alive = a->alive;
    pr->shown_valid = 1;
}

void predict_tile(const predict *pr, const world *w, int layer, int index, double playout_tick, sim_tile *t,
                  double *at)
{
    *t = w->arena.tiles[layer][index];
    uint32_t m = pr->enabled && pr->shown_valid ? pr->mine[layer][index] : 0;
    if (!m) {
        *at = playout_tick;
        return;
    }
    *at = pr->shown_tick;
    if (t->state == TILE_SOLID && pr->stood[layer][index]) {
        /* We stood on it and the server has not said so yet: lit, but it
         * waits for the server before it falls. */
        t->state = TILE_TOUCHED;
        t->touch_tick = m;
        *at = fmin(fmax(*at, (double)m), (double)m + SIM_FALL_TICKS - 1);
    }
}
