#ifndef HF_COOKIE_H
#define HF_COOKIE_H

#include <stdint.h>
#include <sys/socket.h>

///////////////////////////////////////////////////////////////////////////////////
// Address cookies: proof that a peer receives what is sent to its address.
//
//     A UDP packet's source address can be forged. If the server opened a
//     connection, and started sending, for every packet that says hello, one
//     forged packet would make it send a stream of packets to somebody else
//     (reflection and amplification), and a flood of them would take every
//     connection slot. So before it keeps anything for an address, the server
//     sends a cookie to that address, and opens a connection only for a packet
//     that carries the cookie back: whoever sends it receives at that address.
//
//     The cookie is the time slot and a SipHash of the address, the port and
//     the time slot under a key only the server knows. The server keeps no
//     list of the cookies it gave out: it works each one out again when it
//     comes back, as TCP SYN cookies do. A cookie is good in its own time slot
//     and the next one, 10 to 20 s; the key changes whenever the server starts.
//
//         cookie (64 bits) = time slot (16 bits) | SipHash (top 48 bits)
///////////////////////////////////////////////////////////////////////////////////

#define HF_COOKIE_SLOT_MS 10000

typedef struct {
    uint8_t key[16];
} hf_cookie_key;

int hf_cookie_key_init(hf_cookie_key *k);  // 0, or -1 without a secure random source

// The cookie for this address at time now_ms (any clock, as long as it is
// always the same one), and whether a cookie that came back is right.
uint64_t hf_cookie_make(const hf_cookie_key *k, const struct sockaddr *addr, uint64_t now_ms);
int hf_cookie_check(const hf_cookie_key *k, const struct sockaddr *addr, uint64_t cookie, uint64_t now_ms);

#endif
