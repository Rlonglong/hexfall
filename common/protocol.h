#ifndef HF_PROTOCOL_H
#define HF_PROTOCOL_H

#include <stdint.h>

#include "wire.h"

/* The game protocol. A message is
 *
 *     u8 type | u16 payload length | payload
 *
 * and the same message bytes travel inside a UDP packet (desktop clients and
 * bots, see reliable.h) or inside a WebSocket binary frame (phones, later).
 * Only the envelope differs between the two; the messages never do.
 * docs/PROTOCOL.md lists every field; web/protocol.js is the browser twin. */

#define HF_PROTOCOL_VERSION 1
#define HF_MAX_PACKET 1200     /* UDP payload bytes; stays under common path MTUs */
#define HF_MAX_MSG 1100        /* largest single message, its 3-byte header included */
#define HF_MAX_PLAYERS 32
#define HF_NAME_MAX 24         /* UTF-8 bytes, without the terminating NUL */
#define HF_INPUT_REDUNDANCY 4  /* every INPUT repeats the newest 4 samples */
#define HF_MAX_TILE_EVENTS 48
#define HF_MAX_TOUCHED 100
#define HF_TILE_BITMAP_BYTES 128
#define HF_ERROR_TEXT_MAX 63
#define HF_POS_SCALE 100.0f    /* positions and velocities travel in 1/100 world units */

/* UDP envelope: u16 magic, u8 protocol version, u8 flags (0), u64 session.
 * The session is 0 until the client has received WELCOME. */
#define HF_UDP_MAGIC 0x4846u   /* "HF" */
#define HF_UDP_HEADER_BYTES 12

enum hf_msg_type {
    /* client -> server */
    MSG_HELLO = 1,
    MSG_READY = 2,
    MSG_INPUT = 3,
    MSG_PING = 4,
    MSG_BYE = 5,
    /* server -> client */
    MSG_WELCOME = 64,
    MSG_PONG = 65,
    MSG_ROOM = 66,
    MSG_SNAPSHOT = 67,
    MSG_TILES = 68,
    MSG_TILE_EVENTS = 69,
    MSG_ELIMINATED = 70,
    MSG_ERROR = 71,
};

enum hf_client_kind { CLIENT_DESKTOP = 0, CLIENT_PHONE = 1, CLIENT_BOT = 2 };
enum hf_phase { PHASE_LOBBY = 0, PHASE_COUNTDOWN = 1, PHASE_PLAYING = 2, PHASE_RESULT = 3 };
enum hf_error_code { ERR_VERSION = 1, ERR_FULL = 2, ERR_KICKED = 3 };

enum { BTN_JUMP = 1 };                                  /* input_sample.buttons */
enum { RP_READY = 1, RP_ALIVE = 2, RP_CONNECTED = 4 };  /* room_player.flags */
enum { SP_GROUNDED = 1, SP_ALIVE = 2 };                 /* snap_player.flags */

/* 1: must arrive, in order (reliable channel). 0: newest wins, a loss is fine. */
int hf_msg_is_reliable(uint8_t type);
const char *hf_msg_name(uint8_t type);

/* Drops control characters, keeps at most HF_NAME_MAX bytes and removes a
 * UTF-8 sequence that the cut left incomplete. */
void hf_clean_name(char *name);

/* ---- client -> server ---- */

typedef struct {
    uint16_t version;
    uint8_t kind;
    uint64_t resume_token;  /* 0 for a new player; the old session id to reclaim a slot */
    char name[HF_NAME_MAX + 1];
} msg_hello;

typedef struct { uint8_t ready; } msg_ready;

/* Stick axes are world-space directions, -127..127. BTN_JUMP is set in the
 * sample taken right after jump was pressed. */
typedef struct {
    uint16_t seq;
    int8_t mx, mz;
    uint8_t buttons;
} input_sample;

typedef struct {
    uint8_t count;
    input_sample s[HF_INPUT_REDUNDANCY];  /* s[0] is the newest */
} msg_input;

typedef struct { uint32_t client_ms; } msg_ping;
typedef struct { uint8_t reason; } msg_bye;

/* ---- server -> client ---- */

typedef struct {
    uint64_t token;      /* session id; also the key to reconnect */
    uint16_t player_id;
    uint8_t color;
    uint8_t tick_hz;
    uint32_t server_ms;
    uint32_t tick;
} msg_welcome;

