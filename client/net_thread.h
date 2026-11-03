#ifndef HF_NET_THREAD_H
#define HF_NET_THREAD_H

#include <stddef.h>
#include <stdint.h>

#include "msg_queue.h"
#include "netsim.h"

/* The network thread. It owns the UDP socket, the reliability layer, clock
 * synchronisation and reconnecting. The main thread never touches any of
 * that; it uses only the functions below, which go through four channels:
 *
 *   outbox     main -> network   messages to send (FIFO); a byte on the
 *                                self-pipe wakes the network thread at once
 *   events     network -> main   reliable messages, in order (FIFO, never drops)
 *   snapshots  network -> main   the newest snapshots (ring, oldest overwritten)
 *   status     network -> main   a copy of net_status, refreshed every loop
 *
 * Connection states. Only the network thread changes them:
 *
 *   CONNECTING --WELCOME--> CONNECTED --4 s without packets--> RECONNECTING --WELCOME--> CONNECTED
 *                               |
 *                               +--events queue full--> RESYNCING
 *
 * RESYNCING: the main thread stopped draining events (its window was being
 * dragged, say) and a reliable message had nowhere to go. Dropping it would
 * leave the main thread with a wrong board, so the network thread discards
 * this session's messages, waits until the main thread has emptied the
 * queue, and reconnects with its session token. The server keeps the same
 * player and sends WELCOME and the whole board again, from which the main
 * thread rebuilds its state. Any state can go to STOPPED via net_stop(). */

typedef enum { NET_CONNECTING, NET_CONNECTED, NET_RECONNECTING, NET_RESYNCING, NET_STOPPED } net_state;

typedef struct {
    const char *host, *port, *name;
    uint8_t kind;           /* CLIENT_DESKTOP, CLIENT_BOT, ... */
    size_t event_capacity;  /* reliable messages that may wait for the main thread; 0 means 1024 */
} net_config;

typedef struct {
    net_state state;
    uint16_t player_id;
    uint64_t token;
    int clock_valid;
    uint32_t clock_offset;  /* server ms = local ms + clock_offset (mod 2^32) */
    double rtt_ms, rtt_min_ms, jitter_ms;
    uint64_t packets_in, packets_out, resends;  /* this session */
    uint64_t sessions, resyncs, reconnects;     /* since net_start */
    uint64_t events_queued, snapshots_received, outbox_refused;
    netsim_cfg inbound;                         /* disturbance applied to received packets */
} net_status;

typedef struct net_thread net_thread;

net_thread *net_start(const net_config *cfg);  /* NULL if the host cannot be resolved or no thread */
void net_stop(net_thread *n);                  /* says BYE, joins the thread, frees everything */

/* Main thread side. */
void net_send(net_thread *n, const uint8_t *msg, size_t len);
size_t net_events(net_thread *n, mq_item *out, size_t max);
size_t net_snapshots(net_thread *n, uint64_t *cursor, mq_item *out, size_t max, uint64_t *skipped);
void net_get_status(net_thread *n, net_status *out);
void net_set_inbound(net_thread *n, netsim_cfg cfg);  /* disturb received packets (demos) */

const char *net_state_name(net_state s);

#endif
