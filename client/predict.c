#include "predict.h"

#include <math.h>
#include <string.h>

#include "input_queue.h"

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
    pr->other_count = 0;
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
    if (b->grounded && !pr->stood[b->layer][i]) {
        pr->stood[b->layer][i] = 1;
        pr->mine[b->layer][i] = tick;  /* touched now, not when we flew low over it */
    }
}

/* What another player is assumed to press: the stick as they were moving. */
static sim_input guess_for(const sim_body *b)
{
    return (sim_input){.mx = b->vx / SIM_MAX_SPEED, .mz = b->vz / SIM_MAX_SPEED};
}

/* Tick `tick` of the server's game as far as we can tell: our bean with our
 * input, the others with the guess, each moved in the server's order, then
 * everybody pushed apart. The server lets tiles fall after moving
 * everybody, so the board is as it was at the end of the tick before. Our
 * own touches are not on it: tiles fall on the server's word only. */
static void step_world(predict *pr, const world *w, sim_body *me, predict_other *others, const sim_input *in,
                       uint32_t tick, int remember)
{
    static sim_arena board;  /* main thread only */
    interp_board_at(&board, &w->arena, tick - 1);
    sim_body *moved[HF_MAX_PLAYERS + 1];
    int n = 0;
    for (int i = 0; i <= pr->other_count; i++) {
        sim_body *b = i == pr->me_slot ? me : &others[i < pr->me_slot ? i : i - 1].body;
        sim_input g = b == me ? *in : guess_for(b);
        sim_body_step(b, &g, &board, tick, NULL, NULL);
        if (b->alive)
            moved[n++] = b;
    }
    sim_shoves(moved, n);
    sim_separate(moved, n);
    if (remember && tick >= board.armed_tick)  /* before that, standing touches nothing */
        mark(pr, me, tick);
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
        pr->other_count = 0;
    }
    pr->phase = w->phase;
}

