#ifndef HF_UTIL_H
#define HF_UTIL_H

#include <stdint.h>

uint64_t hf_now_us(void);      /* monotonic clock */
uint64_t hf_now_ms(void);
uint64_t hf_random_u64(void);  /* from the OS entropy source; used for session ids */
double hf_random01(void);      /* fast per-thread PRNG in [0, 1), for gameplay and fault injection */

void hf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
