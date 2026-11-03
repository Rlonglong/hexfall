#ifndef HF_WIRE_H
#define HF_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Byte-level reading and writing for the wire format.
 *
 * Every multi-byte field is big-endian (network byte order). A browser reads
 * the same bytes with DataView, whose getters default to big-endian, so the
 * C and JavaScript sides never disagree. Structs are never memcpy'd onto the
 * wire: padding and byte order differ between machines.
 *
 * Writers and readers never touch memory outside their buffer. Running past
 * the end sets `overflow` / `error` and turns later calls into no-ops, so a
 * caller can write or read a whole message and check the flag once. */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    int overflow;
} wbuf;

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    int error;
} rbuf;

static inline void wb_init(wbuf *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

static inline int wb_reserve(wbuf *w, size_t n)
{
    if (w->overflow || w->cap - w->len < n) {
        w->overflow = 1;
        return 0;
    }
    return 1;
}

static inline void wb_u8(wbuf *w, uint8_t v)
{
    if (wb_reserve(w, 1))
        w->buf[w->len++] = v;
}

static inline void wb_u16(wbuf *w, uint16_t v)
{
    if (!wb_reserve(w, 2))
        return;
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)v;
}

static inline void wb_u32(wbuf *w, uint32_t v)
{
    if (!wb_reserve(w, 4))
        return;
    for (int shift = 24; shift >= 0; shift -= 8)
        w->buf[w->len++] = (uint8_t)(v >> shift);
}

static inline void wb_u64(wbuf *w, uint64_t v)
{
    if (!wb_reserve(w, 8))
        return;
    for (int shift = 56; shift >= 0; shift -= 8)
        w->buf[w->len++] = (uint8_t)(v >> shift);
}

static inline void wb_i8(wbuf *w, int8_t v) { wb_u8(w, (uint8_t)v); }
static inline void wb_i16(wbuf *w, int16_t v) { wb_u16(w, (uint16_t)v); }

static inline void wb_bytes(wbuf *w, const void *p, size_t n)
{
    if (!wb_reserve(w, n))
        return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

/* u8 length, then that many UTF-8 bytes. Longer strings are cut at 255 bytes. */
static inline void wb_str(wbuf *w, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    wb_u8(w, (uint8_t)n);
    wb_bytes(w, s, n);
}

static inline void rb_init(rbuf *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->error = 0;
}

static inline size_t rb_left(const rbuf *r) { return r->error ? 0 : r->len - r->pos; }

static inline int rb_need(rbuf *r, size_t n)
{
    if (r->error || r->len - r->pos < n) {
        r->error = 1;
        return 0;
    }
    return 1;
}

static inline uint8_t rb_u8(rbuf *r) { return rb_need(r, 1) ? r->buf[r->pos++] : 0; }

static inline uint16_t rb_u16(rbuf *r)
{
    if (!rb_need(r, 2))
        return 0;
    uint16_t v = (uint16_t)((r->buf[r->pos] << 8) | r->buf[r->pos + 1]);
    r->pos += 2;
    return v;
}

static inline uint32_t rb_u32(rbuf *r)
{
    if (!rb_need(r, 4))
        return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | r->buf[r->pos++];
    return v;
}

static inline uint64_t rb_u64(rbuf *r)
{
    if (!rb_need(r, 8))
        return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | r->buf[r->pos++];
    return v;
}

static inline int8_t rb_i8(rbuf *r) { return (int8_t)rb_u8(r); }
static inline int16_t rb_i16(rbuf *r) { return (int16_t)rb_u16(r); }

static inline void rb_bytes(rbuf *r, void *out, size_t n)
{
    if (!rb_need(r, n))
        return;
    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
}

/* Reads a u8-length string into out (cap counts the NUL). A longer string is
 * truncated; the caller cleans up any UTF-8 sequence cut in half. */
static inline void rb_str(rbuf *r, char *out, size_t cap)
{
    size_t n = rb_u8(r);
    if (!rb_need(r, n)) {
        if (cap)
            out[0] = 0;
        return;
    }
    size_t keep = n < cap - 1 ? n : cap - 1;
    memcpy(out, r->buf + r->pos, keep);
    out[keep] = 0;
    r->pos += n;
}

/* Splits the next n bytes off as their own reader. */
static inline rbuf rb_sub(rbuf *r, size_t n)
{
    rbuf s;
    if (!rb_need(r, n)) {
        rb_init(&s, NULL, 0);
        s.error = 1;
        return s;
    }
    rb_init(&s, r->buf + r->pos, n);
    r->pos += n;
    return s;
}

#endif
