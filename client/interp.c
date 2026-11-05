#include "interp.h"

#include <math.h>
#include <string.h>

/* See interp.h for the why; this file is the how. */

#define TICK_MS (1000.0 / SIM_TICK_HZ)

const char *interp_mode_name(interp_mode mode)
{
    switch (mode) {
    case INTERP_NONE: return "none";
    case INTERP_FIXED: return "fixed";
    case INTERP_ADAPTIVE: return "adaptive";
    default: return "?";
    }
}

const char *interp_guess_name(interp_guess guess)
{
    switch (guess) {
    case GUESS_KEEP: return "keep going";
    case GUESS_LET_GO: return "let go";
    case GUESS_FREEZE: return "freeze";
    default: return "?";
    }
}

void interp_init(interp *ip, interp_mode mode, const sim_arena *board)
{
    memset(ip, 0, sizeof *ip);
    ip->mode = mode;
    ip->board = board;
}

static void restart_counters(interp *ip)
{
    ip->frames = ip->frames_guessed = ip->frames_waiting = ip->snapshots = ip->too_late = 0;
    ip->stutter_ms = 0;
    ip->shown_valid = 0;
    ip->guess_error_sum = ip->guess_error_max = 0;
    ip->guess_errors = 0;
}

void interp_set_mode(interp *ip, interp_mode mode)
{
    ip->mode = mode;
    restart_counters(ip);
}

void interp_set_guess(interp *ip, interp_guess guess)
{
    ip->guess = guess;
    restart_counters(ip);
}

/* Server time of a tick, in ms since `base`. */
static double tick_ms(const interp *ip, uint32_t tick) { return (double)(int32_t)(tick - ip->base) * TICK_MS; }

static const interp_slot *slot_of(const interp *ip, uint32_t tick)
{
    const interp_slot *sl = &ip->slot[tick % INTERP_HISTORY];
    return sl->used && sl->s.tick == tick ? sl : NULL;
}

/* ---- measuring the network ---- */