typedef struct { uint32_t client_ms, server_ms; } msg_pong;

typedef struct {
    uint16_t id;
    uint8_t color, flags, place;  /* place: 1 = winner, 0 = still in or not finished */
    char name[HF_NAME_MAX + 1];
} room_player;

typedef struct {
    uint8_t phase;
    uint32_t tick, phase_end_tick;
    uint8_t count;
    room_player p[HF_MAX_PLAYERS];
} msg_room;

typedef struct {
    uint16_t id;
    int16_t x, y, z, vx, vy, vz;  /* fixed point, HF_POS_SCALE */
    uint16_t facing;              /* 65536 = one full turn */
    uint8_t flags, layer;
} snap_player;

typedef struct {
    uint32_t tick;
    uint16_t ack_input;  /* newest INPUT seq this client's player was simulated with */
    uint8_t count;
    snap_player p[HF_MAX_PLAYERS];
} msg_snapshot;

typedef struct {
    uint8_t layer;
    int8_t q, r;  /* axial hex coordinates */
} tile_ref;

/* Tiles touched at `tick`. Each falls SIM_FALL_TICKS later, which every
 * client can work out by itself. */
typedef struct {
    uint32_t tick;
    uint8_t count;
    tile_ref t[HF_MAX_TILE_EVENTS];
} msg_tile_events;

typedef struct {
    tile_ref t;
    uint32_t touch_tick;
} touched_tile;

/* The whole board: one "gone" bit per tile in canonical order (sim.h),
 * most significant bit first, plus the tiles that are touched but not gone. */
typedef struct {
    uint32_t tick;
    uint8_t layers, ring;
    uint16_t gone_bytes;
    uint8_t gone[HF_TILE_BITMAP_BYTES];
    uint8_t touched_count;
    touched_tile touched[HF_MAX_TOUCHED];
} msg_tiles;

typedef struct {
    uint16_t player_id;
    uint8_t place;
    uint32_t tick;
} msg_eliminated;

typedef struct {
    uint8_t code;
    char text[HF_ERROR_TEXT_MAX + 1];
} msg_error;

/* Splits the next message off r. Returns 1 and sets type/payload, or 0 at the
 * end of the buffer or when the header claims more bytes than there are. */
int hf_next_msg(rbuf *r, uint8_t *type, rbuf *payload);

/* Encoders append one complete message to w; check w->overflow afterwards.
 * Decoders read a payload from hf_next_msg and return 1 if it was well formed.
 * Extra trailing bytes are ignored so newer versions can append fields. */
void hf_put_hello(wbuf *w, const msg_hello *m);
void hf_put_ready(wbuf *w, const msg_ready *m);
void hf_put_input(wbuf *w, const msg_input *m);
void hf_put_ping(wbuf *w, const msg_ping *m);
void hf_put_bye(wbuf *w, const msg_bye *m);
void hf_put_welcome(wbuf *w, const msg_welcome *m);
void hf_put_pong(wbuf *w, const msg_pong *m);
void hf_put_room(wbuf *w, const msg_room *m);
void hf_put_snapshot(wbuf *w, const msg_snapshot *m);
void hf_put_tiles(wbuf *w, const msg_tiles *m);
void hf_put_tile_events(wbuf *w, const msg_tile_events *m);
void hf_put_eliminated(wbuf *w, const msg_eliminated *m);
void hf_put_error(wbuf *w, const msg_error *m);

int hf_get_hello(rbuf *p, msg_hello *m);
int hf_get_ready(rbuf *p, msg_ready *m);
int hf_get_input(rbuf *p, msg_input *m);
int hf_get_ping(rbuf *p, msg_ping *m);
int hf_get_bye(rbuf *p, msg_bye *m);
int hf_get_welcome(rbuf *p, msg_welcome *m);
int hf_get_pong(rbuf *p, msg_pong *m);
int hf_get_room(rbuf *p, msg_room *m);
int hf_get_snapshot(rbuf *p, msg_snapshot *m);
int hf_get_tiles(rbuf *p, msg_tiles *m);
int hf_get_tile_events(rbuf *p, msg_tile_events *m);
int hf_get_eliminated(rbuf *p, msg_eliminated *m);
int hf_get_error(rbuf *p, msg_error *m);

/* UDP envelope helpers. */
void hf_put_udp_header(wbuf *w, uint64_t session);
int hf_get_udp_header(rbuf *r, uint64_t *session);

#endif
