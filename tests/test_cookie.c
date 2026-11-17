#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cookie.h"
#include "protocol.h"
#include "siphash.h"

///////////////////////////////////////////////////////////////////////////////////
// SipHash against the test vectors of its paper, and address cookies: right
// for the address, the port and the time they were made for, and for nothing
// else.
///////////////////////////////////////////////////////////////////////////////////

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static struct sockaddr_in v4(const char *ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static struct sockaddr_in6 v6(const char *ip, uint16_t port)
{
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}

#define SA(a) ((const struct sockaddr *)&(a))

static void test_siphash(void)
{
    // The paper's key 00 01 .. 0f, and messages 00 01 .. (len - 1); outputs
    // as the paper prints them (appendix A, and the reference vectors).
    uint8_t key[16], msg[15];
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)i;
    for (int i = 0; i < 15; i++)
        msg[i] = (uint8_t)i;
    CHECK(hf_siphash24(key, msg, 0) == 0x726fdb47dd0e0e31ull);
    CHECK(hf_siphash24(key, msg, 15) == 0xa129ca6149be45e5ull);
}

static void test_cookies(void)
{
    hf_cookie_key k, other;
    for (int i = 0; i < 16; i++) {
        k.key[i] = (uint8_t)(i * 7 + 1);
        other.key[i] = (uint8_t)(i * 7 + 2);
    }
    struct sockaddr_in a = v4("203.0.113.5", 5000), port = v4("203.0.113.5", 5001), ip = v4("203.0.113.6", 5000);
    struct sockaddr_in6 mapped = v6("::ffff:203.0.113.5", 5000);
    const uint64_t t = 1234567;  // 4.6 s into slot 123
    uint64_t c = hf_cookie_make(&k, SA(a), t);

    CHECK(c != 0 && (c >> 48) == t / HF_COOKIE_SLOT_MS);
    CHECK(hf_cookie_check(&k, SA(a), c, t));
    CHECK(hf_cookie_check(&k, SA(a), c, t + HF_COOKIE_SLOT_MS));        // still good in the next slot
    CHECK(!hf_cookie_check(&k, SA(a), c, t + 2 * HF_COOKIE_SLOT_MS));   // not after that
    CHECK(hf_cookie_check(&k, SA(mapped), c, t));                       // the same address written as IPv6
    CHECK(!hf_cookie_check(&k, SA(port), c, t));
    CHECK(!hf_cookie_check(&k, SA(ip), c, t));
    CHECK(!hf_cookie_check(&other, SA(a), c, t));                       // another server, or a restart
    CHECK(!hf_cookie_check(&k, SA(a), 0, t));
    for (int bit = 0; bit < 64; bit++)
        CHECK(!hf_cookie_check(&k, SA(a), c ^ (1ull << bit), t));      // any change gives it away

    // A real key from the system, and the cookie in the envelope.
    hf_cookie_key real;
    CHECK(hf_cookie_key_init(&real) == 0);
    uint64_t rc = hf_cookie_make(&real, SA(a), t);
    uint8_t buf[HF_UDP_HEADER_BYTES];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_udp_header(&w, rc, HF_UDP_COOKIE);
    CHECK(!w.overflow && w.len == HF_UDP_HEADER_BYTES);
    rbuf r;
    rb_init(&r, buf, w.len);
    uint64_t session;
    uint8_t flags;
    CHECK(hf_get_udp_header(&r, &session, &flags) && session == rc && flags == HF_UDP_COOKIE);
    CHECK(hf_cookie_check(&real, SA(a), session, t));
}

int main(void)
{
    test_siphash();
    test_cookies();
    printf("cookie: all checks passed\n");
    return 0;
}
