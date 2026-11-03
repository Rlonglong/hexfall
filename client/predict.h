#ifndef HF_PREDICT_H
#define HF_PREDICT_H

#include <stdint.h>

#include "interp.h"
#include "protocol.h"
#include "sim.h"
#include "world.h"

/* Client-side prediction: our own player moves the moment a key is pressed.
 *
 * The problem. Since step 2 every player is drawn as the server last
 * described them, a little in the past (interp.h). For other players
 * nobody can tell. For our own player everybody can: a key press travels
 * to the server, waits its turn there, comes back in a snapshot and then
 * waits in the jitter buffer, so the bean starts to move RTT + queue +
 * delay after the press. From about 100 ms on, it feels like steering a
 * boat.
 *
 * The idea. The client has the same physics as the server (sim.c) and
 * knows its own inputs, so it can work out where the server is going to
 * put us before the server does: each input moves our bean as it is sent.
 * The server stays in charge. Every snapshot says where it really put us
 * and with which of our inputs (ack_input): the client starts again from
 * there and replays the inputs the server has not used yet. That is
 * reconciliation. When both ran the same physics on the same inputs, the
 * replay lands exactly where the prediction already was and nothing shows.
 * When they did not (another player bumped us, or an input came too late
 * and the server used the old stick once more), the bean is pulled to the
 * right place over PREDICT_SMOOTH_MS instead of jumping there; how often
 * and how far is counted.
 *
 * Timing. The server uses one input per tick, in order (input_queue.h). If
 * the newest snapshot is of tick T and has used our inputs up to A, input
 * A + k will be used at tick T + k, so after our newest input L we are at
 * tick T + (L - A). That is our "present": L - A ticks ahead of the newest
 * snapshot, and further still ahead of the moment the others are drawn at.
 * Between two input ticks the bean is drawn part of the way to the next
 * step, worked out with the keys as they are right now, so a press shows
 * on the very next frame (predict_frame).
 *
 * The jump cooldown is not in a snapshot. It comes from our own
 * prediction for input A, and the replay carries it on from there.
 *
 * Bumping into others. The server pushes players apart when they meet
 * (sim_separate). For a bump to happen on screen the moment it happens,
 * and for the prediction to agree with the server about it, the others
 * have to be predicted too. So from the newest snapshot the client plays
 * the server's ticks for everybody up to our present: our inputs for us;
 * for each of the others, the stick held as they were moving (the
 * screen's default guess, interp.h; in the air nothing they press
 * matters); then everybody pushed apart, in the server's order.
 *
 * That gives the others two moments: the playout past (interp.h), where
 * every position is real, and our present, which is partly a guess. Far
 * from our bean the past is drawn, as in step 2. Closer than PREDICT_NEAR
 * the present is drawn, so what our bean bumps into is where it is drawn;
 * between PREDICT_NEAR and PREDICT_FAR, a mix (predict_scene). When a
 * snapshot shows a guess was wrong, the change is smoothed out like our
 * own corrections.
 *
 * Tiles. A tile cannot be right for two moments either. Drawn at the
 * playout moment, a tile would light up `delay` ms after we step on it,
 * and could still be drawn while our bean falls through it. So the tiles
 * our bean has been on, and the tiles near it, are drawn at our present
 * moment (predict_tile): lit as soon as we step on them, before the server
 * says so, and falling when the server says they fall, never on our word
 * alone. If the server has not agreed within PREDICT_UNCONFIRMED ticks
 * that we touched one, it goes back to how the server sees it. Tiles far
 * away are drawn at the playout moment, with the players on them.
 *
 * Main thread only; no sockets, no raylib. tests/test_predict.c runs it
 * against a server and network of its own making. */

#define PREDICT_HISTORY 64       /* inputs remembered, about 2 s */
#define PREDICT_MAX_AHEAD 30     /* more unused inputs than this: the ack is from before this round */
#define PREDICT_SMOOTH_MS 100.0  /* a correction fades out with this time constant */
#define PREDICT_SNAP 4.0f        /* a correction bigger than this is not smoothed */
#define PREDICT_UNCONFIRMED 20   /* ticks a tile we stepped on waits for the server to agree */
#define PREDICT_NOISE 0.02       /* corrections smaller than this are rounding (positions travel in 1/100 units) */
#define PREDICT_NEAR 6.0f        /* others this close to our bean are drawn at our moment... */
#define PREDICT_FAR 10.0f        /* ...and from this far on at the playout moment */

typedef struct {
    int used;
    uint16_t seq;
    sim_input in;
    sim_body after;  /* our body once the server has used this input, as predicted */
} predict_step;

typedef struct {
    uint16_t id;
    sim_body body;              /* at our present, as far as we can tell */
    float off_x, off_y, off_z;  /* a change of guess still fading out */
} predict_other;

typedef struct {
    int enabled;  /* off: our bean is drawn like everybody else's (step 2) */
    int active;   /* predicting now: in a round, alive, and with an ack from this round */
    int phase;    /* the room's phase when last looked at */

    int have_seq;
    uint16_t seq;   /* newest input sent */
    uint32_t tick;  /* the present: the server tick `body` belongs to */
    sim_body body;  /* after input `seq` */
    predict_step hist[PREDICT_HISTORY];  /* input s in hist[s % PREDICT_HISTORY] */

    float off_x, off_y, off_z;  /* a correction still fading out */

    /* Everybody else, in the server's order (the snapshot's); our own place
     * in that order is me_slot. */
    predict_other others[HF_MAX_PLAYERS];
    int other_count, me_slot;

    /* Tiles our bean has been on (at the height of the floor): the tick it
     * first was, 0 = never; and whether it stood there, which touches it. */
    uint32_t mine[SIM_LAYERS][SIM_TILES];
    uint8_t stood[SIM_LAYERS][SIM_TILES];

    /* What to draw for us, refreshed by predict_frame; only if shown_valid
     * (otherwise our bean comes from the interpolation like the others). */
    int shown_valid;
    world_body shown;
    world_body shown_others[HF_MAX_PLAYERS];  /* the others at our moment, in the order of `others` */
    double shown_tick;                        /* the present, fractional */

    /* Since enabled. */
    int ahead;  /* inputs the server had not used yet, at the newest snapshot */
    uint64_t snapshots, corrections;
    double correction_sum, correction_max;
} predict;

void predict_init(predict *pr);
void predict_set_enabled(predict *pr, int on);

/* A newer snapshot has reached the world: the server's word on where we are. */
void predict_snapshot(predict *pr, const world *w);

/* An input, just sent. */
void predict_input(predict *pr, const world *w, const input_sample *s);

/* Once per frame. `keys` is the input as it would be sent now, `frac` how
 * far it is (0..1) from the last input tick to the next. */
void predict_frame(predict *pr, const world *w, const input_sample *keys, double frac, double dt_ms);

/* The scene to draw: `past` (the interpolation's view) with our own bean
 * where we predict it, and the others near it moved to our moment. */
void predict_scene(const predict *pr, const world *w, const interp_view *past, interp_view *out);

/* How to draw a tile: judge `*t` at moment `*at` (with interp_tile_state). */
void predict_tile(const predict *pr, const world *w, int layer, int index, double playout_tick, sim_tile *t,
                  double *at);

#endif
