#ifndef HF_INPUT_QUEUE_H
#define HF_INPUT_QUEUE_H

#include <stdint.h>

#include "protocol.h"
#include "sim.h"

/* One player's inputs on the server, and the rule for which one is
 * simulated at each tick. It is in common/ because the client predicts its
 * own player (client/predict.c) and has to turn an input into motion
 * exactly as the server does; tests/test_predict.c runs this same queue as
 * its server.
 *
 * Every INPUT message repeats the newest HF_INPUT_REDUNDANCY samples, so a
 * lost packet costs nothing; repeats and out-of-date samples are dropped
 * when they arrive. Each tick takes the oldest sample waiting:
 *
 *   - none waiting (late or lost): the stick stays where it was, a button
 *     press (jump, dive, shove) is not repeated, and `applied` stays as it
 *     was;
 *   - more than INPUTQ_TARGET + 2 waiting (a burst after a stall): the
 *     oldest are dropped down to INPUTQ_TARGET, keeping any button press,
 *     since a longer queue only adds latency.
 *
 * `applied` goes back to the client in every snapshot as ack_input: "your
 * player, as shown here, has been simulated with your inputs up to this
 * one". */

#define INPUTQ_CAP 16
#define INPUTQ_TARGET 2

typedef struct {
    input_sample q[INPUTQ_CAP];  /* oldest first */
    int len;
    int have_seq;
    uint16_t newest;   /* newest seq received */
    uint16_t applied;  /* seq simulated at the last tick */
    input_sample current;
} inputq;

/* Nothing waiting and the stick let go, as at the start of a round or when
 * the connection is lost; the sequence numbers carry on. */
void inputq_clear(inputq *iq);
void inputq_add(inputq *iq, const msg_input *m);
sim_input inputq_next(inputq *iq);

/* A sample as the simulation uses it. */
sim_input inputq_to_sim(const input_sample *s);

#endif
