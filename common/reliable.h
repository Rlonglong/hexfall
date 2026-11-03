#ifndef HF_RELIABLE_H
#define HF_RELIABLE_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "wire.h"

/* Reliability on top of UDP (the idea of UNP chapter 22, done per packet).
 *
 * Every packet carries its own sequence number, the newest sequence number
 * received from the peer, and a 32-bit history of the packets before that.
 * So one packet acknowledges up to 33 of the peer's packets, and losing an
 * ack costs nothing as long as a later packet gets through.
 *
 * Reliable messages are copied into packets and sent again, after a timeout
 * estimated the way TCP does it (SRTT + 4 * RTTVAR), until a packet that
 * carried them is acknowledged. The receiver hands them over strictly in
 * order. Unreliable messages are sent once; a lost snapshot is simply
 * replaced by the next one, so it never holds anything up.
 *
 * A phone on WebSocket skips this layer: TCP already orders and delivers.
 *
 * Packet body, after the UDP envelope (protocol.h):
 *   u16 seq | u8 flags | u16 ack | u32 ack_bits | u8 n_reliable
 *   n_reliable x (u16 message id, message)  |  unreliable messages to the end */

#define REL_WINDOW 128          /* reliable messages in flight per direction */
#define REL_PACKET_HISTORY 256  /* sent packets remembered so acks can be matched */
#define REL_IDS_PER_PACKET 32
#define REL_HEADER_BYTES 10

typedef void (*rel_deliver_fn)(void *ctx, const uint8_t *msg, size_t len);

typedef struct {
    int used, acked, ever_sent;
    uint16_t id, len;
    uint64_t last_sent_ms;
    uint8_t data[HF_MAX_MSG];
} rel_out;

typedef struct {
    int used;
    uint16_t id, len;
    uint8_t data[HF_MAX_MSG];
} rel_in;

typedef struct {
    int used, acked;
    uint16_t seq;
    uint64_t sent_ms;
    uint8_t n_ids;
    uint16_t ids[REL_IDS_PER_PACKET];
} rel_sent;

typedef struct {
    uint16_t next_seq;     /* sequence number of our next packet */
    uint16_t remote_seq;   /* newest packet received from the peer */
    uint32_t remote_bits;  /* bit i set: remote_seq - 1 - i was received */
    int have_remote;
    rel_sent sent[REL_PACKET_HISTORY];

    uint16_t next_id;      /* id the next queued reliable message gets */
    uint16_t oldest_id;    /* oldest reliable message not yet acknowledged */
    rel_out out[REL_WINDOW];

    uint16_t expect_id;    /* next reliable id to hand to the application */
    rel_in in[REL_WINDOW];

    uint8_t unrel[HF_MAX_PACKET];
    size_t unrel_len;

    double srtt_ms, rttvar_ms, rto_ms;
    int rtt_valid;

    uint64_t packets_sent, packets_received, packets_dropped, resends;
} rel_endpoint;

void rel_init(rel_endpoint *e);

/* Queue a complete message. Reliable: -1 when the window is full, which means
 * the peer has stopped acknowledging. Unreliable: -1 when this tick's packets
 * are already full; the message is then just not sent. */
int rel_queue_reliable(rel_endpoint *e, const uint8_t *msg, size_t len);
int rel_queue_unreliable(rel_endpoint *e, const uint8_t *msg, size_t len);
void rel_drop_unreliable(rel_endpoint *e);

/* 1 if a packet written now would carry something besides acks. */
int rel_wants_send(const rel_endpoint *e, uint64_t now_ms);

/* Appends a packet body to w, which already holds the envelope. */
void rel_write_packet(rel_endpoint *e, uint64_t now_ms, wbuf *w);

/* Reads a packet body and calls fn once per delivered message, reliable ones
 * in order. Returns 0 for a duplicate, stale or malformed packet. */
int rel_read_packet(rel_endpoint *e, uint64_t now_ms, rbuf *r, rel_deliver_fn fn, void *ctx);

size_t rel_in_flight(const rel_endpoint *e);

#endif
