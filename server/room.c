#include <math.h>
#include <stdio.h>
#include <string.h>

#include "input_queue.h"
#include "server.h"
#include "sim.h"
#include "util.h"

/* The room: one lobby and one game at a time. The server is the only one
 * who decides where everybody is, which tile fell and who is out; clients
 * send inputs and draw what they are told.
 *
 *   LOBBY --(enough players, all ready)--> COUNTDOWN --(3 s)--> PLAYING
 *     ^                                       |                    |
 *     +------(someone left or un-readied)-----+      (one left) RESULT --(5 s)--> LOBBY
 *
 * For the first START_GRACE_TICKS of a round the floor holds (the arena's
 * armed_tick): everybody can find their feet before tiles start to fall.
 * The ROOM message's phase_end_tick says when, while PLAYING. Back in the
 * lobby the board is whole again, so the countdown shows the next round's. */

#define TICK_HZ SIM_TICK_HZ
#define COUNTDOWN_TICKS (3 * TICK_HZ)
#define START_GRACE_TICKS (2 * TICK_HZ)
#define RESULT_TICKS (5 * TICK_HZ)
#define COLORS 16

typedef struct {
    int used;
    uint16_t id;
    uint64_t token;
    char name[HF_NAME_MAX + 1];
    uint8_t color, kind;
    conn *c;          /* NULL while disconnected; the slot waits RECONNECT_GRACE_MS */
    uint64_t lost_ms;
    int ready, playing, place;
    sim_body body;
    inputq in;        /* one input per tick; in.applied is echoed in snapshots */
} player;

static struct {
    player p[HF_MAX_PLAYERS];
    uint16_t last_id;
    int phase;
    uint32_t tick, phase_end_tick;
    int min_players, round_players, alive;
    int room_dirty, tiles_dirty;
    sim_arena arena;
    tile_ref touched[HF_MAX_TILE_EVENTS];
    int touched_n;
} R;

static const char *phase_name(int phase)
{
    switch (phase) {
    case PHASE_LOBBY: return "LOBBY";
    case PHASE_COUNTDOWN: return "COUNTDOWN";
    case PHASE_PLAYING: return "PLAYING";
    case PHASE_RESULT: return "RESULT";
    default: return "?";
    }
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case CLIENT_DESKTOP: return "desktop";
    case CLIENT_PHONE: return "phone";
    case CLIENT_BOT: return "bot";
    default: return "unknown";
    }
}

void room_init(int min_players)
{
    memset(&R, 0, sizeof R);
    R.min_players = min_players < 1 ? 1 : min_players;
    R.phase = PHASE_LOBBY;
    sim_arena_reset(&R.arena);
}

static void send_to(conn *c, const wbuf *w)
{
    if (c && !w->overflow)
        conn_send(c, w->buf, w->len);
}

static void broadcast(const wbuf *w)
{
    if (w->overflow)
        return;
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        if (R.p[i].used && R.p[i].c)
            conn_send(R.p[i].c, w->buf, w->len);
}

static void set_phase(int phase, uint32_t end_tick)
{
    if (R.phase != phase)
        hf_log("phase %s -> %s", phase_name(R.phase), phase_name(phase));
    R.phase = phase;
    R.phase_end_tick = end_tick;
    R.room_dirty = 1;
}

static void send_error(conn *c, uint8_t code, const char *text, uint64_t now_ms)
{
    msg_error m = {.code = code};
    snprintf(m.text, sizeof m.text, "%s", text);
    uint8_t buf[128];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_error(&w, &m);
    send_to(c, &w);
    conn_close_soon(c, now_ms);
}

static void send_tiles(conn *only)
{
    msg_tiles m;
    memset(&m, 0, sizeof m);
    m.tick = R.tick;
    m.layers = SIM_LAYERS;
    m.ring = SIM_RING;
    int bit = 0;
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++, bit++) {
            const sim_tile *t = &R.arena.tiles[layer][i];
            if (t->state == TILE_GONE)
                m.gone[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
            else if (t->state == TILE_TOUCHED && m.touched_count < HF_MAX_TOUCHED) {
                int q, r;
                sim_tile_qr(i, &q, &r);
                touched_tile *tt = &m.touched[m.touched_count++];
                tt->t = (tile_ref){(uint8_t)layer, (int8_t)q, (int8_t)r};
                tt->touch_tick = t->touch_tick;
            }
        }
    m.gone_bytes = (uint16_t)((bit + 7) / 8);
    uint8_t buf[HF_MAX_MSG];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_tiles(&w, &m);
    if (only)
        send_to(only, &w);
    else
        broadcast(&w);
}

