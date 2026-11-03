#ifndef HF_MSG_QUEUE_H
#define HF_MSG_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Channels between the main thread and the network thread.
 *
 * Each channel has one producer thread and one consumer thread, and one mutex
 * that guards every field of it. Items are copied in and copied out under the
 * lock, so neither thread ever holds a pointer into the other's data. No lock
 * is held while calling out, and no code holds two channel locks at once, so
 * the channels cannot deadlock. */

typedef struct {
    uint64_t at_ms;  /* when the network thread received it (0 for outgoing items) */
    uint16_t len;
    uint8_t data[HF_MAX_MSG];
} mq_item;

/* A FIFO that never drops: when it is full, the push is refused and the
 * producer decides what that means. Used for messages that must not be lost. */
typedef struct {
    pthread_mutex_t lock;
    mq_item *items;
    size_t cap, head, count;
    uint64_t pushed, refused;
} mq_fifo;

int mq_fifo_init(mq_fifo *q, size_t cap);
void mq_fifo_destroy(mq_fifo *q);
int mq_fifo_push(mq_fifo *q, const uint8_t *msg, size_t len, uint64_t at_ms);  /* 0, or -1 if full */
size_t mq_fifo_pop(mq_fifo *q, mq_item *out, size_t max);                      /* oldest first */
size_t mq_fifo_count(mq_fifo *q);
uint64_t mq_fifo_refused(mq_fifo *q);

/* The newest MQ_RING items; a push overwrites the oldest. Used for snapshots,
 * where only recent ones matter. The reader keeps its own cursor. */
#define MQ_RING 32

typedef struct {
    pthread_mutex_t lock;
    mq_item items[MQ_RING];
    uint64_t written;  /* items pushed so far; item k is in items[k % MQ_RING] */
} mq_ring;

int mq_ring_init(mq_ring *r);
void mq_ring_destroy(mq_ring *r);
void mq_ring_push(mq_ring *r, const uint8_t *msg, size_t len, uint64_t at_ms);

/* Copies the items after *cursor, oldest first, and advances *cursor. If the
 * reader fell more than MQ_RING behind, the overwritten items are added to
 * *skipped (which may be NULL). */
size_t mq_ring_read(mq_ring *r, uint64_t *cursor, mq_item *out, size_t max, uint64_t *skipped);

#endif
