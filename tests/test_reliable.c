#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "reliable.h"

/* Two endpoints talk over a simulated link that drops 30% of packets,
 * delays the rest by 20-120 ms (so they also arrive out of order) and
 * duplicates some. Every reliable message must arrive exactly once and in
 * order, in both directions. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define LOSS 0.30
#define DUPLICATE 0.05
#define TICK_MS 33
#define TO_B 1500  /* reliable messages A sends to B */
#define TO_A 400

typedef struct {
    int used, to_b;
    uint64_t due;
    size_t len;
    uint8_t data[HF_MAX_PACKET];
} flight;

typedef struct {
    uint32_t expect;
    int reliable, unreliable, out_of_order;
} sink;

static flight air[8192];
static size_t air_top;  /* slots at or past this index are free */
static uint64_t rng = 88172645463325252ull;

static double rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

static void launch(const uint8_t *data, size_t len, int to_b, uint64_t now)
{
    int copies = rnd() < DUPLICATE ? 2 : 1;
    for (int c = 0; c < copies; c++) {
        if (rnd() < LOSS)
            continue;
        for (size_t i = 0; i < sizeof air / sizeof air[0]; i++) {
            if (air[i].used)
                continue;
            air[i] = (flight){.used = 1, .to_b = to_b, .due = now + 20 + (uint64_t)(rnd() * 100), .len = len};
            memcpy(air[i].data, data, len);
            if (i + 1 > air_top)
                air_top = i + 1;
            break;
        }
    }
}

static void deliver(void *ctx, const uint8_t *msg, size_t len)
{
    sink *s = ctx;
    rbuf r, payload;
    uint8_t type;
    rb_init(&r, msg, len);
    CHECK(hf_next_msg(&r, &type, &payload));
    if (hf_msg_is_reliable(type)) {
        uint32_t index = rb_u32(&payload);
        if (index != s->expect)
            s->out_of_order++;
        s->expect = index + 1;
        s->reliable++;
    } else {
        s->unreliable++;
    }
}

/* A reliable test message: ROOM type, a counter, and some filler so sizes vary. */
static size_t make_reliable(uint8_t *buf, uint32_t index)
{
    wbuf w;
    wb_init(&w, buf, HF_MAX_MSG);
    wb_u8(&w, MSG_ROOM);
    size_t filler = (index * 37) % 300;
    wb_u16(&w, (uint16_t)(4 + filler));
    wb_u32(&w, index);
    for (size_t i = 0; i < filler; i++)
        wb_u8(&w, (uint8_t)i);
    return w.len;
}

static size_t make_unreliable(uint8_t *buf)
{
    wbuf w;
    wb_init(&w, buf, HF_MAX_MSG);
    wb_u8(&w, MSG_SNAPSHOT);
    wb_u16(&w, 200);
    for (int i = 0; i < 200; i++)
        wb_u8(&w, 0);
    return w.len;
}

static void send_packets(rel_endpoint *e, int to_b, uint64_t now)
{
    for (int k = 0; k < 4; k++) {
        if (k > 0 && !rel_wants_send(e, now))
            break;
        uint8_t pkt[HF_MAX_PACKET];
        wbuf w;
        wb_init(&w, pkt, sizeof pkt);
        rel_write_packet(e, now, &w);
        CHECK(!w.overflow);
        launch(pkt, w.len, to_b, now);
    }
    rel_drop_unreliable(e);
}

int main(void)
{
    static rel_endpoint a, b;
    sink at_a = {0}, at_b = {0};
    rel_init(&a);
    rel_init(&b);
    uint32_t queued_to_b = 0, queued_to_a = 0;
    int unreliable_sent = 0;
    uint8_t msg[HF_MAX_MSG];

    uint64_t now = 0;
    for (; now < 600000; now++) {
        if (now % TICK_MS == 0) {
            while (queued_to_b < TO_B && rel_queue_reliable(&a, msg, make_reliable(msg, queued_to_b)) == 0)
                queued_to_b++;
            while (queued_to_a < TO_A && rel_queue_reliable(&b, msg, make_reliable(msg, queued_to_a)) == 0)
                queued_to_a++;
            if (rel_queue_unreliable(&a, msg, make_unreliable(msg)) == 0)
                unreliable_sent++;
            send_packets(&a, 1, now);
            send_packets(&b, 0, now);
        }
        for (size_t i = 0; i < air_top; i++) {
            flight *f = &air[i];
            if (!f->used || f->due > now)
                continue;
            rbuf r;
            rb_init(&r, f->data, f->len);
            if (f->to_b)
                rel_read_packet(&b, now, &r, deliver, &at_b);
            else
                rel_read_packet(&a, now, &r, deliver, &at_a);
            f->used = 0;
        }
        while (air_top > 0 && !air[air_top - 1].used)
            air_top--;
        if (at_b.reliable == TO_B && at_a.reliable == TO_A)
            break;
    }

    printf("reliable: %d/%d and %d/%d delivered in %.1f s simulated, %llu resends, srtt %.0f ms, rto %.0f ms\n",
           at_b.reliable, TO_B, at_a.reliable, TO_A, (double)now / 1000.0, (unsigned long long)(a.resends + b.resends),
           a.srtt_ms, a.rto_ms);
    printf("          unreliable: %d of %d arrived (at most once each, losses allowed)\n", at_b.unreliable,
           unreliable_sent);
    CHECK(at_b.reliable == TO_B && at_a.reliable == TO_A);
    CHECK(at_b.out_of_order == 0 && at_a.out_of_order == 0);
    CHECK(at_b.unreliable <= unreliable_sent);
    CHECK(a.rtt_valid && a.srtt_ms > 20 && a.srtt_ms < 400);
    printf("reliable: all checks passed\n");
    return 0;
}
