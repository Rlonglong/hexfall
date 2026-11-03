#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "clock_sync.h"

/* Simulated PING/PONG exchanges. Each direction takes 20 ms plus queueing
 * that is usually small but sometimes long, and the uplink is busier than
 * the downlink, so many samples are lopsided and their average is biased.
 * The estimate must stay within a few milliseconds of the true offset
 * anyway, across a 32-bit wrap-around of both clocks. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static double rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

/* Queueing delay: mostly 0-3 ms, but with probability p up to max ms. */
static uint32_t queueing(double p, double max) { return (uint32_t)(rnd() < p ? rnd() * max : rnd() * 3); }

static int32_t error_of(const clock_sync *c, uint32_t true_offset) { return (int32_t)(c->offset - true_offset); }

int main(void)
{
    clock_sync c;
    cs_init(&c);
    uint32_t local = 0xFFFFF000u;      /* the local clock wraps during the test */
    uint32_t true_offset = 0x7FFFFF00u; /* and so does the server's */
    double naive_sum = 0;
    int samples = 0, worst = 0;

    for (int i = 0; i < 200; i++, local += 500) {
        uint32_t up = 20 + queueing(0.4, 150), down = 20 + queueing(0.1, 60);
        uint32_t t0 = local, ts = local + up + true_offset, t1 = local + up + down;
        CHECK(cs_add(&c, t0, ts, t1));
        naive_sum += (int32_t)(ts - (t0 + (t1 - t0) / 2) - true_offset);
        samples++;
        if (i >= 8) {
            int err = abs(error_of(&c, true_offset));
            worst = err > worst ? err : worst;
        }
    }
    printf("clock_sync: worst error after settling %d ms (averaging every sample would be %.1f ms off)\n", worst,
           fabs(naive_sum / samples));
    CHECK(worst <= 3);
    CHECK(c.rtt_min_ms >= 40 && c.rtt_min_ms <= 46);

    /* Once settled, a small shift is followed gradually, never in one jump. */
    true_offset += 30;
    uint32_t before = c.offset;
    CHECK(cs_add(&c, local, local + 20 + true_offset, local + 40));
    CHECK((int32_t)(c.offset - before) <= 2);
    for (int i = 0; i < 40; i++, local += 500)
        cs_add(&c, local, local + 20 + true_offset, local + 40);
    CHECK(abs(error_of(&c, true_offset)) <= 1);

    /* A large change (the server restarted) is taken at once. */
    true_offset += 10000;
    for (int i = 0; i < CS_WINDOW; i++, local += 500)
        cs_add(&c, local, local + 20 + true_offset, local + 40);
    CHECK(abs(error_of(&c, true_offset)) <= 1);

    /* A reply that took longer than 5 seconds is ignored. */
    CHECK(!cs_add(&c, local, local + true_offset, local + 6000));

    printf("clock_sync: all checks passed\n");
    return 0;
}
