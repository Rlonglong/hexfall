#include "netsim.h"

#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "util.h"

#define QUEUE_SLOTS 4096

typedef struct {
    int used, fd;
    uint64_t due_ms;
    size_t len;
    socklen_t tolen;
    struct sockaddr_storage to;
    uint8_t data[HF_MAX_PACKET];
} delayed;

static netsim_cfg cfg;                /* set before threads start, then only read */
static _Thread_local delayed *queue;  /* this thread's; allocated on first use */

static void raw_send(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t tolen)
{
    /* With UDP a full socket buffer just drops the packet, as the network would. */
    if (to)
        (void)sendto(fd, buf, len, 0, to, tolen);
    else
        (void)send(fd, buf, len, 0);
}

void netsim_set(netsim_cfg c) { cfg = c; }
netsim_cfg netsim_get(void) { return cfg; }

void netsim_send(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t tolen,
                 uint64_t now_ms)
{
    if (cfg.loss > 0 && hf_random01() < cfg.loss)
        return;
    int delay = cfg.delay_ms + (cfg.jitter_ms > 0 ? (int)(hf_random01() * cfg.jitter_ms) : 0);
    if (delay <= 0 || len > HF_MAX_PACKET) {
        raw_send(fd, buf, len, to, tolen);
        return;
    }
    if (!queue && !(queue = calloc(QUEUE_SLOTS, sizeof *queue))) {
        raw_send(fd, buf, len, to, tolen);
        return;
    }
    for (int i = 0; i < QUEUE_SLOTS; i++) {
        delayed *d = &queue[i];
        if (d->used)
            continue;
        d->used = 1;
        d->fd = fd;
        d->due_ms = now_ms + (uint64_t)delay;
        d->len = len;
        d->tolen = to ? tolen : 0;
        if (to)
            memcpy(&d->to, to, tolen);
        memcpy(d->data, buf, len);
        return;
    }
    raw_send(fd, buf, len, to, tolen);  /* queue full: send it now */
}

void netsim_flush(uint64_t now_ms)
{
    if (!queue)
        return;
    for (int i = 0; i < QUEUE_SLOTS; i++) {
        delayed *d = &queue[i];
        if (!d->used || d->due_ms > now_ms)
            continue;
        raw_send(d->fd, d->data, d->len, d->tolen ? (struct sockaddr *)&d->to : NULL, d->tolen);
        d->used = 0;
    }
}

int netsim_wait_ms(uint64_t now_ms)
{
    if (!queue)
        return -1;
    int best = -1;
    for (int i = 0; i < QUEUE_SLOTS; i++) {
        if (!queue[i].used)
            continue;
        int wait = queue[i].due_ms > now_ms ? (int)(queue[i].due_ms - now_ms) : 0;
        if (best < 0 || wait < best)
            best = wait;
    }
    return best;
}

void netsim_release(void)
{
    free(queue);
    queue = NULL;
}

void netsim_forget_fd(int fd)
{
    /* The next socket opened will likely get the same number, and must not
     * send the old socket's packets. */
    if (!queue)
        return;
    for (int i = 0; i < QUEUE_SLOTS; i++)
        if (queue[i].used && queue[i].fd == fd)
            queue[i].used = 0;
}
