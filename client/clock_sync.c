#include "clock_sync.h"

#include <string.h>

#define MAX_RTT_MS 5000u  /* longer than this is a stale or bogus reply */
#define SETTLE_SAMPLES 4  /* until then, follow the best sample directly */
#define SLEW_MS 2         /* once settled, move at most this much per sample */
#define JUMP_MS 100       /* farther off than this, jump instead (server restarted) */

void cs_init(clock_sync *c) { memset(c, 0, sizeof *c); }

int cs_add(clock_sync *c, uint32_t t0, uint32_t ts, uint32_t t1)
{
    uint32_t rtt = t1 - t0;
    if (rtt > MAX_RTT_MS)
        return 0;
    c->s[c->next].rtt = rtt;
    c->s[c->next].offset = ts - (t0 + rtt / 2);
    c->next = (c->next + 1) % CS_WINDOW;
    if (c->count < CS_WINDOW)
        c->count++;

    /* Oldest to newest, so that among equally fast samples the newest wins. */
    int start = c->count < CS_WINDOW ? 0 : c->next, best = start;
    double sum = 0;
    for (int i = 0; i < c->count; i++) {
        int k = (start + i) % CS_WINDOW;
        if (c->s[k].rtt <= c->s[best].rtt)
            best = k;
        sum += c->s[k].rtt;
    }
    double mean = sum / c->count, dev = 0;
    for (int i = 0; i < c->count; i++)
        dev += c->s[i].rtt > mean ? c->s[i].rtt - mean : mean - c->s[i].rtt;

    uint32_t target = c->s[best].offset;
    int32_t diff = (int32_t)(target - c->offset);
    if (!c->valid || c->count < SETTLE_SAMPLES || diff > JUMP_MS || diff < -JUMP_MS)
        c->offset = target;
    else if (diff > SLEW_MS)
        c->offset += SLEW_MS;
    else if (diff < -SLEW_MS)
        c->offset -= SLEW_MS;
    else
        c->offset = target;
    c->valid = 1;
    c->rtt_ms = rtt;
    c->rtt_min_ms = c->s[best].rtt;
    c->jitter_ms = dev / c->count;
    return 1;
}

uint32_t cs_server_time(const clock_sync *c, uint32_t local_ms) { return local_ms + c->offset; }