static float dist(const sim_body *a, const sim_body *b)
{
    float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* From an old position to a new one without a jump on screen: the
 * difference goes into *off, which fades out. */
static float carry(float *ox, float *oy, float *oz, const sim_body *old, const sim_body *now)
{
    float d = dist(old, now);
    if (d < PREDICT_SNAP && old->alive && now->alive) {
        *ox += old->x - now->x;
        *oy += old->y - now->y;
        *oz += old->z - now->z;
    } else {
        *ox = *oy = *oz = 0;
    }
    return d;
}

static void carry_over(predict *pr, const sim_body *old, const sim_body *now, int count)
{
    float d = carry(&pr->off_x, &pr->off_y, &pr->off_z, old, now);
    if (count && d > PREDICT_NOISE) {
        pr->corrections++;
        pr->correction_sum += d;
        pr->correction_max = fmax(pr->correction_max, d);
    }
}

/* The others as the newest snapshot has them, in its order; where we had
 * them before smoothly turns into where we have them now. */
static void load_others(predict *pr, const world *w, predict_other *before, int *before_n)
{
    *before_n = pr->other_count;
    memcpy(before, pr->others, sizeof pr->others[0] * (size_t)pr->other_count);
    pr->other_count = 0;
    pr->me_slot = 0;
    for (int i = 0; i < w->body_count; i++) {
        const world_body *b = &w->bodies[i];
        if (b->id == w->me) {
            pr->me_slot = pr->other_count;
            continue;
        }
        pr->others[pr->other_count++] = (predict_other){.id = b->id, .body = world_body_to_sim(b)};
    }
}

static void carry_others(predict *pr, const predict_other *before, int before_n)
{
    for (int i = 0; i < pr->other_count; i++) {
        predict_other *o = &pr->others[i];
        for (int j = 0; j < before_n; j++)
            if (before[j].id == o->id) {
                o->off_x = before[j].off_x;
                o->off_y = before[j].off_y;
                o->off_z = before[j].off_z;
                carry(&o->off_x, &o->off_y, &o->off_z, &before[j].body, &o->body);
                break;
            }
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
    static predict_other before[HF_MAX_PLAYERS];  /* main thread only */
    int before_n = 0;
    pr->active = 0;

    if (!pr->enabled || !b || (w->phase != PHASE_PLAYING && w->phase != PHASE_RESULT)) {
        pr->shown_valid = 0;  /* not in this round: drawn like everybody else */
        pr->other_count = 0;
    } else if (!b->alive) {
        pr->shown = *b;  /* out: stays out, whatever the past on screen still shows */
        pr->shown_valid = 1;
        pr->other_count = 0;
    } else if (w->phase == PHASE_RESULT) {
        /* The round is over and nobody moves any more: everybody where the
         * server stopped them, reached smoothly. */
        load_others(pr, w, before, &before_n);
        pr->body = world_body_to_sim(b);
        if (was_active) {
            carry_over(pr, &old, &pr->body, 0);
            carry_others(pr, before, before_n);
        }
        pr->active = 1;
    } else if (fresh) {
        /* Where the server says everybody was after our input A, then the
         * ticks the server has yet to play: ours with our inputs after A,
         * the others with the guess. */
        load_others(pr, w, before, &before_n);
        sim_body body = world_body_to_sim(b);
        const predict_step *acked = step_of(pr, w->ack_input);
        if (acked) {
            body.jump_cooldown = acked->after.jump_cooldown;
            body.shove_cooldown = acked->after.shove_cooldown;
            body.shove_ticks = acked->after.shove_ticks;
        }
        uint32_t tick = w->tick;
        for (uint16_t seq = (uint16_t)(w->ack_input + 1); seq != (uint16_t)(pr->seq + 1); seq++) {
            predict_step *st = step_of(pr, seq);
            if (!st)
                break;  /* cannot happen with unused <= PREDICT_MAX_AHEAD < PREDICT_HISTORY */
            step_world(pr, w, &body, pr->others, &st->in, ++tick, 1);
            st->after = body;
        }
        if (was_active) {
            carry_over(pr, &old, &body, 1);
            carry_others(pr, before, before_n);
        }
        pr->body = body;
        pr->snapshots++;
        pr->active = 1;
    } else {
        pr->shown_valid = 0;  /* the ack is from before this round: wait for one that is not */
        pr->other_count = 0;
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
        step_world(pr, w, &pr->body, pr->others, &st->in, pr->tick, 1);
    st->after = pr->body;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Between two steps, plus what is left of a correction. */
static void show(world_body *o, uint16_t id, const sim_body *a, const sim_body *b, float t, float ox, float oy,
                 float oz)
{
    const sim_body *near = t < 0.5f ? a : b;
    o->id = id;
    o->x = lerpf(a->x, b->x, t) + ox;
    o->y = lerpf(a->y, b->y, t) + oy;
    o->z = lerpf(a->z, b->z, t) + oz;
    o->vx = lerpf(a->vx, b->vx, t);
    o->vy = lerpf(a->vy, b->vy, t);
    o->vz = lerpf(a->vz, b->vz, t);
    o->facing = a->facing + atan2f(sinf(b->facing - a->facing), cosf(b->facing - a->facing)) * t;
    o->grounded = near->grounded;
    o->layer = near->layer;
    o->move = near->move;
    o->move_ticks = near->move_ticks;
    o->shoving = near->shove_ticks > 0;
    o->alive = a->alive;
}

void predict_frame(predict *pr, const world *w, const input_sample *keys, double frac, double dt_ms)
{
    watch_phase(pr, w);
    float k = (float)exp(-dt_ms / PREDICT_SMOOTH_MS);
    pr->off_x *= k;
    pr->off_y *= k;
    pr->off_z *= k;
    for (int i = 0; i < pr->other_count; i++) {
        pr->others[i].off_x *= k;
        pr->others[i].off_y *= k;
        pr->others[i].off_z *= k;
    }
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    pr->shown_tick = (double)pr->tick + frac;
    if (!pr->active)
        return;

    /* Part of the way to the next step, everybody's, taken with our keys as
     * they are now. */
    sim_body next = pr->body;
    static predict_other next_others[HF_MAX_PLAYERS];  /* main thread only */
    memcpy(next_others, pr->others, sizeof pr->others[0] * (size_t)pr->other_count);
    if (w->phase == PHASE_PLAYING) {
        sim_input in = inputq_to_sim(keys);
        step_world(pr, w, &next, next_others, &in, pr->tick + 1, 0);
    }
    float t = (float)frac;
    show(&pr->shown, w->me, &pr->body, &next, t, pr->off_x, pr->off_y, pr->off_z);
    for (int i = 0; i < pr->other_count; i++) {
        const predict_other *o = &pr->others[i];
        show(&pr->shown_others[i], o->id, &o->body, &next_others[i].body, t, o->off_x, o->off_y, o->off_z);
    }
    pr->shown_valid = 1;
}

/* 1 near our bean, 0 far from it, a mix between. */
static float nearness(const predict *pr, float x, float z)
{
    float dx = x - pr->shown.x, dz = z - pr->shown.z, d = sqrtf(dx * dx + dz * dz);
    float k = (PREDICT_FAR - d) / (PREDICT_FAR - PREDICT_NEAR);
    return k < 0 ? 0 : k > 1 ? 1 : k;
}

void predict_scene(const predict *pr, const world *w, const interp_view *past, interp_view *out)
{
    *out = *past;
    if (!pr->shown_valid)
        return;
    int i = 0;
    while (i < out->count && out->bodies[i].id != w->me)
        i++;
    if (i == out->count && out->count < HF_MAX_PLAYERS)
        out->count++;
    if (i < out->count)
        out->bodies[i] = pr->shown;
    if (!pr->active || !pr->shown.alive)
        return;

    for (int k = 0; k < out->count; k++) {
        world_body *o = &out->bodies[k];
        const world_body *now = NULL;
        for (int j = 0; j < pr->other_count && !now; j++)
            if (pr->shown_others[j].id == o->id)
                now = &pr->shown_others[j];
        if (o->id == w->me || !now || !o->alive || !now->alive)
            continue;  /* who is out stays as the server says */
        float t = nearness(pr, now->x, now->z);
        if (t <= 0)
            continue;
        o->x = lerpf(o->x, now->x, t);
        o->y = lerpf(o->y, now->y, t);
        o->z = lerpf(o->z, now->z, t);
        o->vx = lerpf(o->vx, now->vx, t);
        o->vy = lerpf(o->vy, now->vy, t);
        o->vz = lerpf(o->vz, now->vz, t);
        o->facing += atan2f(sinf(now->facing - o->facing), cosf(now->facing - o->facing)) * t;
        if (t >= 0.5f) {
            o->grounded = now->grounded;
            o->layer = now->layer;
        }
    }
}

void predict_tile(const predict *pr, const world *w, int layer, int index, double playout_tick, sim_tile *t,
                  double *at)
{
    *t = w->arena.tiles[layer][index];
    uint32_t m = pr->enabled && pr->shown_valid ? pr->mine[layer][index] : 0;
    int near = 0;
    if (!m && pr->active && pr->shown.alive) {
        int q, r;
        float x, z;
        sim_tile_qr(index, &q, &r);
        sim_tile_center(q, r, &x, &z);
        near = nearness(pr, x, z) >= 0.5f;
    }
    if (!m && !near) {
        *at = playout_tick;
        return;
    }
    *at = pr->shown_tick;
    if (m && t->state == TILE_SOLID && pr->stood[layer][index]) {
        /* We stood on it and the server has not said so yet: lit, but it
         * waits for the server before it falls. */
        t->state = TILE_TOUCHED;
        t->touch_tick = m;
        *at = fmin(fmax(*at, (double)m), (double)m + SIM_FALL_TICKS - 1);
    }
}
