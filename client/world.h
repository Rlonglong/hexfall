#ifndef HF_WORLD_H
#define HF_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "sim.h"

/* What the client knows about the game, built only from server messages:
 * the room, the board and the latest snapshot. No drawing and no sockets
 * here, so it can be tested on its own and every renderer reads the same
 * state. Step 1 shows the latest snapshot as is; interpolation and
 * prediction will add their own views on top. */

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

/* Something worth telling the player. Knock-outs keep who and where, so a
 * renderer can show them its own way; anything else is plain text. */
typedef struct {
    char text[64];
    uint64_t at_ms;
    int knockout;
    uint16_t player;
    uint8_t place;
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

/* A snapshot from the snapshot channel; older than the newest one is ignored. */
void world_apply_snapshot(world *w, const uint8_t *msg, size_t len, uint64_t arrived_ms);

const world_player *world_player_by_id(const world *w, uint16_t id);
const world_body *world_body_by_id(const world *w, uint16_t id);
int world_alive_count(const world *w);

#endif
