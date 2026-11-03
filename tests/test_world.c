#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "sim.h"
#include "world.h"

/* The client's game state, fed the messages a server would send, including
 * a second WELCOME as after a resync. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static uint8_t buf[HF_MAX_MSG];

static size_t welcome(uint16_t id, uint32_t tick)
{
    msg_welcome m = {.token = 42, .player_id = id, .tick_hz = SIM_TICK_HZ, .tick = tick};
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_welcome(&w, &m);
    return w.len;
}

/* Encoded and decoded again, as the client gets it from the snapshot channel. */
static const world_snapshot *snapshot(uint32_t tick, uint16_t id, float x)
{
    static world_snapshot out;
    msg_snapshot m = {.tick = tick, .count = 1};
    m.p[0] = (snap_player){.id = id, .x = (int16_t)(x * HF_POS_SCALE), .flags = SP_ALIVE | SP_GROUNDED};
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_snapshot(&w, &m);
    CHECK(world_decode_snapshot(buf, w.len, &out));
    return &out;
}

int main(void)
{
    static world w;
    world_init(&w);
    wbuf wb;

    world_apply_event(&w, buf, welcome(7, 100), 0);
    CHECK(w.welcomed && w.me == 7 && !w.have_board);

    msg_room room = {.phase = PHASE_PLAYING, .tick = 100, .count = 1};
    room.p[0] = (room_player){.id = 7, .color = 3, .flags = RP_READY | RP_ALIVE | RP_CONNECTED};
    strcpy(room.p[0].name, "me");
    wb_init(&wb, buf, sizeof buf);
    hf_put_room(&wb, &room);
    world_apply_event(&w, buf, wb.len, 0);
    CHECK(w.phase == PHASE_PLAYING && world_player_by_id(&w, 7) && !strcmp(world_player_by_id(&w, 7)->name, "me"));

    static msg_tiles tiles;
    memset(&tiles, 0, sizeof tiles);
    tiles.tick = 100;
    tiles.layers = SIM_LAYERS;
    tiles.ring = SIM_RING;
    tiles.gone_bytes = (SIM_LAYERS * SIM_TILES + 7) / 8;
    tiles.gone[0] = 0x80;  /* tile 0 of the top floor */
    wb_init(&wb, buf, sizeof buf);
    hf_put_tiles(&wb, &tiles);
    world_apply_event(&w, buf, wb.len, 0);
    CHECK(w.have_board && w.arena.tiles[0][0].state == TILE_GONE && w.arena.tiles[0][1].state == TILE_SOLID);

    msg_tile_events ev = {.tick = 101, .count = 1, .t = {{.layer = 0, .q = 0, .r = 0}}};
    wb_init(&wb, buf, sizeof buf);
    hf_put_tile_events(&wb, &ev);
    world_apply_event(&w, buf, wb.len, 0);
    int centre = sim_tile_index(0, 0);
    CHECK(w.arena.tiles[0][centre].state == TILE_TOUCHED);

    /* Snapshots: a newer one is used, an older one that arrives later is not,
     * and the touched tile falls once the server's time passes its moment. */
    world_apply_snapshot(&w, snapshot(105, 7, 1.5f), 1000);
    CHECK(w.tick == 105 && w.body_count == 1 && world_body_by_id(&w, 7)->x == 1.5f);
    world_apply_snapshot(&w, snapshot(104, 7, 9.0f), 1001);
    CHECK(w.tick == 105 && world_body_by_id(&w, 7)->x == 1.5f && w.snapshots_late == 1);
    CHECK(w.arena.tiles[0][centre].state == TILE_TOUCHED);
    world_apply_snapshot(&w, snapshot(101 + SIM_FALL_TICKS, 7, 2.0f), 1002);
    CHECK(w.arena.tiles[0][centre].state == TILE_GONE);

    /* A knock-out keeps who and where, for the renderer to show its own way. */
    msg_eliminated ko = {.player_id = 7, .place = 3, .tick = 130};
    wb_init(&wb, buf, sizeof buf);
    hf_put_eliminated(&wb, &ko);
    world_apply_event(&w, buf, wb.len, 5000);
    CHECK(w.note_count == 1 && w.notes[0].knockout && w.notes[0].player == 7 && w.notes[0].place == 3);
    CHECK(w.notes[0].at_ms == 5000 && w.notes[0].tick == 130 && strstr(w.notes[0].text, "#3"));

    /* A second WELCOME (after a resync) forgets the old board and bodies,
     * but not what just happened. */
    world_apply_event(&w, buf, welcome(7, 300), 0);
    CHECK(w.welcomed && !w.have_board && w.body_count == 0 && w.arena.tiles[0][centre].state == TILE_SOLID);
    CHECK(w.note_count == 1 && w.notes[0].knockout);

    printf("world: all checks passed\n");
    return 0;
}
