#include "sim.h"

#include <math.h>
#include <string.h>

const float SIM_LAYER_Y[SIM_LAYERS] = {0.0f, -12.0f, -24.0f};

#define DT (1.0f / SIM_TICK_HZ)
#define GRAVITY 40.0f
#define JUMP_SPEED 13.0f
#define MAX_SPEED 8.0f
#define ACCEL_GROUND 34.0f
#define ACCEL_AIR 12.0f
#define JUMP_COOLDOWN_TICKS 9
#define WORLD_LIMIT 60.0f
#define SQRT3 1.7320508f
#define TWO_PI 6.2831853f

static int16_t index_of[2 * SIM_RING + 1][2 * SIM_RING + 1];
static int8_t qr_of[SIM_TILES][2];
static int tables_ready;

static void build_tables(void)
{
    for (int q = 0; q <= 2 * SIM_RING; q++)
        for (int r = 0; r <= 2 * SIM_RING; r++)
            index_of[q][r] = -1;
    int n = 0;
    for (int q = -SIM_RING; q <= SIM_RING; q++) {
        int r0 = q < 0 ? -SIM_RING - q : -SIM_RING;
        int r1 = q > 0 ? SIM_RING - q : SIM_RING;
        for (int r = r0; r <= r1; r++) {
            index_of[q + SIM_RING][r + SIM_RING] = (int16_t)n;
            qr_of[n][0] = (int8_t)q;
            qr_of[n][1] = (int8_t)r;
            n++;
        }
    }
    tables_ready = 1;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int sim_tile_index(int q, int r)
{
    if (!tables_ready)
        build_tables();
    if (q < -SIM_RING || q > SIM_RING || r < -SIM_RING || r > SIM_RING)
        return -1;
    return index_of[q + SIM_RING][r + SIM_RING];
}

void sim_tile_qr(int index, int *q, int *r)
{
    if (!tables_ready)
        build_tables();
    *q = qr_of[index][0];
    *r = qr_of[index][1];
}

void sim_tile_center(int q, int r, float *x, float *z)
{
    *x = SIM_HEX_SIZE * SQRT3 * ((float)q + (float)r / 2.0f);
    *z = SIM_HEX_SIZE * 1.5f * (float)r;
}

int sim_world_tile(float x, float z, int *q, int *r)
{
    /* Pointy-top axial coordinates, then cube rounding. */
    float qf = (SQRT3 / 3.0f * x - z / 3.0f) / SIM_HEX_SIZE;
    float rf = (2.0f / 3.0f * z) / SIM_HEX_SIZE;
    float sf = -qf - rf;
    float rq = roundf(qf), rr = roundf(rf), rs = roundf(sf);
    float dq = fabsf(rq - qf), dr = fabsf(rr - rf), ds = fabsf(rs - sf);
    if (dq > dr && dq > ds)
        rq = -rr - rs;
    else if (dr > ds)
        rr = -rq - rs;
    *q = (int)rq;
    *r = (int)rr;
    return sim_tile_index(*q, *r) >= 0;
}

/* Index of the tile under (x, z) on a floor, or -1 if there is none or it is gone. */
static int solid_tile(const sim_arena *a, int layer, float x, float z, int *q, int *r)
{
    if (layer < 0 || layer >= SIM_LAYERS || !sim_world_tile(x, z, q, r))
        return -1;
    int i = sim_tile_index(*q, *r);
    return a->tiles[layer][i].state == TILE_GONE ? -1 : i;
}

int sim_tile_state_at(const sim_arena *a, int layer, float x, float z)
{
    int q, r;
    if (layer < 0 || layer >= SIM_LAYERS || !sim_world_tile(x, z, &q, &r))
        return -1;
    return a->tiles[layer][sim_tile_index(q, r)].state;
}

void sim_arena_reset(sim_arena *a)
{
    if (!tables_ready)
        build_tables();
    memset(a, 0, sizeof *a);  /* every tile TILE_SOLID */
}

void sim_touch(sim_arena *a, int layer, int q, int r, uint32_t tick, sim_touch_fn fn, void *ctx)
{
    int i = sim_tile_index(q, r);
    if (i < 0 || layer < 0 || layer >= SIM_LAYERS)
        return;
    sim_tile *t = &a->tiles[layer][i];
    if (t->state != TILE_SOLID)
        return;
    t->state = TILE_TOUCHED;
    t->touch_tick = tick;
    if (fn)
        fn(ctx, layer, q, r);
}

int sim_arena_tick(sim_arena *a, uint32_t tick)
{
    int fell = 0;
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++) {
            sim_tile *t = &a->tiles[layer][i];
            if (t->state == TILE_TOUCHED && tick - t->touch_tick >= SIM_FALL_TICKS) {
                t->state = TILE_GONE;
                fell++;
            }
        }
    return fell;
}

