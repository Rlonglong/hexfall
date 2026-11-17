#ifndef HF_SIPHASH_H
#define HF_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

///////////////////////////////////////////////////////////////////////////////////
// SipHash-2-4 (Aumasson and Bernstein, 2012): a keyed hash for short inputs.
//
//     With a secret 128-bit key, nobody who does not know the key can work out
//     the hash of an input, nor find two inputs with the same hash. That makes
//     it a message authentication code for small things, which is what the
//     address cookies need (cookie.h); Linux uses it for TCP SYN cookies too.
//     Written from the paper; tests/test_cookie.c checks it against the
//     paper's test vectors.
///////////////////////////////////////////////////////////////////////////////////

uint64_t hf_siphash24(const uint8_t key[16], const void *data, size_t len);

#endif
