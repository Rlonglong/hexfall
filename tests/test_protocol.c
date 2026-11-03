#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "sim.h"
#include "util.h"

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static uint8_t buf[HF_MAX_MSG];

/* Encodes into buf and returns a reader positioned on the single message's payload. */
static rbuf only_message(wbuf *w, uint8_t expect_type)
{
    CHECK(!w->overflow);
    rbuf r, payload;
    uint8_t type;
    rb_init(&r, w->buf, w->len);
    CHECK(hf_next_msg(&r, &type, &payload));
    CHECK(type == expect_type);
    CHECK(rb_left(&r) == 0);
    return payload;
}

static void test_roundtrips(void)
{
    wbuf w;

    msg_hello h = {.version = HF_PROTOCOL_VERSION, .kind = CLIENT_PHONE, .resume_token = 0x0123456789ABCDEFull};
    strcpy(h.name, "小明");
    wb_init(&w, buf, sizeof buf);
    hf_put_hello(&w, &h);
    rbuf p = only_message(&w, MSG_HELLO);
    msg_hello h2;
    CHECK(hf_get_hello(&p, &h2));
    CHECK(h2.version == h.version && h2.kind == h.kind && h2.resume_token == h.resume_token);
    CHECK(strcmp(h2.name, "小明") == 0);

    msg_input in = {.count = 3};
    for (int i = 0; i < 3; i++)
        in.s[i] = (input_sample){.seq = (uint16_t)(65535 - i), .mx = (int8_t)(-127 + i), .mz = 127, .buttons = (uint8_t)(i & 1)};
    wb_init(&w, buf, sizeof buf);
    hf_put_input(&w, &in);
    p = only_message(&w, MSG_INPUT);
    msg_input in2;
    CHECK(hf_get_input(&p, &in2));
    CHECK(in2.count == 3);
    for (int i = 0; i < 3; i++)  /* field by field: the struct has padding */
        CHECK(in.s[i].seq == in2.s[i].seq && in.s[i].mx == in2.s[i].mx && in.s[i].mz == in2.s[i].mz &&
              in.s[i].buttons == in2.s[i].buttons);

    msg_room room = {.phase = PHASE_PLAYING, .tick = 77, .phase_end_tick = 99, .count = HF_MAX_PLAYERS};
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        room.p[i] = (room_player){.id = (uint16_t)(i + 1), .color = (uint8_t)i, .flags = RP_READY, .place = 0};
        memset(room.p[i].name, 'x', HF_NAME_MAX);  /* longest possible names must still fit */
    }
    wb_init(&w, buf, sizeof buf);
    hf_put_room(&w, &room);
    p = only_message(&w, MSG_ROOM);
    msg_room room2;
    CHECK(hf_get_room(&p, &room2));
    CHECK(room2.count == HF_MAX_PLAYERS && room2.p[31].id == 32 && strlen(room2.p[31].name) == HF_NAME_MAX);

    msg_snapshot snap = {.tick = 123456, .ack_input = 4321, .count = HF_MAX_PLAYERS};
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        snap.p[i] = (snap_player){.id = (uint16_t)i, .x = (int16_t)(-32767 + i), .y = -2400, .z = 32767, .vx = 1,
                                  .vy = -1, .vz = 0, .facing = 65535, .flags = SP_ALIVE, .layer = 2};
    wb_init(&w, buf, sizeof buf);
    hf_put_snapshot(&w, &snap);
    p = only_message(&w, MSG_SNAPSHOT);
    msg_snapshot snap2;
    CHECK(hf_get_snapshot(&p, &snap2));
    CHECK(snap2.tick == snap.tick && snap2.ack_input == snap.ack_input && snap2.count == snap.count);
    CHECK(memcmp(snap.p, snap2.p, sizeof snap.p[0] * HF_MAX_PLAYERS) == 0);

    msg_tiles tiles;
    memset(&tiles, 0, sizeof tiles);
    tiles.tick = 9;
    tiles.layers = SIM_LAYERS;
    tiles.ring = SIM_RING;
    tiles.gone_bytes = (SIM_LAYERS * SIM_TILES + 7) / 8;
    tiles.gone[0] = 0x80;
    tiles.gone[tiles.gone_bytes - 1] = 0x20;
    tiles.touched_count = HF_MAX_TOUCHED;
    for (int i = 0; i < HF_MAX_TOUCHED; i++)
        tiles.touched[i] = (touched_tile){{(uint8_t)(i % 3), (int8_t)(i % 17 - 8), -3}, (uint32_t)i};
    wb_init(&w, buf, sizeof buf);
    hf_put_tiles(&w, &tiles);
    p = only_message(&w, MSG_TILES);
    msg_tiles tiles2;
    CHECK(hf_get_tiles(&p, &tiles2));
    CHECK(tiles2.gone_bytes == tiles.gone_bytes && memcmp(tiles.gone, tiles2.gone, tiles.gone_bytes) == 0);
    CHECK(tiles2.touched_count == HF_MAX_TOUCHED && tiles2.touched[99].touch_tick == 99);
    CHECK(tiles2.touched[99].t.q == tiles.touched[99].t.q);

    msg_error err = {.code = ERR_FULL};
    strcpy(err.text, "room is full");
    wb_init(&w, buf, sizeof buf);
    hf_put_error(&w, &err);
    p = only_message(&w, MSG_ERROR);
    msg_error err2;
    CHECK(hf_get_error(&p, &err2) && err2.code == ERR_FULL && strcmp(err2.text, "room is full") == 0);
}