static player *find_token(uint64_t token)
{
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        if (R.p[i].used && R.p[i].token == token)
            return &R.p[i];
    return NULL;
}

static player *free_slot(void)
{
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        if (!R.p[i].used)
            return &R.p[i];
    return NULL;
}

static void on_hello(conn *c, rbuf *payload, uint64_t now_ms)
{
    msg_hello m;
    if (!hf_get_hello(payload, &m))
        return;
    if (m.version != HF_PROTOCOL_VERSION) {
        send_error(c, ERR_VERSION, "protocol version mismatch", now_ms);
        return;
    }
    player *p = m.resume_token ? find_token(m.resume_token) : NULL;
    int resumed = p != NULL;
    if (p) {
        if (p->c && p->c != c) {
            /* The old connection is still around (the client restarted before
             * we noticed): retire it so the token belongs to one connection. */
            p->c->player = -1;
            p->c->token = 0;
            conn_close_soon(p->c, now_ms);
        }
        /* The player keeps their slot but gets this connection's fresh token.
         * Packets still in flight from the old connection carry the old token
         * and are now rejected as stale instead of mixing their sequence
         * numbers and acks into the new connection's. */
        p->token = c->token;
    } else {
        p = free_slot();
        if (!p) {
            send_error(c, ERR_FULL, "room is full", now_ms);
            return;
        }
        memset(p, 0, sizeof *p);
        p->used = 1;
        if (++R.last_id == 0)
            R.last_id = 1;
        p->id = R.last_id;
        p->token = c->token;
        p->color = (uint8_t)(p->id % COLORS);
        p->kind = m.kind;
        hf_clean_name(m.name);
        if (m.name[0])
            snprintf(p->name, sizeof p->name, "%s", m.name);
        else
            snprintf(p->name, sizeof p->name, "Player %u", p->id);
    }
    p->c = c;
    p->lost_ms = 0;
    c->player = (int)(p - R.p);

    msg_welcome w = {
        .token = p->token,
        .player_id = p->id,
        .color = p->color,
        .tick_hz = TICK_HZ,
        .server_ms = (uint32_t)now_ms,
        .tick = R.tick,
    };
    uint8_t buf[64];
    wbuf wb;
    wb_init(&wb, buf, sizeof buf);
    hf_put_welcome(&wb, &w);
    send_to(c, &wb);
    send_tiles(c);
    R.room_dirty = 1;
    hf_log("%s %s #%u \"%s\" (%s)", conn_addr_str(c), resumed ? "resumed" : "joined as", p->id,
           p->name, kind_name(p->kind));
}

static void on_bye(conn *c, player *p, uint64_t now_ms)
{
    hf_log("#%u \"%s\" left", p->id, p->name);
    if (R.phase == PHASE_PLAYING && p->playing && p->place == 0)
        p->body.alive = 0;  /* leaving counts as falling out */
    p->c = NULL;
    p->lost_ms = now_ms - RECONNECT_GRACE_MS;  /* a deliberate exit gets no grace */
    c->player = -1;
    conn_close_soon(c, now_ms);
    R.room_dirty = 1;
}

void room_on_message(conn *c, uint8_t type, rbuf *payload, uint64_t now_ms)
{
    if (c->player < 0) {
        if (type == MSG_HELLO)
            on_hello(c, payload, now_ms);
        return;
    }
    player *p = &R.p[c->player];
    switch (type) {
    case MSG_READY: {
        msg_ready m;
        if (hf_get_ready(payload, &m) && p->ready != !!m.ready) {
            p->ready = !!m.ready;
            R.room_dirty = 1;
        }
        break;
    }
    case MSG_INPUT: {
        msg_input m;
        if (hf_get_input(payload, &m))
            inputq_add(&p->in, &m);
        break;
    }
    case MSG_PING: {
        msg_ping m;
        if (!hf_get_ping(payload, &m))
            break;
        msg_pong o = {.client_ms = m.client_ms, .server_ms = (uint32_t)now_ms};
        uint8_t buf[32];
        wbuf w;
        wb_init(&w, buf, sizeof buf);
        hf_put_pong(&w, &o);
        send_to(c, &w);
        break;
    }
    case MSG_BYE:
        on_bye(c, p, now_ms);
        break;
    default:
        break;  /* server-to-client or unknown types: ignore */
    }
}

