#ifndef HF_CLOCK_SYNC_H
#define HF_CLOCK_SYNC_H

#include <stdint.h>

/* Estimating the server's clock from PING/PONG, the way NTP does it.
 *
 * A PONG echoes the client's send time t0 and adds the server's time ts; it
 * arrives back at t1. If the request and the reply took equally long, the
 * server read its clock at local time t0 + rtt/2, so
 *
 *     offset = ts - (t0 + rtt / 2)        server time = local time + offset
 *
 * Queueing in a busy network or a busy server makes some round trips long
 * and lopsided, and those samples carry the most error. So, as in Cristian's
 * algorithm, the estimate comes from the sample with the smallest round trip
 * among the recent ones, and the clock moves toward it gradually so that game
 * time never jumps once it has settled.
 *
 * Both clocks are 32-bit milliseconds that wrap around, and the two machines'
 * clocks are unrelated, so the offset is kept modulo 2^32: all arithmetic is
 * done in uint32_t, and only differences between nearby values are signed. */

#define CS_WINDOW 16

typedef struct {
    struct {
        uint32_t rtt, offset;
    } s[CS_WINDOW];
    int count, next;
    int valid;
    uint32_t offset;    /* server ms = local ms + offset (mod 2^32) */
    double rtt_ms;      /* newest sample */
    double rtt_min_ms;  /* smallest in the window: the network's floor */
    double jitter_ms;   /* mean absolute deviation of the window's round trips */
} clock_sync;

void cs_init(clock_sync *c);

/* Adds a PING/PONG sample: t0 local send time, ts server time, t1 local
 * receive time. Returns 0 for a sample that cannot be right (a huge RTT). */
int cs_add(clock_sync *c, uint32_t t0, uint32_t ts, uint32_t t1);

uint32_t cs_server_time(const clock_sync *c, uint32_t local_ms);

#endif
