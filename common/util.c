#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

uint64_t hf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t hf_now_ms(void) { return hf_now_us() / 1000u; }

// Session tokens and the cookie key: guessable values there would let
// anybody take over a player, so there is no fallback if this fails.
int hf_random_secret(void *buf, size_t n) { return n <= 256 && getentropy(buf, n) == 0 ? 0 : -1; }

static _Thread_local uint64_t rng_state;  // one generator per thread, no sharing

double hf_random01(void)
{
    if (rng_state == 0) {
        uint64_t seed;
        if (hf_random_secret(&seed, sizeof seed) != 0)
            seed = hf_now_us() ^ ((uint64_t)getpid() << 32);  // gameplay and fault injection only: need not be secret
        rng_state = seed | 1;
    }
    // xorshift64*
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    uint64_t r = rng_state * 0x2545F4914F6CDD1Dull;
    return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

void hf_log(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