/* Cutting a message anywhere must be detected, never read past the end. */
static void test_truncation(void)
{
    msg_snapshot snap = {.tick = 1, .count = 5};
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_snapshot(&w, &snap);
    CHECK(!w.overflow);
    for (size_t cut = 0; cut < w.len; cut++) {
        rbuf r, payload;
        uint8_t type;
        rb_init(&r, buf, cut);
        if (hf_next_msg(&r, &type, &payload)) {
            msg_snapshot out;
            CHECK(!hf_get_snapshot(&payload, &out));
        }
    }
}

/* Throw random bytes at the decoders. Run it under `make asan` to catch any
 * out-of-bounds read. */
static void test_garbage(void)
{
    uint8_t junk[300];
    for (int round = 0; round < 200000; round++) {
        size_t n = (size_t)(hf_random01() * sizeof junk);
        for (size_t i = 0; i < n; i++)
            junk[i] = (uint8_t)(hf_random01() * 256);
        if (n > 0 && hf_random01() < 0.5)
            junk[0] = (uint8_t)(MSG_HELLO + (int)(hf_random01() * 72));
        rbuf r, payload;
        uint8_t type;
        rb_init(&r, junk, n);
        while (hf_next_msg(&r, &type, &payload)) {
            union {
                msg_hello hello;
                msg_input input;
                msg_room room;
                msg_snapshot snap;
                msg_tiles tiles;
                msg_tile_events events;
                msg_error error;
            } u;
            switch (type) {
            case MSG_HELLO: hf_get_hello(&payload, &u.hello); hf_clean_name(u.hello.name); break;
            case MSG_INPUT: hf_get_input(&payload, &u.input); break;
            case MSG_ROOM: hf_get_room(&payload, &u.room); break;
            case MSG_SNAPSHOT: hf_get_snapshot(&payload, &u.snap); break;
            case MSG_TILES: hf_get_tiles(&payload, &u.tiles); break;
            case MSG_TILE_EVENTS: hf_get_tile_events(&payload, &u.events); break;
            case MSG_ERROR: hf_get_error(&payload, &u.error); break;
            default: break;
            }
        }
    }
}

static void test_clean_name(void)
{
    char a[64] = "Bot\x01\x1f-01\x7f";
    hf_clean_name(a);
    CHECK(strcmp(a, "Bot-01") == 0);

    /* 9 Chinese characters are 27 bytes; 24 bytes keep exactly 8 of them. */
    char b[64] = "一二三四五六七八九";
    hf_clean_name(b);
    CHECK(strcmp(b, "一二三四五六七八") == 0);

    /* "ab" + 8 characters = 26 bytes; the cut at 24 splits the 8th, which must go. */
    char c[64] = "ab一二三四五六七八";
    hf_clean_name(c);
    CHECK(strcmp(c, "ab一二三四五六七") == 0);

    char d[8] = "\x80\x80\x80";
    hf_clean_name(d);
    CHECK(d[0] == 0);
}

static void test_tiles_and_sim(void)
{
    int count = 0;
    for (int q = -SIM_RING; q <= SIM_RING; q++)
        for (int r = -SIM_RING; r <= SIM_RING; r++)
            count += sim_tile_index(q, r) >= 0;
    CHECK(count == SIM_TILES);
    int q, r;
    sim_tile_qr(0, &q, &r);
    CHECK(q == -SIM_RING && r == 0);
    sim_tile_qr(SIM_TILES - 1, &q, &r);
    CHECK(q == SIM_RING && r == 0);
    for (int i = 0; i < SIM_TILES; i++) {
        float x, z;
        int q2, r2;
        sim_tile_qr(i, &q, &r);
        CHECK(sim_tile_index(q, r) == i);
        sim_tile_center(q, r, &x, &z);
        CHECK(sim_world_tile(x, z, &q2, &r2) && q2 == q && r2 == r);
    }

    /* Standing still: the tile underfoot falls after SIM_FALL_TICKS, and the
     * body drops to the floor below. */
    sim_arena a;
    sim_body b;
    sim_input idle = {0};
    sim_arena_reset(&a);
    sim_body_spawn(&b, 0, 0);
    uint32_t tick = 1;
    for (; tick < 200 && b.layer == 0; tick++) {
        sim_body_step(&b, &idle, &a, tick, NULL, NULL);
        sim_arena_tick(&a, tick);
    }
    CHECK(b.layer == 1 && b.alive);
    CHECK(tick > SIM_FALL_TICKS);
}

int main(void)
{
    test_roundtrips();
    test_truncation();
    test_garbage();
    test_clean_name();
    test_tiles_and_sim();
    printf("protocol: all checks passed\n");
    return 0;
}
