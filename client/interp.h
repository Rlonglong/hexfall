#ifndef HF_INTERP_H
#define HF_INTERP_H

#include <stdint.h>

#include "sim.h"
#include "world.h"

/* Snapshot interpolation: which moment of the game to draw.
 *
 * The problem. The server simulates 30 ticks a second and sends a snapshot
 * of every player after each one, so on a perfect network a snapshot
 * arrives every 33.3 ms. On a real network they do not: each spends a
 * different time on the way (jitter), some never arrive (loss; snapshots
 * are never resent), and a late one may arrive after a newer one
 * (reordering). Drawing each snapshot the moment it arrives, as step 1 does
 * (INTERP_NONE), shows all of that on screen: players freeze while nothing
 * comes, then jump.
 *
 * The idea. Keep the last INTERP_HISTORY snapshots and draw the game as it
 * was a short time ago, at the "playout" time. Players are placed between
 * the two snapshots on either side of that moment, so they move smoothly
 * even when snapshots arrive in clumps. A snapshot that arrives out of
 * order still fills its place, as long as the playout time has not passed
 * it. This is the jitter buffer of internet voice and video, applied to
 * game state. The price is that everything is drawn `delay` ms behind the
 * newest news. Step 3 takes that price back for our own player by
 * predicting it.
 *
 * Timing. The server sends tick k at T0 + k * P on its own clock, with
 * P = 33.3 ms. A snapshot that arrives at local time a_k therefore spent
 *
 *     transit_k = a_k - k * P
 *
 * on the way, plus an unknown constant: the two clocks are unrelated, and
 * none of this needs clock sync. Only differences matter. The smallest
 * transit in the history is the fastest trip the network currently makes,
 * and
 *
 *     late_k = transit_k - min transit
 *
 * is how much later than that snapshot k came. The moment to draw is
 *
 *     playout = (now - min transit - delay) / P             (in ticks)
 *
 * Drawing any moment between ticks k-1 and k needs snapshot k, so it must
 * be there by the time the playout reaches k-1. Working it out, that holds
 * when late_k <= delay - P. If snapshot k is lost, k+1 has to do instead,
 * which takes one more P.
 *
 * Modes, switchable while playing to compare them:
 *
 *   INTERP_NONE      the newest snapshot as it is (step 1), for comparison
 *   INTERP_FIXED     delay = INTERP_FIXED_MS (3 ticks), whatever the network
 *                    does; a common default in shipped games
 *   INTERP_ADAPTIVE  delay = P * (1 + lost snapshots in a row, at most
 *                    INTERP_MAX_BRIDGE) + late_k at the INTERP_PERCENTILE of
 *                    the history + INTERP_MARGIN_MS. It goes up at once when
 *                    the network gets worse and comes down slowly
 *                    (INTERP_DECAY_MS_PER_S) when it gets better, so that a
 *                    network with occasional bad seconds does not make it
 *                    swing up and down.
 *
 * The playout clock never jumps. It runs with real time, and when the
 * delay or the minimum transit changes it runs up to INTERP_SLEW faster or
 * slower until it is where it should be (unless it is more than
 * INTERP_SNAP_MS off, as at the start).
 *
 * When the snapshot after the playout time has not arrived, the screen has
 * to guess, and those frames are what a bigger delay avoids. It guesses
 * with the game's own physics (sim_body_step, the server's code) from the
 * newest snapshot it has, on the board as it was then. What nobody can
 * know is what each player pressed meanwhile, so the guess picks one of:
 *
 *   GUESS_LET_GO  they let go of the stick (the default). Someone running
 *                 slides to a stop within a quarter of a second; someone
 *                 in the air carries on along the arc, because gravity is
 *                 the same for everybody and in the air the stick only
 *                 steers a little (sim.c: ACCEL_AIR is a third of
 *                 ACCEL_GROUND). They land on, or fall through, the tiles
 *                 that are there.
 *   GUESS_KEEP    they hold the stick as they were moving.
 *   GUESS_FREEZE  they stand exactly where they were, mid-air or not.
 *
 * After INTERP_GUESS_MS the screen stops guessing and waits (the renderer
 * shows a spinner): better to stop than to keep making things up. Each time
 * a snapshot arrives that the screen had to do without, the guess for its
 * moment is compared with what really happened there, so how wrong the
 * guesses are can be measured, and the three compared.
 *
 * Everything on screen is shown at the same moment: tiles light up and
 * fall at the playout tick too (interp_tile_state), and the renderer holds
 * back the knock-out notice until the playout reaches its tick. Otherwise
 * a tile would vanish from under a bean that, on screen, has not reached
 * it yet.
 *
 * Main thread only; no locks, no sockets, no raylib. The time comes in as
 * an argument, so tests/test_interp.c can run a network of its own making. */

