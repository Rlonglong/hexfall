#include "protocol.h"

#include <string.h>

int hf_msg_is_reliable(uint8_t type)
{
    switch (type) {
    case MSG_INPUT:
    case MSG_PING:
    case MSG_PONG:
    case MSG_SNAPSHOT:
        return 0;
    default:
        return 1;
    }
}

const char *hf_msg_name(uint8_t type)
{
    switch (type) {
    case MSG_HELLO: return "HELLO";
    case MSG_READY: return "READY";
    case MSG_INPUT: return "INPUT";
    case MSG_PING: return "PING";
    case MSG_BYE: return "BYE";
    case MSG_WELCOME: return "WELCOME";
    case MSG_PONG: return "PONG";
    case MSG_ROOM: return "ROOM";
    case MSG_SNAPSHOT: return "SNAPSHOT";
    case MSG_TILES: return "TILES";
    case MSG_TILE_EVENTS: return "TILE_EVENTS";
    case MSG_ELIMINATED: return "ELIMINATED";
    case MSG_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

void hf_clean_name(char *s)
{
    size_t n = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != 0x7F)
            s[n++] = (char)c;
    }
    if (n > HF_NAME_MAX)
        n = HF_NAME_MAX;
    s[n] = 0;

    /* If the last character's bytes were cut off, drop the rest of it too. */
    size_t start = n;
    while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80)
        start--;
    if (start == 0) {
        if (n > 0 && ((unsigned char)s[0] & 0xC0) == 0x80)
            s[0] = 0;  /* nothing but continuation bytes */
        return;
    }
    unsigned char lead = (unsigned char)s[start - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (lead >= 0x80 && n - (start - 1) < need)
        s[start - 1] = 0;
}

/* ---- framing ---- */

static size_t msg_begin(wbuf *w, uint8_t type)
{
    wb_u8(w, type);
    size_t at = w->len;
    wb_u16(w, 0);  /* patched by msg_end */
    return at;
}

static void msg_end(wbuf *w, size_t at)
{
    if (w->overflow)
        return;
    size_t n = w->len - at - 2;
    if (n > 0xFFFF || n + 3 > HF_MAX_MSG) {
        w->overflow = 1;
        return;
    }
    w->buf[at] = (uint8_t)(n >> 8);
    w->buf[at + 1] = (uint8_t)n;
}

int hf_next_msg(rbuf *r, uint8_t *type, rbuf *payload)
{
    if (rb_left(r) == 0)
        return 0;
    *type = rb_u8(r);
    uint16_t n = rb_u16(r);
    *payload = rb_sub(r, n);
    return !r->error;
}

static uint8_t cap_u8(unsigned v, unsigned max) { return (uint8_t)(v > max ? max : v); }

/* ---- client -> server ---- */

void hf_put_hello(wbuf *w, const msg_hello *m)
{
    size_t at = msg_begin(w, MSG_HELLO);
    wb_u16(w, m->version);
    wb_u8(w, m->kind);
    wb_u64(w, m->resume_token);
    wb_str(w, m->name);
    msg_end(w, at);
}

int hf_get_hello(rbuf *p, msg_hello *m)
{
    m->version = rb_u16(p);
    m->kind = rb_u8(p);
    m->resume_token = rb_u64(p);
    rb_str(p, m->name, sizeof m->name);
    return !p->error;
}

void hf_put_ready(wbuf *w, const msg_ready *m)
{
    size_t at = msg_begin(w, MSG_READY);
    wb_u8(w, m->ready);
    msg_end(w, at);
}

int hf_get_ready(rbuf *p, msg_ready *m)
{
    m->ready = rb_u8(p);
    return !p->error;
}

void hf_put_input(wbuf *w, const msg_input *m)
{
    size_t at = msg_begin(w, MSG_INPUT);
    uint8_t n = cap_u8(m->count, HF_INPUT_REDUNDANCY);
    wb_u8(w, n);
    for (int i = 0; i < n; i++) {
        wb_u16(w, m->s[i].seq);
        wb_i8(w, m->s[i].mx);
        wb_i8(w, m->s[i].mz);
        wb_u8(w, m->s[i].buttons);
    }
    msg_end(w, at);
}

int hf_get_input(rbuf *p, msg_input *m)
{
    m->count = cap_u8(rb_u8(p), HF_INPUT_REDUNDANCY);
    for (int i = 0; i < m->count; i++) {
        m->s[i].seq = rb_u16(p);
        m->s[i].mx = rb_i8(p);
        m->s[i].mz = rb_i8(p);
        m->s[i].buttons = rb_u8(p);
    }
    return !p->error;
}

