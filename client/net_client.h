#ifndef HF_NET_CLIENT_H
#define HF_NET_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "protocol.h"
#include "reliable.h"

/* The desktop side of the UDP transport: one socket, the reliability layer,
 * and the session handshake. Bots use it now; the desktop game client will
 * use the same code, and only add drawing and keyboard input. */

typedef struct hf_client hf_client;
typedef void (*hf_client_msg_fn)(hf_client *cl, uint8_t type, rbuf *payload, void *user);

struct hf_client {
    int fd;
    uint64_t token;  /* from WELCOME; 0 before */
    uint16_t player_id;
    int welcomed;
    rel_endpoint rel;
    uint64_t last_recv_ms;
    uint64_t bytes_in, bytes_out;
    hf_client_msg_fn on_msg;
    void *user;
};

/* getaddrinfo() wrapper: host name or address, IPv4 or IPv6. */
int hf_resolve(const char *host, const char *port, struct sockaddr_storage *out, socklen_t *len);

int hf_client_open(hf_client *cl, const struct sockaddr *server, socklen_t len, hf_client_msg_fn fn,
                   void *user);
void hf_client_hello(hf_client *cl, const char *name, uint8_t kind, uint64_t resume_token);
void hf_client_send(hf_client *cl, const uint8_t *msg, size_t len);  /* channel picked by type */
void hf_client_receive(hf_client *cl, uint64_t now_ms);              /* drains the socket */
void hf_client_flush(hf_client *cl, uint64_t now_ms);                /* call once per tick */
void hf_client_close(hf_client *cl);

#endif