#define INTERP_HISTORY 64            /* snapshots kept, about 2 s; also the window for the statistics */
#define INTERP_FIXED_MS 100.0
#define INTERP_PERCENTILE 0.95       /* the share of snapshots the adaptive delay waits for */
#define INTERP_MARGIN_MS 5.0
#define INTERP_MAX_BRIDGE 2          /* lost snapshots in a row the adaptive delay plans for */
#define INTERP_MAX_MS 500.0          /* never draw further behind than this */
#define INTERP_DECAY_MS_PER_S 20.0
#define INTERP_SLEW 0.10
#define INTERP_SNAP_MS 250.0
#define INTERP_GUESS_MS 200.0       /* the longest the screen guesses before it stops and waits */
#define INTERP_TELEPORT 4.0f         /* farther than this per tick is a jump, not a move */

typedef enum { INTERP_NONE, INTERP_FIXED, INTERP_ADAPTIVE, INTERP_MODES } interp_mode;
typedef enum { GUESS_LET_GO, GUESS_KEEP, GUESS_FREEZE, GUESS_MODES } interp_guess;

/* What to draw: the players at one moment of server time. */
typedef struct {
    double tick;  /* that moment, in ticks; fractional between snapshots */
    int guessed;  /* no snapshot after that moment yet: the players are a guess */
    int count;
    world_body bodies[HF_MAX_PLAYERS];
} interp_view;

typedef struct {
    int used;
    uint64_t arrived_ms;
    world_snapshot s;
} interp_slot;

typedef struct {
    interp_mode mode;
    interp_guess guess;
    const sim_arena *board;  /* the world's board, for guessing */

    /* The history: the snapshot of tick t lives in slot[t % INTERP_HISTORY]
     * (a slot still holding an older tick counts as empty). Times are kept
     * in ms since tick `base`, so the doubles stay small. */
    interp_slot slot[INTERP_HISTORY];
    int have;
    uint32_t base, newest;
    uint64_t newest_arrived_ms;

    /* The network, measured over the history at every snapshot. */
    double min_transit_ms;
    double late_ms;     /* late_k at INTERP_PERCENTILE */
    double loss;        /* share of the history's ticks that never came */
    int longest_gap;    /* most snapshots lost in a row */

    /* The playout clock. */
    double delay_ms;    /* the delay being steered to */
    double playout_ms;  /* the moment on screen, in ms since `base` */
    int playing;
    uint64_t last_ms;

    double waiting_ms;  /* how long the screen has been stopped, waiting; 0 if it is not */

    /* Since the mode or the guess was last set. */
    uint64_t frames, frames_guessed, frames_waiting, snapshots, too_late;
    double stutter_ms;  /* moving average of |step of the moment shown - step of real time| per frame */
    double shown_ms;
    int shown_valid;
    double guess_error_sum, guess_error_max;  /* distance from a guess to what really happened */
    uint64_t guess_errors;

    interp_view view;  /* refreshed by interp_frame() */
} interp;

void interp_init(interp *ip, interp_mode mode, const sim_arena *board);
void interp_set_mode(interp *ip, interp_mode mode);    /* keeps the history, restarts the counters */
void interp_set_guess(interp *ip, interp_guess guess);  /* the same */
const char *interp_mode_name(interp_mode mode);
const char *interp_guess_name(interp_guess guess);

/* A snapshot from the network thread, with the time the network thread
 * received it. Any order; duplicates and very old ones are ignored. A tick
 * far from the ones in the history (the server restarted, or a long
 * reconnect) starts the history over. */
void interp_add(interp *ip, const world_snapshot *s, uint64_t arrived_ms);

/* Once per frame: moves the playout clock to now_ms and fills ip->view. */
void interp_frame(interp *ip, uint64_t now_ms);

const world_body *interp_body(const interp_view *v, uint16_t id);
int interp_alive_count(const interp_view *v);

/* A tile as it was at `tick`: the world knows its newest state and when it
 * was touched, and every touched tile falls SIM_FALL_TICKS later. */
int interp_tile_state(const sim_tile *t, double tick);

/* The whole board as it was at the end of `tick`. The server moves the
 * players before it lets tiles fall, so tick k is played on the board at
 * the end of tick k - 1. */
void interp_board_at(sim_arena *out, const sim_arena *board, uint32_t tick);

#endif
