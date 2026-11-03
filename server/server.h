#ifndef HF_SERVER_H
#define HF_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "protocol.h"
#include "reliable.h"

#define MAX_CONNS 64
#define CONN_TIMEOUT_MS 4000       /* nothing heard for this long: the connection is lost */
#define HANDSHAKE_TIMEOUT_MS 5000  /* a peer that never sends HELLO */
#define RECONNECT_GRACE_MS 15000   /* a lost player's slot is kept this long */

/* One client connection, whatever carries it. The room only ever calls
 * conn_send(); how a message travels is the transport's business. The
 * WebSocket transport is added next to UDP without touching room.c. */
typedef enum { TRANSPORT_UDP = 0, TRANSPORT_WEBSOCKET = 1 } transport_kind;

typedef struct conn {
    int used;
    transport_kind transport;
    uint64_t token;           /* this connection's session id; the client puts it in every UDP packet */
    int token_confirmed;      /* the client has started using the token */
    struct sockaddr_storage addr;
    socklen_t addr_len;
    rel_endpoint rel;         /* UDP only */
    uint64_t created_ms, last_recv_ms, close_at_ms;
    int kill;                 /* set from inside a callback; acted on in udp_maintain */
    int flush_now;            /* a PONG is waiting: send without waiting for the tick */
    int player;               /* room slot, -1 until HELLO */
} conn;

/* The message type picks the channel: reliable types are resent until acked
 * and arrive in order; the rest are sent once. */
void conn_send(conn *c, const uint8_t *msg, size_t len);
void conn_close_soon(conn *c, uint64_t now_ms);  /* let queued messages go out, then drop */
const char *conn_addr_str(const conn *c);

/* room.c: the game. Knows nothing about sockets. */
void room_init(int min_players);
void room_on_message(conn *c, uint8_t type, rbuf *payload, uint64_t now_ms);
void room_on_conn_lost(conn *c, uint64_t now_ms);
void room_tick(uint64_t now_ms);
void room_stats(char *out, size_t cap);

/* udp.c: the UDP transport. */
int udp_open(uint16_t port);
void udp_receive(int fd, uint64_t now_ms);  /* also sends PONGs immediately */
void udp_flush(int fd, uint64_t now_ms);
void udp_maintain(uint64_t now_ms);
void udp_stats(char *out, size_t cap);

#endif