void hf_put_ping(wbuf *w, const msg_ping *m)
{
    size_t at = msg_begin(w, MSG_PING);
    wb_u32(w, m->client_ms);
    msg_end(w, at);
}

int hf_get_ping(rbuf *p, msg_ping *m)
{
    m->client_ms = rb_u32(p);
    return !p->error;
}

void hf_put_bye(wbuf *w, const msg_bye *m)
{
    size_t at = msg_begin(w, MSG_BYE);
    wb_u8(w, m->reason);
    msg_end(w, at);
}

int hf_get_bye(rbuf *p, msg_bye *m)
{
    m->reason = rb_u8(p);
    return !p->error;
}

/* ---- server -> client ---- */

void hf_put_welcome(wbuf *w, const msg_welcome *m)
{
    size_t at = msg_begin(w, MSG_WELCOME);
    wb_u64(w, m->token);
    wb_u16(w, m->player_id);
    wb_u8(w, m->color);
    wb_u8(w, m->tick_hz);
    wb_u32(w, m->server_ms);
    wb_u32(w, m->tick);
    msg_end(w, at);
}

int hf_get_welcome(rbuf *p, msg_welcome *m)
{
    m->token = rb_u64(p);
    m->player_id = rb_u16(p);
    m->color = rb_u8(p);
    m->tick_hz = rb_u8(p);
    m->server_ms = rb_u32(p);
    m->tick = rb_u32(p);
    return !p->error;
}

void hf_put_pong(wbuf *w, const msg_pong *m)
{
    size_t at = msg_begin(w, MSG_PONG);
    wb_u32(w, m->client_ms);
    wb_u32(w, m->server_ms);
    msg_end(w, at);
}

int hf_get_pong(rbuf *p, msg_pong *m)
{
    m->client_ms = rb_u32(p);
    m->server_ms = rb_u32(p);
    return !p->error;
}

void hf_put_room(wbuf *w, const msg_room *m)
{
    size_t at = msg_begin(w, MSG_ROOM);
    uint8_t n = cap_u8(m->count, HF_MAX_PLAYERS);
    wb_u8(w, m->phase);
    wb_u32(w, m->tick);
    wb_u32(w, m->phase_end_tick);
    wb_u8(w, n);
    for (int i = 0; i < n; i++) {
        wb_u16(w, m->p[i].id);
        wb_u8(w, m->p[i].color);
        wb_u8(w, m->p[i].flags);
        wb_u8(w, m->p[i].place);
        wb_str(w, m->p[i].name);
    }
    msg_end(w, at);
}

int hf_get_room(rbuf *p, msg_room *m)
{
    memset(m, 0, sizeof *m);
    m->phase = rb_u8(p);
    m->tick = rb_u32(p);
    m->phase_end_tick = rb_u32(p);
    m->count = cap_u8(rb_u8(p), HF_MAX_PLAYERS);
    for (int i = 0; i < m->count; i++) {
        m->p[i].id = rb_u16(p);
        m->p[i].color = rb_u8(p);
        m->p[i].flags = rb_u8(p);
        m->p[i].place = rb_u8(p);
        rb_str(p, m->p[i].name, sizeof m->p[i].name);
    }
    return !p->error;
}

void hf_put_snapshot(wbuf *w, const msg_snapshot *m)
{
    size_t at = msg_begin(w, MSG_SNAPSHOT);
    uint8_t n = cap_u8(m->count, HF_MAX_PLAYERS);
    wb_u32(w, m->tick);
    wb_u16(w, m->ack_input);
    wb_u8(w, n);
    for (int i = 0; i < n; i++) {
        const snap_player *s = &m->p[i];
        wb_u16(w, s->id);
        wb_i16(w, s->x);
        wb_i16(w, s->y);
        wb_i16(w, s->z);
        wb_i16(w, s->vx);
        wb_i16(w, s->vy);
        wb_i16(w, s->vz);
        wb_u16(w, s->facing);
        wb_u8(w, s->flags);
        wb_u8(w, s->layer);
    }
    msg_end(w, at);
}

int hf_get_snapshot(rbuf *p, msg_snapshot *m)
{
    m->tick = rb_u32(p);
    m->ack_input = rb_u16(p);
    m->count = cap_u8(rb_u8(p), HF_MAX_PLAYERS);
    for (int i = 0; i < m->count; i++) {
        snap_player *s = &m->p[i];
        s->id = rb_u16(p);
        s->x = rb_i16(p);
        s->y = rb_i16(p);
        s->z = rb_i16(p);
        s->vx = rb_i16(p);
        s->vy = rb_i16(p);
        s->vz = rb_i16(p);
        s->facing = rb_u16(p);
        s->flags = rb_u8(p);
        s->layer = rb_u8(p);
    }
    return !p->error;
}

