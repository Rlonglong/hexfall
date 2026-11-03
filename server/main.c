#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netsim.h"
#include "server.h"
#include "sim.h"
#include "util.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port N] [--min-players N] [--loss PERCENT] [--delay MS] [--jitter MS]\n"
            "  --loss/--delay/--jitter disturb outgoing packets on purpose (testing, demos)\n",
            argv0);
}

int main(int argc, char **argv)
{
    int port = 8080, min_players = 2;
    netsim_cfg ns = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--port") && v)
            port = atoi(argv[++i]);
        else if (!strcmp(a, "--min-players") && v)
            min_players = atoi(argv[++i]);
        else if (!strcmp(a, "--loss") && v)
            ns.loss = atof(argv[++i]) / 100.0;
        else if (!strcmp(a, "--delay") && v)
            ns.delay_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--jitter") && v)
            ns.jitter_ms = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    netsim_set(ns);
    room_init(min_players);

    int fd = udp_open((uint16_t)port);
    if (fd < 0) {
        perror("cannot open the UDP port");
        return 1;
    }
    hf_log("hexfall server: UDP port %d, %d ticks/s, game starts with %d+ players", port, SIM_TICK_HZ,
           min_players);
    if (ns.loss > 0 || ns.delay_ms > 0 || ns.jitter_ms > 0)
        hf_log("network disturbance on: loss %.0f%%, delay %d ms, jitter %d ms", ns.loss * 100, ns.delay_ms,
               ns.jitter_ms);

    /* One thread, one loop: wait for packets until the next tick is due, then
     * simulate, send, and check timeouts. */
    const uint64_t tick_us = 1000000u / SIM_TICK_HZ;
    uint64_t next_tick = hf_now_us() + tick_us;
    uint64_t next_stats = hf_now_ms() + 5000;
    while (!stop) {
        uint64_t now_us = hf_now_us();
        int timeout = now_us >= next_tick ? 0 : (int)((next_tick - now_us + 999) / 1000);
        int delayed = netsim_wait_ms(now_us / 1000);
        if (delayed >= 0 && delayed < timeout)
            timeout = delayed;

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, timeout);
        if (ready < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        if (ready > 0 && (pfd.revents & POLLIN))
            udp_receive(fd, hf_now_ms());

        now_us = hf_now_us();
        for (int caught_up = 0; now_us >= next_tick && caught_up < 5; caught_up++) {
            uint64_t tick_ms = now_us / 1000;
            room_tick(tick_ms);
            udp_flush(fd, tick_ms);
            next_tick += tick_us;
        }
        if (now_us >= next_tick)
            next_tick = now_us + tick_us;  /* fell far behind: skip rather than spiral */

        uint64_t now_ms = hf_now_ms();
        udp_maintain(now_ms);
        netsim_flush(now_ms);
        if (now_ms >= next_stats) {
            char room[160], udp[200];
            room_stats(room, sizeof room);
            udp_stats(udp, sizeof udp);
            hf_log("%s | %s", room, udp);
            next_stats = now_ms + 5000;
        }
    }
    hf_log("shutting down");
    close(fd);
    return 0;
}
