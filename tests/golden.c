#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "sim.h"

/* Writes one of every message with known values. tests/test_protocol_js.mjs
 * decodes the file with web/protocol.js and compares field by field, and
 * re-encodes the client messages to compare byte by byte. Change a message
 * here and there together. */

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "golden.bin";
    static uint8_t out[8192];
    wbuf w;
    wb_init(&w, out, sizeof out);

    msg_hello hello = {.version = HF_PROTOCOL_VERSION, .kind = CLIENT_PHONE, .resume_token = 0x0123456789ABCDEFull};
    strcpy(hello.name, "小明");
    hf_put_hello(&w, &hello);

    msg_ready ready = {.ready = 1};
    hf_put_ready(&w, &ready);

    msg_input input = {.count = 2};
    input.s[0] = (input_sample){.seq = 65535, .mx = -127, .mz = 64, .buttons = BTN_JUMP};
    input.s[1] = (input_sample){.seq = 65534, .mx = 0, .mz = -1, .buttons = 0};
    hf_put_input(&w, &input);

    msg_ping ping = {.client_ms = 4000000000u};
    hf_put_ping(&w, &ping);

    msg_bye bye = {.reason = 0};
    hf_put_bye(&w, &bye);

    msg_welcome welcome = {.token = 0xFEDCBA9876543210ull, .player_id = 7, .color = 3, .tick_hz = 30,
                           .server_ms = 123456, .tick = 789};
    hf_put_welcome(&w, &welcome);

    msg_pong pong = {.client_ms = 1, .server_ms = 4294967295u};
    hf_put_pong(&w, &pong);

    msg_room room = {.phase = PHASE_PLAYING, .tick = 1000, .phase_end_tick = 0, .count = 2};
    room.p[0] = (room_player){.id = 7, .color = 3, .flags = RP_READY | RP_ALIVE | RP_CONNECTED, .place = 0};
    strcpy(room.p[0].name, "小明");
    room.p[1] = (room_player){.id = 9, .color = 1, .flags = RP_CONNECTED, .place = 2};
    strcpy(room.p[1].name, "Bot-01");
    hf_put_room(&w, &room);

    msg_snapshot snap = {.tick = 1001, .ack_input = 65535, .count = 1};
    snap.p[0] = (snap_player){.id = 7, .x = -1234, .y = 0, .z = 2500, .vx = 800, .vy = -1300, .vz = 0,
                              .facing = 49152, .flags = SP_GROUNDED | SP_ALIVE, .layer = 1};
    hf_put_snapshot(&w, &snap);

    msg_tile_events events = {.tick = 1002, .count = 2};
    events.t[0] = (tile_ref){.layer = 0, .q = -8, .r = 8};
    events.t[1] = (tile_ref){.layer = 2, .q = 0, .r = 0};
    hf_put_tile_events(&w, &events);

    msg_tiles tiles;
    memset(&tiles, 0, sizeof tiles);
    tiles.tick = 1003;
    tiles.layers = SIM_LAYERS;
    tiles.ring = SIM_RING;
    tiles.gone_bytes = (SIM_LAYERS * SIM_TILES + 7) / 8;  /* 651 bits -> 82 bytes */
    tiles.gone[0] = 0x80;                                 /* tile 0 of layer 0 is gone */
    tiles.gone[tiles.gone_bytes - 1] = 0x20;              /* bit 650, the last tile of layer 2 */
    tiles.touched_count = 1;
    tiles.touched[0] = (touched_tile){.t = {.layer = 1, .q = 3, .r = -2}, .touch_tick = 1000};
    hf_put_tiles(&w, &tiles);

    msg_eliminated elim = {.player_id = 9, .place = 2, .tick = 1004};
    hf_put_eliminated(&w, &elim);

    msg_error err = {.code = ERR_FULL};
    strcpy(err.text, "room is full");
    hf_put_error(&w, &err);

    if (w.overflow) {
        fprintf(stderr, "golden: buffer too small\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(out, 1, w.len, f) != w.len) {
        perror(path);
        return 1;
    }
    fclose(f);
    printf("golden: wrote %zu bytes to %s\n", w.len, path);
    return 0;
}