static void put_tile(wbuf *w, const tile_ref *t)
{
    wb_u8(w, t->layer);
    wb_i8(w, t->q);
    wb_i8(w, t->r);
}

static void get_tile(rbuf *p, tile_ref *t)
{
    t->layer = rb_u8(p);
    t->q = rb_i8(p);
    t->r = rb_i8(p);
}

void hf_put_tiles(wbuf *w, const msg_tiles *m)
{
    size_t at = msg_begin(w, MSG_TILES);
    uint16_t bytes = m->gone_bytes > HF_TILE_BITMAP_BYTES ? HF_TILE_BITMAP_BYTES : m->gone_bytes;
    uint8_t n = cap_u8(m->touched_count, HF_MAX_TOUCHED);
    wb_u32(w, m->tick);
    wb_u8(w, m->layers);
    wb_u8(w, m->ring);
    wb_u16(w, bytes);
    wb_bytes(w, m->gone, bytes);
    wb_u8(w, n);
    for (int i = 0; i < n; i++) {
        put_tile(w, &m->touched[i].t);
        wb_u32(w, m->touched[i].touch_tick);
    }
    msg_end(w, at);
}

int hf_get_tiles(rbuf *p, msg_tiles *m)
{
    memset(m, 0, sizeof *m);
    m->tick = rb_u32(p);
    m->layers = rb_u8(p);
    m->ring = rb_u8(p);
    uint16_t bytes = rb_u16(p);
    rbuf bits = rb_sub(p, bytes);
    m->gone_bytes = bytes > HF_TILE_BITMAP_BYTES ? HF_TILE_BITMAP_BYTES : bytes;
    rb_bytes(&bits, m->gone, m->gone_bytes);
    m->touched_count = cap_u8(rb_u8(p), HF_MAX_TOUCHED);
    for (int i = 0; i < m->touched_count; i++) {
        get_tile(p, &m->touched[i].t);
        m->touched[i].touch_tick = rb_u32(p);
    }
    return !p->error && !bits.error;
}

void hf_put_tile_events(wbuf *w, const msg_tile_events *m)
{
    size_t at = msg_begin(w, MSG_TILE_EVENTS);
    uint8_t n = cap_u8(m->count, HF_MAX_TILE_EVENTS);
    wb_u32(w, m->tick);
    wb_u8(w, n);
    for (int i = 0; i < n; i++)
        put_tile(w, &m->t[i]);
    msg_end(w, at);
}

int hf_get_tile_events(rbuf *p, msg_tile_events *m)
{
    m->tick = rb_u32(p);
    m->count = cap_u8(rb_u8(p), HF_MAX_TILE_EVENTS);
    for (int i = 0; i < m->count; i++)
        get_tile(p, &m->t[i]);
    return !p->error;
}

void hf_put_eliminated(wbuf *w, const msg_eliminated *m)
{
    size_t at = msg_begin(w, MSG_ELIMINATED);
    wb_u16(w, m->player_id);
    wb_u8(w, m->place);
    wb_u32(w, m->tick);
    msg_end(w, at);
}

int hf_get_eliminated(rbuf *p, msg_eliminated *m)
{
    m->player_id = rb_u16(p);
    m->place = rb_u8(p);
    m->tick = rb_u32(p);
    return !p->error;
}

void hf_put_error(wbuf *w, const msg_error *m)
{
    size_t at = msg_begin(w, MSG_ERROR);
    wb_u8(w, m->code);
    wb_str(w, m->text);
    msg_end(w, at);
}

int hf_get_error(rbuf *p, msg_error *m)
{
    m->code = rb_u8(p);
    rb_str(p, m->text, sizeof m->text);
    return !p->error;
}

/* ---- UDP envelope ---- */

void hf_put_udp_header(wbuf *w, uint64_t session)
{
    wb_u16(w, HF_UDP_MAGIC);
    wb_u8(w, HF_PROTOCOL_VERSION);
    wb_u8(w, 0);
    wb_u64(w, session);
}

int hf_get_udp_header(rbuf *r, uint64_t *session)
{
    uint16_t magic = rb_u16(r);
    uint8_t version = rb_u8(r);
    (void)rb_u8(r);
    *session = rb_u64(r);
    return !r->error && magic == HF_UDP_MAGIC && version == HF_PROTOCOL_VERSION;
}
