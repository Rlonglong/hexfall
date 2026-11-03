#include "msg_queue.h"

#include <stdlib.h>
#include <string.h>

static void copy_item(mq_item *dst, const mq_item *src)
{
    dst->at_ms = src->at_ms;
    dst->len = src->len;
    memcpy(dst->data, src->data, src->len);
}

static void fill_item(mq_item *it, const uint8_t *msg, size_t len, uint64_t at_ms)
{
    it->at_ms = at_ms;
    it->len = (uint16_t)len;
    memcpy(it->data, msg, len);
}

int mq_fifo_init(mq_fifo *q, size_t cap)
{
    memset(q, 0, sizeof *q);
    if (cap == 0 || !(q->items = calloc(cap, sizeof *q->items)))
        return -1;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->items);
        q->items = NULL;
        return -1;
    }
    q->cap = cap;
    return 0;
}

void mq_fifo_destroy(mq_fifo *q)
{
    if (!q->items)
        return;
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    q->items = NULL;
}

int mq_fifo_push(mq_fifo *q, const uint8_t *msg, size_t len, uint64_t at_ms)
{
    if (len == 0 || len > HF_MAX_MSG)
        return -1;
    pthread_mutex_lock(&q->lock);
    int ok = q->count < q->cap;
    if (ok) {
        fill_item(&q->items[(q->head + q->count) % q->cap], msg, len, at_ms);
        q->count++;
        q->pushed++;
    } else {
        q->refused++;
    }
    pthread_mutex_unlock(&q->lock);
    return ok ? 0 : -1;
}

size_t mq_fifo_pop(mq_fifo *q, mq_item *out, size_t max)
{
    pthread_mutex_lock(&q->lock);
    size_t n = q->count < max ? q->count : max;
    for (size_t i = 0; i < n; i++)
        copy_item(&out[i], &q->items[(q->head + i) % q->cap]);
    q->head = (q->head + n) % q->cap;
    q->count -= n;
    pthread_mutex_unlock(&q->lock);
    return n;
}

size_t mq_fifo_count(mq_fifo *q)
{
    pthread_mutex_lock(&q->lock);
    size_t n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

uint64_t mq_fifo_refused(mq_fifo *q)
{
    pthread_mutex_lock(&q->lock);
    uint64_t n = q->refused;
    pthread_mutex_unlock(&q->lock);
    return n;
}

int mq_ring_init(mq_ring *r)
{
    memset(r, 0, sizeof *r);
    return pthread_mutex_init(&r->lock, NULL) == 0 ? 0 : -1;
}

void mq_ring_destroy(mq_ring *r) { pthread_mutex_destroy(&r->lock); }

void mq_ring_push(mq_ring *r, const uint8_t *msg, size_t len, uint64_t at_ms)
{
    if (len == 0 || len > HF_MAX_MSG)
        return;
    pthread_mutex_lock(&r->lock);
    fill_item(&r->items[r->written % MQ_RING], msg, len, at_ms);
    r->written++;
    pthread_mutex_unlock(&r->lock);
}

size_t mq_ring_read(mq_ring *r, uint64_t *cursor, mq_item *out, size_t max, uint64_t *skipped)
{
    pthread_mutex_lock(&r->lock);
    uint64_t oldest = r->written > MQ_RING ? r->written - MQ_RING : 0;
    if (*cursor < oldest) {
        if (skipped)
            *skipped += oldest - *cursor;
        *cursor = oldest;
    }
    size_t n = 0;
    while (*cursor < r->written && n < max) {
        copy_item(&out[n++], &r->items[*cursor % MQ_RING]);
        (*cursor)++;
    }
    pthread_mutex_unlock(&r->lock);
    return n;
}
