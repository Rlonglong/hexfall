#include "siphash.h"

// All words are read little-endian, as the specification says, whatever the
// machine.
static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static void sipround(uint64_t v[4])
{
    v[0] += v[1];
    v[1] = rotl(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl(v[0], 32);
    v[2] += v[3];
    v[3] = rotl(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = rotl(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = rotl(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl(v[2], 32);
}

uint64_t hf_siphash24(const uint8_t key[16], const void *data, size_t len)
{
    const uint8_t *in = data;
    uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v[4] = {
        0x736f6d6570736575ull ^ k0,  // "somepseudorandomlygeneratedbytes"
        0x646f72616e646f6dull ^ k1,
        0x6c7967656e657261ull ^ k0,
        0x7465646279746573ull ^ k1,
    };

    // Every whole 8-byte word: two rounds (the "2" in 2-4).
    size_t whole = len - len % 8;
    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m = le64(in + i);
        v[3] ^= m;
        sipround(v);
        sipround(v);
        v[0] ^= m;
    }

    // The last word: the bytes left over, and the length in the top byte.
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < len % 8; i++)
        b |= (uint64_t)in[whole + i] << (8 * i);
    v[3] ^= b;
    sipround(v);
    sipround(v);
    v[0] ^= b;

    // Finalisation: four rounds (the "4").
    v[2] ^= 0xff;
    for (int i = 0; i < 4; i++)
        sipround(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}
