#ifndef HF_SIM_H
#define HF_SIM_H

#include <stdint.h>

/* Hex Fall simulation, shared by the server (which decides what happens) and
 * the clients (which will use it to predict their own player).
 *
 * Arena: SIM_LAYERS stacked hexagonal floors, each a hexagon of hexes with
 * radius SIM_RING in axial coordinates (q, r). A tile touched by a player
 * stays solid for SIM_FALL_TICKS, then is gone. Nothing is touched before
 * the arena's armed_tick, which gives everybody a moment to get going at
 * the start of a round. Below the last floor is the slime; reaching it
 * means elimination.
 *
 * Running straight at full tilt builds up past SIM_MAX_SPEED for about a
 * second, then breaks into a sprint up to SIM_SPRINT_SPEED; turning away,
 * letting go or bumping into someone ends it. A jump keeps the speed it
 * took off with, so a sprinting jump goes furthest.
 *
 * Canonical tile order (used by the TILES bitmap): q from -RING to RING, and
 * for each q, r ascending over the valid range. */

#define SIM_TICK_HZ 30
#define SIM_RING 8
#define SIM_LAYERS 3
#define SIM_TILES 217  /* 1 + 3 * RING * (RING + 1) */
#define SIM_FALL_TICKS 24  /* 0.8 s */
#define SIM_HEX_SIZE 2.0f
#define SIM_BODY_RADIUS 0.9f
#define SIM_SLIME_Y (-34.0f)
#define SIM_MAX_SPEED 8.0f     /* running speed; what the stick asks for at full tilt */
#define SIM_SPRINT_FROM 8.5f   /* built up to this, the sprint kicks in... */
#define SIM_SPRINT_SPEED 11.0f /* ...up to this */

extern const float SIM_LAYER_Y[SIM_LAYERS];

enum { TILE_SOLID = 0, TILE_TOUCHED = 1, TILE_GONE = 2 };

typedef struct {
    uint8_t state;
    uint32_t touch_tick;
} sim_tile;

typedef struct {
    sim_tile tiles[SIM_LAYERS][SIM_TILES];
    uint32_t armed_tick;  /* touches before this tick do nothing */
} sim_arena;

typedef struct {
    float mx, mz;  /* world-space direction, length <= 1 */
    int jump;
} sim_input;

typedef struct {
    float x, y, z, vx, vy, vz, facing;
    int grounded, layer, alive, jump_cooldown;
} sim_body;

typedef void (*sim_touch_fn)(void *ctx, int layer, int q, int r);

int sim_tile_index(int q, int r);  /* -1 outside the arena */
void sim_tile_qr(int index, int *q, int *r);
void sim_tile_center(int q, int r, float *x, float *z);
int sim_world_tile(float x, float z, int *q, int *r);  /* nearest hex; 1 if inside the arena */
int sim_tile_state_at(const sim_arena *a, int layer, float x, float z);  /* -1 if no tile there */

void sim_arena_reset(sim_arena *a);
void sim_touch(sim_arena *a, int layer, int q, int r, uint32_t tick, sim_touch_fn fn, void *ctx);
int sim_arena_tick(sim_arena *a, uint32_t tick);  /* touched tiles whose time is up become gone */

void sim_spawn_point(int i, int n, float *x, float *z);
void sim_body_spawn(sim_body *b, float x, float z);
void sim_body_step(sim_body *b, const sim_input *in, sim_arena *a, uint32_t tick, sim_touch_fn fn,
                   void *ctx);
void sim_separate(sim_body **bodies, int n);

#endif