static void sort(double *v, int n)
{
    for (int i = 1; i < n; i++) {
        double x = v[i];
        int j = i;
        for (; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
}

/* Transit, lateness, loss and gaps over the snapshots in the history. */
static void measure(interp *ip)
{
    double transit[INTERP_HISTORY];
    int n = 0;
    uint32_t oldest = ip->newest;
    for (int i = 0; i < INTERP_HISTORY; i++) {
        const interp_slot *sl = &ip->slot[i];
        if (!sl->used || ip->newest - sl->s.tick >= INTERP_HISTORY)
            continue;
        transit[n++] = (double)sl->arrived_ms - tick_ms(ip, sl->s.tick);
        if ((int32_t)(sl->s.tick - oldest) < 0)
            oldest = sl->s.tick;
    }
    if (n == 0)
        return;

    double lo = transit[0];
    for (int i = 1; i < n; i++)
        lo = fmin(lo, transit[i]);
    for (int i = 0; i < n; i++)
        transit[i] -= lo;  /* now: how late each one was */
    sort(transit, n);
    int at = (int)ceil(INTERP_PERCENTILE * n) - 1;
    ip->min_transit_ms = lo;
    ip->late_ms = transit[at < 0 ? 0 : at];

    /* Counted from the oldest one we have: before it we cannot tell lost
     * from not yet started. */
    int span = (int)(ip->newest - oldest) + 1, run = 0, longest = 0;
    for (uint32_t t = oldest; t != ip->newest + 1; t++) {
        run = slot_of(ip, t) ? 0 : run + 1;
        if (run > longest)
            longest = run;
    }
    ip->loss = 1.0 - (double)n / span;
    ip->longest_gap = longest;
}

static void score_guess(interp *ip, const world_snapshot *s);

void interp_add(interp *ip, const world_snapshot *s, uint64_t arrived_ms)
{
    int32_t ahead = ip->have ? (int32_t)(s->tick - ip->newest) : 0;
    if (!ip->have || ahead > 4 * INTERP_HISTORY || ahead < -4 * INTERP_HISTORY) {
        /* The first snapshot, or one from a different stretch of time: start over. */
        memset(ip->slot, 0, sizeof ip->slot);
        ip->have = 1;
        ip->base = ip->newest = s->tick;
        ip->newest_arrived_ms = arrived_ms;
        ip->playing = 0;
        ip->shown_valid = 0;
        ahead = 0;
    }
    if (ahead <= -INTERP_HISTORY) {
        ip->too_late++;  /* older than anything we keep */
        return;
    }
    interp_slot *sl = &ip->slot[s->tick % INTERP_HISTORY];
    if (sl->used && sl->s.tick == s->tick)
        return;  /* a duplicate */
    ip->snapshots++;
    /* Needed since the playout passed the tick before it: the screen had to
     * do without it. */
    if (ip->playing && ip->mode != INTERP_NONE && tick_ms(ip, s->tick) - TICK_MS < ip->playout_ms) {
        ip->too_late++;
        score_guess(ip, s);
    }

    sl->used = 1;
    sl->arrived_ms = arrived_ms;
    sl->s.tick = s->tick;
    sl->s.ack_input = s->ack_input;
    sl->s.count = s->count;
    memcpy(sl->s.bodies, s->bodies, sizeof s->bodies[0] * (size_t)s->count);
    if (ahead > 0) {
        ip->newest = s->tick;
        ip->newest_arrived_ms = arrived_ms;
    }
    measure(ip);
}

/* ---- the playout clock ---- */

static double wanted_delay(const interp *ip)
{
    if (ip->mode == INTERP_FIXED)
        return INTERP_FIXED_MS;
    int bridge = ip->longest_gap < INTERP_MAX_BRIDGE ? ip->longest_gap : INTERP_MAX_BRIDGE;
    double d = TICK_MS * (1 + bridge) + ip->late_ms + INTERP_MARGIN_MS;
    return d < TICK_MS ? TICK_MS : d > INTERP_MAX_MS ? INTERP_MAX_MS : d;
}

static void steer(interp *ip, uint64_t now_ms, double dt)
{
    double want = wanted_delay(ip);
    if (!ip->playing || want > ip->delay_ms)
        ip->delay_ms = want;  /* worse: at once */
    else
        ip->delay_ms = fmax(want, ip->delay_ms - INTERP_DECAY_MS_PER_S * dt / 1000.0);  /* better: slowly */

    double target = (double)now_ms - ip->min_transit_ms - ip->delay_ms;
    if (!ip->playing || fabs(target - ip->playout_ms) > INTERP_SNAP_MS) {
        ip->playout_ms = target;
        ip->playing = 1;
        return;
    }
    ip->playout_ms += dt;
    double err = target - ip->playout_ms, room = INTERP_SLEW * dt;
    ip->playout_ms += err > room ? room : err < -room ? -room : err;
}

/* ---- what to draw ---- */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float lerp_angle(float a, float b, float t)
{
    return a + atan2f(sinf(b - a), cosf(b - a)) * t;  /* the short way round */
}

static const world_body *find(const world_snapshot *s, int hint, uint16_t id)
{
    if (hint < s->count && s->bodies[hint].id == id)
        return &s->bodies[hint];  /* the server keeps its order, so usually here */
    for (int i = 0; i < s->count; i++)
        if (s->bodies[i].id == id)
            return &s->bodies[i];
    return NULL;
}

/* Between a and b. Who is there comes from a: a player who appears in b is
 * drawn once the playout reaches b, and one missing from b (left, or fell
 * in) stays as in a until then. */
static void blend(interp_view *v, const world_snapshot *a, const world_snapshot *b, float t)
{
    v->count = a->count;
    for (int i = 0; i < a->count; i++) {
        const world_body *pa = &a->bodies[i], *pb = find(b, i, pa->id);
        world_body *o = &v->bodies[i];
        *o = *pa;
        if (!pb || pb->alive != pa->alive)
            continue;
        float dx = pb->x - pa->x, dy = pb->y - pa->y, dz = pb->z - pa->z, most = INTERP_TELEPORT * (float)(b->tick - a->tick);
        if (dx * dx + dy * dy + dz * dz > most * most)
            continue;  /* a jump in position, not a move: no sliding across */
        o->x = lerpf(pa->x, pb->x, t);
        o->y = lerpf(pa->y, pb->y, t);
        o->z = lerpf(pa->z, pb->z, t);
        o->vx = lerpf(pa->vx, pb->vx, t);
        o->vy = lerpf(pa->vy, pb->vy, t);
        o->vz = lerpf(pa->vz, pb->vz, t);
        o->facing = lerp_angle(pa->facing, pb->facing, t);
        if (t >= 0.5f) {
            o->grounded = pb->grounded;
            o->layer = pb->layer;
            o->move = pb->move;
            o->move_ticks = pb->move_ticks;
            o->shoving = pb->shoving;
        }
    }
}

static void copy(interp_view *v, const world_snapshot *s)
{
    v->count = s->count;
    memcpy(v->bodies, s->bodies, sizeof s->bodies[0] * (size_t)s->count);
}

/* ---- guessing ---- */

static double guess_limit_ms(const interp *ip) { return ip->guess == GUESS_FREEZE ? 0 : INTERP_GUESS_MS; }

void interp_board_at(sim_arena *out, const sim_arena *board, uint32_t tick)
{
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++) {
            out->tiles[layer][i].state = (uint8_t)interp_tile_state(&board->tiles[layer][i], tick);
            out->tiles[layer][i].touch_tick = board->tiles[layer][i].touch_tick;
        }
    out->armed_tick = board->armed_tick;
}

/* Where the players of snapshot a would be `ahead_ms` later, if each did
 * what the guess says: whole ticks of the game's physics, then part of one. */
static void guess(interp_view *v, const interp *ip, const world_snapshot *a, double ahead_ms)
{
    copy(v, a);
    if (ip->guess == GUESS_FREEZE || !ip->board || ahead_ms <= 0)
        return;
    double ticks = ahead_ms / TICK_MS;
    int steps = (int)ceil(ticks);
    float t = (float)(ticks - (steps - 1));
    static sim_arena board;  /* main thread only */
    sim_body now[HF_MAX_PLAYERS], before[HF_MAX_PLAYERS];
    for (int i = 0; i < a->count; i++)
        now[i] = world_body_to_sim(&a->bodies[i]);
    for (int k = 1; k <= steps; k++) {
        interp_board_at(&board, ip->board, a->tick + (uint32_t)k - 1);  /* the server lets tiles fall after moving */
        for (int i = 0; i < a->count; i++) {
            before[i] = now[i];
            sim_input in = {0};
            if (ip->guess == GUESS_KEEP)
                in = (sim_input){.mx = now[i].vx / SIM_MAX_SPEED, .mz = now[i].vz / SIM_MAX_SPEED};
            sim_body_step(&now[i], &in, &board, a->tick + (uint32_t)k, NULL, NULL);
        }
    }
    for (int i = 0; i < a->count; i++) {
        world_body *o = &v->bodies[i];
        if (!o->alive)
            continue;
        o->x = lerpf(before[i].x, now[i].x, t);
        o->y = lerpf(before[i].y, now[i].y, t);
        o->z = lerpf(before[i].z, now[i].z, t);
        o->vx = now[i].vx;
        o->vy = now[i].vy;
        o->vz = now[i].vz;
        o->facing = lerp_angle(before[i].facing, now[i].facing, t);
        o->grounded = now[i].grounded;
        o->layer = now[i].layer;
        o->move = now[i].move;
        o->move_ticks = now[i].move_ticks;
        o->shoving = now[i].shove_ticks > 0;
        /* alive stays as the snapshot says: only the server knocks anyone out */
    }
}

/* Snapshot s came after the screen had to do without it: what would the
 * guess have shown for its moment, and how far off was that? */
static void score_guess(interp *ip, const world_snapshot *s)
{
    const interp_slot *from = NULL;
    for (uint32_t k = 1; k < INTERP_HISTORY && !from; k++)
        from = slot_of(ip, s->tick - k);
    if (!from)
        return;
    double ahead = (double)(s->tick - from->s.tick) * TICK_MS;
    static interp_view g;  /* main thread only */
    guess(&g, ip, &from->s, fmin(ahead, guess_limit_ms(ip)));
    for (int i = 0; i < g.count; i++) {
        const world_body *gb = &g.bodies[i], *real = find(s, i, gb->id);
        if (!real || !gb->alive || !real->alive)
            continue;
        float dx = real->x - gb->x, dy = real->y - gb->y, dz = real->z - gb->z;
        double d = sqrt((double)(dx * dx + dy * dy + dz * dz));
        if (d > INTERP_TELEPORT * (double)(s->tick - from->s.tick))
            continue;  /* a jump in position: no guess could have known */
        ip->guess_error_sum += d;
        ip->guess_error_max = fmax(ip->guess_error_max, d);
        ip->guess_errors++;
    }
}

/* Fills the view for the playout time; returns the moment shown, in ms since
 * `base` (behind the playout when the guess had to stop: then *waiting). */
static double sample(interp *ip, int *waiting)
{
    interp_view *v = &ip->view;
    double p = ip->playout_ms / TICK_MS;  /* ticks since base */
    int32_t newest = (int32_t)(ip->newest - ip->base), oldest = newest - INTERP_HISTORY + 1;
    int32_t f = (int32_t)floor(p);
    const interp_slot *a = NULL, *b = NULL;
    for (int32_t t = f < newest ? f : newest; t >= oldest && !a; t--)
        a = slot_of(ip, ip->base + (uint32_t)t);
    for (int32_t t = f + 1 > oldest ? f + 1 : oldest; t <= newest && !b; t++)
        b = slot_of(ip, ip->base + (uint32_t)t);

    double shown;
    v->guessed = 0;
    if (a && b) {
        double ta = tick_ms(ip, a->s.tick), tb = tick_ms(ip, b->s.tick);
        blend(v, &a->s, &b->s, (float)((ip->playout_ms - ta) / (tb - ta)));
        shown = ip->playout_ms;
    } else if (a) {
        double ta = tick_ms(ip, a->s.tick), ahead = ip->playout_ms - ta, limit = guess_limit_ms(ip);
        if (ahead > 0.5) {
            ip->frames_guessed++;
            v->guessed = 1;
            if (ahead > limit) {
                ip->frames_waiting++;
                *waiting = 1;
                ahead = limit;
            }
        }
        guess(v, ip, &a->s, ahead);
        shown = ta + ahead;
    } else if (b) {
        copy(v, &b->s);  /* the playout is before anything we have: just after a start */
        shown = tick_ms(ip, b->s.tick);
    } else {
        v->count = 0;
        shown = ip->playout_ms;
    }
    v->tick = (double)ip->base + shown / TICK_MS;
    return shown;
}

void interp_frame(interp *ip, uint64_t now_ms)
{
    double dt = ip->last_ms && now_ms > ip->last_ms ? (double)(now_ms - ip->last_ms) : 0;
    ip->last_ms = now_ms;
    interp_view *v = &ip->view;
    if (!ip->have) {
        v->count = 0;
        v->tick = 0;
        v->guessed = 0;
        ip->waiting_ms = 0;
        return;
    }
    ip->frames++;

    double shown;
    int waiting = 0;
    if (ip->mode == INTERP_NONE) {
        /* Step 1: the newest snapshot as it is. Tiles still animate between
         * ticks, as they did then; the beans do not. */
        const interp_slot *sl = slot_of(ip, ip->newest);
        if (sl)
            copy(v, &sl->s);
        v->guessed = 0;
        double into = ((double)now_ms - (double)ip->newest_arrived_ms) / TICK_MS;
        v->tick = (double)ip->newest + (into < 0 ? 0 : into > 0.999 ? 0.999 : into);
        shown = tick_ms(ip, ip->newest);
        ip->playing = 0;  /* the next mode starts its clock afresh */
        ip->delay_ms = 0;
    } else {
        steer(ip, now_ms, dt);
        shown = sample(ip, &waiting);
    }
    ip->waiting_ms = waiting ? ip->waiting_ms + dt : 0;

    /* On a smooth screen the moment shown moves exactly as fast as real time. */
    if (ip->shown_valid && dt > 0) {
        double err = fabs((shown - ip->shown_ms) - dt), k = 1.0 - exp(-dt / 1000.0);
        ip->stutter_ms += (err - ip->stutter_ms) * k;
    }
    ip->shown_ms = shown;
    ip->shown_valid = 1;
}

const world_body *interp_body(const interp_view *v, uint16_t id)
{
    for (int i = 0; i < v->count; i++)
        if (v->bodies[i].id == id)
            return &v->bodies[i];
    return NULL;
}

int interp_alive_count(const interp_view *v)
{
    int n = 0;
    for (int i = 0; i < v->count; i++)
        n += v->bodies[i].alive;
    return n;
}

int interp_tile_state(const sim_tile *t, double tick)
{
    if (t->state == TILE_SOLID || t->touch_tick == 0)
        return t->state;  /* untouched, or gone before we knew when */
    double since = floor(tick) - (double)t->touch_tick;
    if (since < 0)
        return TILE_SOLID;  /* touched in the newest news, but not yet on screen */
    return since < SIM_FALL_TICKS ? TILE_TOUCHED : TILE_GONE;
}