void sim_spawn_point(int i, int n, float *x, float *z)
{
    float angle = (float)i / (float)(n > 0 ? n : 1) * TWO_PI + 0.2f;
    float radius = (i % 2) ? 17.0f : 10.0f;
    *x = sinf(angle) * radius;
    *z = cosf(angle) * radius;
}

void sim_body_spawn(sim_body *b, float x, float z)
{
    memset(b, 0, sizeof *b);
    b->x = x;
    b->z = z;
    b->y = SIM_LAYER_Y[0];
    b->grounded = 1;
    b->alive = 1;
    b->facing = atan2f(-x, -z);
}

void sim_body_step(sim_body *b, const sim_input *in, sim_arena *a, uint32_t tick, sim_touch_fn fn,
                   void *ctx)
{
    if (!b->alive)
        return;
    float mx = in->mx, mz = in->mz;
    float len = sqrtf(mx * mx + mz * mz);
    if (len > 1.0f) {
        mx /= len;
        mz /= len;
    }
    float accel = (b->grounded ? ACCEL_GROUND : ACCEL_AIR) * DT;
    b->vx += clampf(mx * MAX_SPEED - b->vx, -accel, accel);
    b->vz += clampf(mz * MAX_SPEED - b->vz, -accel, accel);
    b->x = clampf(b->x + b->vx * DT, -WORLD_LIMIT, WORLD_LIMIT);
    b->z = clampf(b->z + b->vz * DT, -WORLD_LIMIT, WORLD_LIMIT);
    if (b->jump_cooldown > 0)
        b->jump_cooldown--;

    int q, r;
    if (b->grounded) {
        if (solid_tile(a, b->layer, b->x, b->z, &q, &r) < 0) {
            b->grounded = 0;  /* the floor fell away, or we walked off it */
            b->vy = 0;
        } else {
            b->y = SIM_LAYER_Y[b->layer];
            sim_touch(a, b->layer, q, r, tick, fn, ctx);
            if (in->jump && b->jump_cooldown == 0) {
                b->vy = JUMP_SPEED;  /* airborne: no tiles touched until landing */
                b->grounded = 0;
                b->jump_cooldown = JUMP_COOLDOWN_TICKS;
            }
        }
    }
    if (!b->grounded) {
        float y0 = b->y;
        b->vy -= GRAVITY * DT;
        b->y += b->vy * DT;
        if (b->vy < 0) {
            /* Falling past a floor: land if there is a tile right there. */
            for (int layer = 0; layer < SIM_LAYERS; layer++) {
                float h = SIM_LAYER_Y[layer];
                if (y0 >= h - 0.001f && b->y <= h && solid_tile(a, layer, b->x, b->z, &q, &r) >= 0) {
                    b->y = h;
                    b->vy = 0;
                    b->grounded = 1;
                    b->layer = layer;
                    sim_touch(a, layer, q, r, tick, fn, ctx);
                    break;
                }
            }
        }
        if (b->y < SIM_SLIME_Y + 1.0f)
            b->alive = 0;
    }
    if (b->vx * b->vx + b->vz * b->vz > 0.25f)
        b->facing = atan2f(b->vx, b->vz);
}

void sim_separate(sim_body **bodies, int n)
{
    const float min_dist = 2.0f * SIM_BODY_RADIUS;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            sim_body *a = bodies[i], *b = bodies[j];
            if (!a->alive || !b->alive || fabsf(a->y - b->y) > 1.6f)
                continue;
            float dx = b->x - a->x, dz = b->z - a->z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d <= 0.0f || d >= min_dist)
                continue;
            float nx = dx / d, nz = dz / d, push = (min_dist - d) / 2.0f;
            a->x -= nx * push;
            a->z -= nz * push;
            b->x += nx * push;
            b->z += nz * push;
            float closing = (a->vx - b->vx) * nx + (a->vz - b->vz) * nz;
            if (closing > 0) {
                float impulse = closing * 0.6f;
                a->vx -= impulse * nx;
                a->vz -= impulse * nz;
                b->vx += impulse * nx;
                b->vz += impulse * nz;
            }
        }
}
