#ifndef HF_NETSIM_H
#define HF_NETSIM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Optional fault injection on outgoing UDP packets: drop some, delay the rest
 * by delay_ms plus up to jitter_ms (which also reorders them). Used by the
 * tests, and to show the reliability and smoothing code working on demand.
 *
 * Set the configuration before starting threads. Each thread has its own
 * delay queue, so a thread must flush the packets it delayed itself. */

typedef struct {
    double loss;  /* 0..1 */
    int delay_ms;
    int jitter_ms;
} netsim_cfg;

void netsim_set(netsim_cfg cfg);
netsim_cfg netsim_get(void);

/* to == NULL sends on a connected socket. */
void netsim_send(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t tolen,
                 uint64_t now_ms);
void netsim_flush(uint64_t now_ms);
int netsim_wait_ms(uint64_t now_ms);  /* until the next delayed packet is due; -1 if none */
void netsim_release(void);            /* frees this thread's queue, dropping what is left */
void netsim_forget_fd(int fd);        /* drops packets queued for a socket about to be closed */

#endif
