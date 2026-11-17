#include "cookie.h"

#include <netinet/in.h>
#include <string.h>

#include "siphash.h"
#include "util.h"

int hf_cookie_key_init(hf_cookie_key *k) { return hf_random_secret(k->key, sizeof k->key); }

// The address as 16 bytes, IPv4 written as ::ffff:a.b.c.d, so a client gets
// the same cookie whether the server's socket shows it as IPv4 or as IPv6;
// then the port and the slot.
static uint64_t mac(const hf_cookie_key *k, const struct sockaddr *addr, uint32_t slot)
{
    uint8_t in[16 + 2 + 4];
    memset(in, 0, sizeof in);
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)addr;
        in[10] = in[11] = 0xff;
        memcpy(in + 12, &a->sin_addr, 4);
        memcpy(in + 16, &a->sin_port, 2);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)addr;
        memcpy(in, &a->sin6_addr, 16);
        memcpy(in + 16, &a->sin6_port, 2);
    }
    for (int i = 0; i < 4; i++)
        in[18 + i] = (uint8_t)(slot >> (24 - 8 * i));
    return hf_siphash24(k->key, in, sizeof in);
}

static uint64_t make(const hf_cookie_key *k, const struct sockaddr *addr, uint32_t slot)
{
    return ((uint64_t)(slot & 0xFFFF) << 48) | (mac(k, addr, slot) >> 16);
}

uint64_t hf_cookie_make(const hf_cookie_key *k, const struct sockaddr *addr, uint64_t now_ms)
{
    return make(k, addr, (uint32_t)(now_ms / HF_COOKIE_SLOT_MS));
}

int hf_cookie_check(const hf_cookie_key *k, const struct sockaddr *addr, uint64_t cookie, uint64_t now_ms)
{
    uint32_t slot = (uint32_t)(now_ms / HF_COOKIE_SLOT_MS);
    return cookie != 0 && (cookie == make(k, addr, slot) || cookie == make(k, addr, slot - 1));
}
