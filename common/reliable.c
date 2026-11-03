#include "reliable.h"

#include <math.h>
#include <string.h>

#define RTO_INITIAL_MS 200.0
#define RTO_MIN_MS 40.0
#define RTO_MAX_MS 1000.0
#define FLAG_HAS_ACK 1

/* Sequence numbers wrap at 65536; a - b as a signed 16-bit value says which
 * one is newer as long as they are less than half the range apart. */
static int16_t seq_diff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

void rel_init(rel_endpoint *e)
{
    memset(e, 0, sizeof *e);
    e->rto_ms = RTO_INITIAL_MS;
}

int rel_queue_reliable(rel_endpoint *e, const uint8_t *msg, size_t len)
{
    if (len < 3 || len > HF_MAX_MSG)
        return -1;
    if ((uint16_t)(e->next_id - e->oldest_id) >= REL_WINDOW)
        return -1;
    rel_out *o = &e->out[e->next_id % REL_WINDOW];
    o->used = 1;
    o->acked = 0;
    o->ever_sent = 0;
    o->id = e->next_id;
    o->len = (uint16_t)len;
    o->last_sent_ms = 0;
    memcpy(o->data, msg, len);
    e->next_id++;
    return 0;
}

int rel_queue_unreliable(rel_endpoint *e, const uint8_t *msg, size_t len)
{
    if (len < 3 || len > sizeof e->unrel - e->unrel_len)
        return -1;
    memcpy(e->unrel + e->unrel_len, msg, len);
    e->unrel_len += len;
    return 0;
}

void rel_drop_unreliable(rel_endpoint *e) { e->unrel_len = 0; }

size_t rel_in_flight(const rel_endpoint *e) { return (uint16_t)(e->next_id - e->oldest_id); }

static int is_due(const rel_endpoint *e, const rel_out *o, uint64_t now_ms)
{
    if (!o->used || o->acked)
        return 0;
    return !o->ever_sent || (double)(now_ms - o->last_sent_ms) >= e->rto_ms;
}

int rel_wants_send(const rel_endpoint *e, uint64_t now_ms)
{
    if (e->unrel_len > 0)
        return 1;
    for (uint16_t id = e->oldest_id; id != e->next_id; id++)
        if (is_due(e, &e->out[id % REL_WINDOW], now_ms))
            return 1;
    return 0;
}

void rel_write_packet(rel_endpoint *e, uint64_t now_ms, wbuf *w)
{
    uint16_t seq = e->next_seq++;
    wb_u16(w, seq);
    wb_u8(w, e->have_remote ? FLAG_HAS_ACK : 0);
    wb_u16(w, e->remote_seq);
    wb_u32(w, e->remote_bits);
    size_t count_at = w->len;
    wb_u8(w, 0);
    if (w->overflow)
        return;

    rel_sent *s = &e->sent[seq % REL_PACKET_HISTORY];
    s->used = 1;
    s->acked = 0;
    s->seq = seq;
    s->sent_ms = now_ms;
    s->n_ids = 0;

    /* Oldest first, so a backlog drains in order; skip what does not fit. */
    for (uint16_t id = e->oldest_id; id != e->next_id && s->n_ids < REL_IDS_PER_PACKET; id++) {
        rel_out *o = &e->out[id % REL_WINDOW];
        if (!is_due(e, o, now_ms) || w->cap - w->len < (size_t)o->len + 2)
            continue;
        wb_u16(w, o->id);
        wb_bytes(w, o->data, o->len);
        if (o->ever_sent)
            e->resends++;
        o->ever_sent = 1;
        o->last_sent_ms = now_ms;
        s->ids[s->n_ids++] = o->id;
    }
    w->buf[count_at] = s->n_ids;

    /* Unreliable messages in order while they fit; the caller may write
     * another packet for the rest, then drops whatever is left. */
    size_t taken = 0;
    while (taken + 3 <= e->unrel_len) {
        size_t mlen = 3 + (((size_t)e->unrel[taken + 1] << 8) | e->unrel[taken + 2]);
        if (w->cap - w->len < mlen)
            break;
        wb_bytes(w, e->unrel + taken, mlen);
        taken += mlen;
    }
    memmove(e->unrel, e->unrel + taken, e->unrel_len - taken);
    e->unrel_len -= taken;
    e->packets_sent++;
}