void room_on_conn_lost(conn *c, uint64_t now_ms)
{
    if (c->player < 0)
        return;
    player *p = &R.p[c->player];
    c->player = -1;
    if (p->c != c)
        return;
    p->c = NULL;
    p->lost_ms = now_ms;
    p->ready = 0;
    inputq_clear(&p->in);  /* stand still until the player is back */
    R.room_dirty = 1;
    hf_log("#%u \"%s\" lost connection; keeping the slot for %d s", p->id, p->name,
           RECONNECT_GRACE_MS / 1000);
}

static void expire_lost_players(uint64_t now_ms)
{
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (!p->used || p->c || now_ms - p->lost_ms < RECONNECT_GRACE_MS)
            continue;
        if (R.phase == PHASE_PLAYING && p->playing && p->place == 0)
            continue;  /* still on the board; remove once the round has placed them */
        hf_log("#%u \"%s\" removed", p->id, p->name);
        p->used = 0;
        R.room_dirty = 1;
    }
}

static void start_round(void)
{
    int n = 0, k = 0;
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        if (R.p[i].used && R.p[i].c)
            n++;
    sim_arena_reset(&R.arena);
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (!p->used)
            continue;
        p->place = 0;
        p->playing = p->c != NULL;  /* someone who is away sits this round out */
        if (!p->playing)
            continue;
        float x, z;
        sim_spawn_point(k++, n, &x, &z);
        sim_body_spawn(&p->body, x, z);
        inputq_clear(&p->in);
    }
    R.round_players = n;
    R.alive = n;
    R.arena.armed_tick = R.tick + 1 + START_GRACE_TICKS;  /* the round's first tick is the next one */
    R.tiles_dirty = 1;
    set_phase(PHASE_PLAYING, R.arena.armed_tick);
    hf_log("round started with %d players", n);
}

static void on_touch(void *ctx, int layer, int q, int r)
{
    (void)ctx;
    if (R.touched_n < HF_MAX_TILE_EVENTS)
        R.touched[R.touched_n++] = (tile_ref){(uint8_t)layer, (int8_t)q, (int8_t)r};
    else
        R.tiles_dirty = 1;  /* too many for one event message: send the whole board */
}

static void finish_round(void)
{
    player *winner = NULL;
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (p->used && p->playing && p->place == 0 && p->body.alive)
            winner = p;
    }
    if (winner) {
        winner->place = 1;
        hf_log("winner: #%u \"%s\"", winner->id, winner->name);
    } else {
        hf_log("round over, nobody left standing");
    }
    set_phase(PHASE_RESULT, R.tick + RESULT_TICKS);
}

static void play_tick(void)
{
    sim_body *bodies[HF_MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (!p->used || !p->playing || !p->body.alive)
            continue;
        sim_input in = inputq_next(&p->in);
        sim_body_step(&p->body, &in, &R.arena, R.tick, on_touch, NULL);
        if (p->body.alive)
            bodies[n++] = &p->body;
    }
    sim_separate(bodies, n);
    sim_arena_tick(&R.arena, R.tick);

    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (!p->used || !p->playing || p->place != 0 || p->body.alive)
            continue;
        p->place = R.alive--;
        msg_eliminated e = {.player_id = p->id, .place = (uint8_t)p->place, .tick = R.tick};
        uint8_t buf[32];
        wbuf w;
        wb_init(&w, buf, sizeof buf);
        hf_put_eliminated(&w, &e);
        broadcast(&w);
        R.room_dirty = 1;
        hf_log("#%u \"%s\" fell into the slime, place %d", p->id, p->name, p->place);
    }
    if ((R.round_players >= 2 && R.alive <= 1) || R.alive <= 0)
        finish_round();
}

static int16_t to_fixed(float v)
{
    float s = v * HF_POS_SCALE;
    if (s > 32767.0f)
        s = 32767.0f;
    if (s < -32767.0f)
        s = -32767.0f;
    return (int16_t)lrintf(s);
}

static uint16_t to_turns(float radians)
{
    float f = radians / (2.0f * (float)M_PI);
    f -= floorf(f);
    return (uint16_t)((uint32_t)lrintf(f * 65536.0f) & 0xFFFF);
}

