#ifndef HF_UTIL_H
#define HF_UTIL_H

#include <stdint.h>

uint64_t hf_now_us(void);      // monotonic clock
uint64_t hf_now_ms(void);
#include <stddef.h>
int hf_random_secret(void *buf, size_t n);  // OS entropy, n <= 256; 0, or -1 if there is none (never a weaker fallback)
double hf_random01(void);      // fast per-thread PRNG in [0, 1), for gameplay and fault injection

void hf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