static void on_packet_acked(rel_endpoint *e, rel_sent *s, uint64_t now_ms, int sample_rtt)
{
    s->acked = 1;
    if (sample_rtt) {
        /* Every packet has its own send time, even when it carries resent
         * messages, so the sample is never ambiguous (Karn's problem). */
        double m = (double)(now_ms - s->sent_ms);
        if (!e->rtt_valid) {
            e->srtt_ms = m;
            e->rttvar_ms = m / 2;
            e->rtt_valid = 1;
        } else {
            double err = m - e->srtt_ms;
            e->srtt_ms += err / 8;
            e->rttvar_ms += (fabs(err) - e->rttvar_ms) / 4;
        }
        e->rto_ms = e->srtt_ms + 4 * e->rttvar_ms;
        if (e->rto_ms < RTO_MIN_MS)
            e->rto_ms = RTO_MIN_MS;
        if (e->rto_ms > RTO_MAX_MS)
            e->rto_ms = RTO_MAX_MS;
    }
    for (int i = 0; i < s->n_ids; i++) {
        rel_out *o = &e->out[s->ids[i] % REL_WINDOW];
        if (o->used && o->id == s->ids[i])
            o->acked = 1;
    }
    while (e->oldest_id != e->next_id) {
        rel_out *o = &e->out[e->oldest_id % REL_WINDOW];
        if (!o->used || !o->acked)
            break;
        o->used = 0;
        e->oldest_id++;
    }
}

static void process_acks(rel_endpoint *e, uint16_t ack, uint32_t bits, uint64_t now_ms)
{
    for (int i = 0; i <= 32; i++) {
        if (i > 0 && !(bits & (1u << (i - 1))))
            continue;
        uint16_t seq = (uint16_t)(ack - i);
        rel_sent *s = &e->sent[seq % REL_PACKET_HISTORY];
        if (s->used && s->seq == seq && !s->acked)
            on_packet_acked(e, s, now_ms, i == 0);
    }
}

/* Records an incoming sequence number. 0 for a duplicate or one too old to track. */
static int note_received(rel_endpoint *e, uint16_t seq)
{
    if (!e->have_remote) {
        e->have_remote = 1;
        e->remote_seq = seq;
        e->remote_bits = 0;
        return 1;
    }
    int d = seq_diff(seq, e->remote_seq);
    if (d > 0) {
        uint32_t bits = d < 32 ? e->remote_bits << d : 0;
        if (d <= 32)
            bits |= 1u << (d - 1);
        e->remote_bits = bits;
        e->remote_seq = seq;
        return 1;
    }
    if (d == 0 || -d > 32)
        return 0;
    uint32_t bit = 1u << (-d - 1);
    if (e->remote_bits & bit)
        return 0;
    e->remote_bits |= bit;
    return 1;
}

int rel_read_packet(rel_endpoint *e, uint64_t now_ms, rbuf *r, rel_deliver_fn fn, void *ctx)
{
    uint16_t seq = rb_u16(r);
    uint8_t flags = rb_u8(r);
    uint16_t ack = rb_u16(r);
    uint32_t bits = rb_u32(r);
    if (r->error) {
        e->packets_dropped++;
        return 0;
    }
    if (flags & FLAG_HAS_ACK)
        process_acks(e, ack, bits, now_ms);
    if (!note_received(e, seq)) {
        e->packets_dropped++;
        return 0;
    }
    e->packets_received++;

    uint8_t n = rb_u8(r);
    for (int i = 0; i < n; i++) {
        uint16_t id = rb_u16(r);
        size_t start = r->pos;
        uint8_t type;
        rbuf payload;
        if (!hf_next_msg(r, &type, &payload))
            break;
        size_t len = r->pos - start;
        int ahead = seq_diff(id, e->expect_id);
        if (ahead < 0 || ahead >= REL_WINDOW || len > HF_MAX_MSG)
            continue;  /* already delivered, or outside the window */
        rel_in *in = &e->in[id % REL_WINDOW];
        if (in->used)
            continue;  /* a resend of something we already hold */
        in->used = 1;
        in->id = id;
        in->len = (uint16_t)len;
        memcpy(in->data, r->buf + start, len);
    }
    for (;;) {
        rel_in *in = &e->in[e->expect_id % REL_WINDOW];
        if (!in->used || in->id != e->expect_id)
            break;
        in->used = 0;
        e->expect_id++;
        fn(ctx, in->data, in->len);
    }
    while (rb_left(r) > 0) {
        size_t start = r->pos;
        uint8_t type;
        rbuf payload;
        if (!hf_next_msg(r, &type, &payload))
            break;
        fn(ctx, r->buf + start, r->pos - start);
    }
    return 1;
}