static void send_updates(void)
{
    uint8_t buf[HF_MAX_MSG];
    wbuf w;
    if (R.room_dirty) {
        msg_room m;
        memset(&m, 0, sizeof m);
        m.phase = (uint8_t)R.phase;
        m.tick = R.tick;
        m.phase_end_tick = R.phase_end_tick;
        for (int i = 0; i < HF_MAX_PLAYERS; i++) {
            player *p = &R.p[i];
            if (!p->used)
                continue;
            room_player *rp = &m.p[m.count++];
            rp->id = p->id;
            rp->color = p->color;
            rp->place = (uint8_t)p->place;
            rp->flags = (uint8_t)((p->ready ? RP_READY : 0) | (p->playing && p->body.alive ? RP_ALIVE : 0) |
                                  (p->c ? RP_CONNECTED : 0));
            snprintf(rp->name, sizeof rp->name, "%s", p->name);
        }
        wb_init(&w, buf, sizeof buf);
        hf_put_room(&w, &m);
        broadcast(&w);
        R.room_dirty = 0;
    }

    if (R.tiles_dirty) {
        send_tiles(NULL);
        R.tiles_dirty = 0;
    } else if (R.touched_n > 0) {
        msg_tile_events e = {.tick = R.tick, .count = (uint8_t)R.touched_n};
        memcpy(e.t, R.touched, sizeof R.touched[0] * (size_t)R.touched_n);
        wb_init(&w, buf, sizeof buf);
        hf_put_tile_events(&w, &e);
        broadcast(&w);
    }
    R.touched_n = 0;

    msg_snapshot s;
    memset(&s, 0, sizeof s);
    s.tick = R.tick;
    if (R.phase == PHASE_PLAYING || R.phase == PHASE_RESULT) {
        for (int i = 0; i < HF_MAX_PLAYERS; i++) {
            player *p = &R.p[i];
            if (!p->used || !p->playing)
                continue;
            snap_player *sp = &s.p[s.count++];
            sp->id = p->id;
            sp->x = to_fixed(p->body.x);
            sp->y = to_fixed(p->body.y);
            sp->z = to_fixed(p->body.z);
            sp->vx = to_fixed(p->body.vx);
            sp->vy = to_fixed(p->body.vy);
            sp->vz = to_fixed(p->body.vz);
            sp->facing = to_turns(p->body.facing);
            sp->flags = (uint8_t)((p->body.grounded ? SP_GROUNDED : 0) | (p->body.alive ? SP_ALIVE : 0));
            sp->layer = (uint8_t)p->body.layer;
        }
    }
    wb_init(&w, buf, sizeof buf);
    hf_put_snapshot(&w, &s);
    if (w.overflow)
        return;
    /* Encoded once; only ack_input differs per client. It sits right after the
     * type (1 byte), the length (2) and the tick (4). */
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        player *p = &R.p[i];
        if (!p->used || !p->c)
            continue;
        buf[7] = (uint8_t)(p->in.applied >> 8);
        buf[8] = (uint8_t)p->in.applied;
        conn_send(p->c, buf, w.len);
    }
}

void room_tick(uint64_t now_ms)
{
    R.tick++;
    expire_lost_players(now_ms);
    int connected = 0, ready = 0;
    for (int i = 0; i < HF_MAX_PLAYERS; i++)
        if (R.p[i].used && R.p[i].c) {
            connected++;
            ready += R.p[i].ready;
        }
    switch (R.phase) {
    case PHASE_LOBBY:
        if (connected >= R.min_players && ready == connected)
            set_phase(PHASE_COUNTDOWN, R.tick + COUNTDOWN_TICKS);
        break;
    case PHASE_COUNTDOWN:
        if (connected < R.min_players || ready < connected)
            set_phase(PHASE_LOBBY, 0);
        else if (R.tick >= R.phase_end_tick)
            start_round();
        break;
    case PHASE_PLAYING:
        play_tick();
        break;
    case PHASE_RESULT:
        if (R.tick >= R.phase_end_tick) {
            for (int i = 0; i < HF_MAX_PLAYERS; i++) {
                R.p[i].ready = 0;
                R.p[i].playing = 0;
            }
            sim_arena_reset(&R.arena);
            R.tiles_dirty = 1;
            set_phase(PHASE_LOBBY, 0);
        }
        break;
    }
    send_updates();
}

void room_stats(char *out, size_t cap)
{
    int players = 0, connected = 0, ready = 0, alive = 0;
    char waiting[64] = "";
    for (int i = 0; i < HF_MAX_PLAYERS; i++) {
        const player *p = &R.p[i];
        if (!p->used)
            continue;
        players++;
        connected += p->c != NULL;
        ready += p->c && p->ready;
        alive += p->playing && p->body.alive;
        if (p->c && !p->ready && strlen(waiting) < 40) {
            strcat(waiting, waiting[0] ? "," : " waiting for ");
            strncat(waiting, p->name, 12);
        }
    }
    snprintf(out, cap, "%s tick=%u players=%d connected=%d ready=%d alive=%d%s", phase_name(R.phase), R.tick,
             players, connected, ready, alive, R.phase == PHASE_LOBBY ? waiting : "");
}
