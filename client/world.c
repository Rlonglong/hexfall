#include "world.h"

#include <stdio.h>
#include <string.h>

void world_init(world *w)
{
    memset(w, 0, sizeof *w);
    w->phase = PHASE_LOBBY;
    sim_arena_reset(&w->arena);
}

static world_note *note(world *w, uint64_t now_ms, const char *text)
{
    if (w->note_count == WORLD_FEED) {
        memmove(w->notes, w->notes + 1, sizeof w->notes[0] * (WORLD_FEED - 1));
        w->note_count--;
    }
    world_note *n = &w->notes[w->note_count++];
    memset(n, 0, sizeof *n);
    snprintf(n->text, sizeof n->text, "%s", text);
    n->at_ms = now_ms;
    return n;
}

const world_player *world_player_by_id(const world *w, uint16_t id)
{
    for (int i = 0; i < w->player_count; i++)
        if (w->players[i].id == id)
            return &w->players[i];
    return NULL;
}

const world_body *world_body_by_id(const world *w, uint16_t id)
{
    for (int i = 0; i < w->body_count; i++)
        if (w->bodies[i].id == id)
            return &w->bodies[i];
    return NULL;
}

int world_alive_count(const world *w)
{
    int n = 0;
    for (int i = 0; i < w->body_count; i++)
        n += w->bodies[i].alive;
    return n;
}

static void apply_board(world *w, const msg_tiles *m)
{
    sim_arena_reset(&w->arena);
    int bit = 0;
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++, bit++)
            if (bit / 8 < m->gone_bytes && (m->gone[bit / 8] & (0x80 >> (bit % 8))))
                w->arena.tiles[layer][i].state = TILE_GONE;
    for (int k = 0; k < m->touched_count; k++) {
        const touched_tile *t = &m->touched[k];
        int i = sim_tile_index(t->t.q, t->t.r);
        if (i < 0 || t->t.layer >= SIM_LAYERS)
            continue;
        w->arena.tiles[t->t.layer][i].state = TILE_TOUCHED;
        w->arena.tiles[t->t.layer][i].touch_tick = t->touch_tick;
    }
    w->have_board = 1;
}

void world_apply_event(world *w, const uint8_t *msg, size_t len, uint64_t now_ms)
{
    rbuf r, p;
    uint8_t type;
    rb_init(&r, msg, len);
    if (!hf_next_msg(&r, &type, &p))
        return;
    switch (type) {
    case MSG_WELCOME: {
        msg_welcome m;
        if (!hf_get_welcome(&p, &m))
            break;
        /* A new session: what we knew may be stale (after a resync, most of it
         * is), so start over; TILES and ROOM follow right behind. */
        world_note keep[WORLD_FEED];
        int kept = w->note_count;
        memcpy(keep, w->notes, sizeof keep);
        world_init(w);
        memcpy(w->notes, keep, sizeof keep);
        w->note_count = kept;
        w->welcomed = 1;
        w->me = m.player_id;
        w->tick = m.tick;
        break;
    }
    case MSG_ROOM: {
        msg_room m;
        if (!hf_get_room(&p, &m))
            break;
        if (m.phase == PHASE_PLAYING && w->phase != PHASE_PLAYING)
            w->body_count = 0;  /* last round's bodies are gone */
        w->phase = m.phase;
        w->phase_end_tick = m.phase_end_tick;
        w->player_count = m.count;
        for (int i = 0; i < m.count; i++) {
            world_player *wp = &w->players[i];
            wp->id = m.p[i].id;
            wp->color = m.p[i].color;
            wp->flags = m.p[i].flags;
            wp->place = m.p[i].place;
            memcpy(wp->name, m.p[i].name, sizeof wp->name);
        }
        break;
    }
    case MSG_TILES: {
        static msg_tiles m;  /* large; this function only runs on the main thread */
        if (hf_get_tiles(&p, &m))
            apply_board(w, &m);
        break;
    }
    case MSG_TILE_EVENTS: {
        msg_tile_events m;
        if (!hf_get_tile_events(&p, &m))
            break;
        for (int k = 0; k < m.count; k++)
            sim_touch(&w->arena, m.t[k].layer, m.t[k].q, m.t[k].r, m.tick, NULL, NULL);
        break;
    }
    case MSG_ELIMINATED: {
        msg_eliminated m;
        if (!hf_get_eliminated(&p, &m))
            break;
        const world_player *wp = world_player_by_id(w, m.player_id);
        char text[64];
        if (m.player_id == w->me)
            snprintf(text, sizeof text, "You fell into the slime: #%d", m.place);
        else
            snprintf(text, sizeof text, "%s fell into the slime (#%d)", wp ? wp->name : "someone", m.place);
        world_note *n = note(w, now_ms, text);
        n->knockout = 1;
        n->player = m.player_id;
        n->place = m.place;
        n->tick = m.tick;
        break;
    }
    case MSG_ERROR: {
        msg_error m;
        char text[64];
        if (!hf_get_error(&p, &m))
            break;
        snprintf(text, sizeof text, "Server: %s", m.text);
        note(w, now_ms, text);
        break;
    }
    default:
        break;
    }
}

int world_decode_snapshot(const uint8_t *msg, size_t len, world_snapshot *out)
{
    rbuf r, p;
    uint8_t type;
    static msg_snapshot m;  /* main thread only */
    rb_init(&r, msg, len);
    if (!hf_next_msg(&r, &type, &p) || type != MSG_SNAPSHOT || !hf_get_snapshot(&p, &m))
        return 0;
    out->tick = m.tick;
    out->ack_input = m.ack_input;
    out->count = m.count;
    for (int i = 0; i < m.count; i++) {
        const snap_player *s = &m.p[i];
        world_body *b = &out->bodies[i];
        b->id = s->id;
        b->x = s->x / HF_POS_SCALE;
        b->y = s->y / HF_POS_SCALE;
        b->z = s->z / HF_POS_SCALE;
        b->vx = s->vx / HF_POS_SCALE;
        b->vy = s->vy / HF_POS_SCALE;
        b->vz = s->vz / HF_POS_SCALE;
        b->facing = (float)s->facing / 65536.0f * 6.2831853f;
        b->grounded = (s->flags & SP_GROUNDED) != 0;
        b->alive = (s->flags & SP_ALIVE) != 0;
        b->layer = s->layer;
    }
    return 1;
}

void world_apply_snapshot(world *w, const world_snapshot *s, uint64_t arrived_ms)
{
    if (w->have_snapshot && (int32_t)(s->tick - w->tick) <= 0) {
        w->snapshots_late++;  /* overtaken by a newer one on the way */
        return;
    }
    w->have_snapshot = 1;
    w->tick = s->tick;
    w->snapshot_at_ms = arrived_ms;
    w->ack_input = s->ack_input;
    w->body_count = s->count;
    memcpy(w->bodies, s->bodies, sizeof w->bodies[0] * (size_t)s->count);
    sim_arena_tick(&w->arena, s->tick);  /* touched tiles whose time is up fall now */
    w->snapshots_used++;
}
