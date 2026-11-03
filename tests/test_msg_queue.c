#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg_queue.h"

/* One producer thread, one consumer thread, as between the network thread
 * and the main thread. Run under `make tsan` to have ThreadSanitizer check
 * that every shared access is protected. */

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define COUNT 300000

static mq_fifo fifo;
static mq_ring ring;

/* Item k is 8 + k % 200 bytes long and starts with k. */
static size_t make_item(uint8_t *buf, uint64_t k)
{
    size_t len = 8 + (size_t)(k % 200);
    memcpy(buf, &k, sizeof k);
    memset(buf + 8, (int)(k & 0xFF), len - 8);
    return len;
}

static void *fifo_producer(void *arg)
{
    (void)arg;
    uint8_t buf[HF_MAX_MSG];
    for (uint64_t k = 0; k < COUNT; k++) {
        size_t len = make_item(buf, k);
        while (mq_fifo_push(&fifo, buf, len, k) != 0)
            sched_yield();  /* full: the consumer will catch up */
    }
    return NULL;
}

static void *ring_producer(void *arg)
{
    (void)arg;
    uint8_t buf[HF_MAX_MSG];
    for (uint64_t k = 0; k < COUNT; k++)
        mq_ring_push(&ring, buf, make_item(buf, k), k);
    return NULL;
}

static void test_fifo(size_t cap)
{
    static mq_item out[16];
    CHECK(mq_fifo_init(&fifo, cap) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, fifo_producer, NULL) == 0);
    uint64_t expect = 0;
    while (expect < COUNT) {
        size_t n = mq_fifo_pop(&fifo, out, 16);
        if (n == 0)
            sched_yield();
        for (size_t i = 0; i < n; i++, expect++) {
            uint64_t k;
            memcpy(&k, out[i].data, sizeof k);
            CHECK(k == expect);  /* nothing lost, duplicated or reordered */
            CHECK(out[i].at_ms == expect && out[i].len == 8 + expect % 200);
            CHECK(out[i].len == 8 || out[i].data[out[i].len - 1] == (uint8_t)(expect & 0xFF));
        }
    }
    pthread_join(t, NULL);
    CHECK(mq_fifo_count(&fifo) == 0);
    printf("msg_queue: FIFO (capacity %zu) moved %d items in order, %llu pushes refused while full\n", cap, COUNT,
           (unsigned long long)mq_fifo_refused(&fifo));
    mq_fifo_destroy(&fifo);
}

static void test_ring(void)
{
    static mq_item out[8];
    CHECK(mq_ring_init(&ring) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, ring_producer, NULL) == 0);
    uint64_t cursor = 0, skipped = 0, seen = 0, last = 0;
    int have_last = 0, done = 0;
    while (!done) {
        size_t n = mq_ring_read(&ring, &cursor, out, 8, &skipped);
        for (size_t i = 0; i < n; i++) {
            uint64_t k;
            memcpy(&k, out[i].data, sizeof k);
            CHECK(!have_last || k > last);  /* always newer, even when items were skipped */
            last = k;
            have_last = 1;
            seen++;
        }
        if (cursor == COUNT)
            done = 1;
        else if (n == 0)
            sched_yield();
    }
    pthread_join(t, NULL);
    CHECK(seen + skipped == COUNT);
    printf("msg_queue: ring read %llu items, %llu overwritten before the reader got to them\n",
           (unsigned long long)seen, (unsigned long long)skipped);
    mq_ring_destroy(&ring);
}

int main(void)
{
    test_fifo(1);  /* the tightest case: every push waits for a pop */
    test_fifo(64);
    test_ring();
    printf("msg_queue: all checks passed\n");
    return 0;
}
