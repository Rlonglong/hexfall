#ifndef HF_WORLD_H
#define HF_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "sim.h"

/* What the client knows about the game, built only from server messages:
 * the room, the board and the latest snapshot. No drawing and no sockets
 * here, so it can be tested on its own and every renderer reads the same
 * state. This is always the newest news; interp.c decides which moment of
 * it is drawn, and predict.c where our own player is drawn. */

#define WORLD_FEED 5

typedef struct {
    uint16_t id;
    uint8_t color, flags, place;
    char name[HF_NAME_MAX + 1];
} world_player;

typedef struct {
    uint16_t id;
    float x, y, z, vx, vy, vz, facing;
    int grounded, alive, layer;
} world_body;

/* One SNAPSHOT, decoded. */
typedef struct {
    uint32_t tick;
    uint16_t ack_input;
    int count;
    world_body bodies[HF_MAX_PLAYERS];
} world_snapshot;

/* Something worth telling the player. Knock-outs keep who, where and when,
 * so a renderer can show them its own way and at the right moment; anything
 * else is plain text. */
typedef struct {
    char text[64];
    uint64_t at_ms;
    int knockout;
    uint16_t player;
    uint8_t place;
    uint32_t tick;  /* the server tick it happened at (knock-outs) */
} world_note;

typedef struct {
    int welcomed;
    uint16_t me;

    int phase;
    uint32_t phase_end_tick;
    int player_count;
    world_player players[HF_MAX_PLAYERS];

    int have_board;
    sim_arena arena;

    int have_snapshot;
    uint32_t tick;        /* tick of the newest snapshot: the newest server time we know */
    uint64_t snapshot_at_ms;
    uint16_t ack_input;
    int body_count;
    world_body bodies[HF_MAX_PLAYERS];
    uint64_t snapshots_used, snapshots_late;

    world_note notes[WORLD_FEED];  /* newest last */
    int note_count;
} world;

void world_init(world *w);

/* A reliable message from the network thread's event channel. A WELCOME
 * starts a new session: everything learned before it is dropped and rebuilt
 * from what the server sends next. */
void world_apply_event(world *w, const uint8_t *msg, size_t len, uint64_t now_ms);

/* A message from the snapshot channel; 0 if it is not a well-formed SNAPSHOT. */
int world_decode_snapshot(const uint8_t *msg, size_t len, world_snapshot *out);

/* Takes a snapshot as the newest news, unless a newer one came first (then
 * it is only counted as late; interp.c may still have a use for it). */
void world_apply_snapshot(world *w, const world_snapshot *s, uint64_t arrived_ms);

/* A body as the physics (sim.c) takes it. The jump cooldown is not in a
 * snapshot, so it starts at 0. */
sim_body world_body_to_sim(const world_body *b);

const world_player *world_player_by_id(const world *w, uint16_t id);
const world_body *world_body_by_id(const world *w, uint16_t id);
int world_alive_count(const world *w);

#endif
