#!/bin/sh
# Stress test for the client's network thread.
#
# A server, bots that keep rounds going (so tiles keep falling and reliable
# events keep flowing), and netprobe: one network thread per probe, each of
# which freezes its "main thread" for a few seconds every round so its event
# queue overflows and the resync path runs. Received packets are also
# dropped and jittered. netprobe checks what arrives and exits non-zero on
# any problem.
#
# Received packets get jitter but no constant extra delay: a constant delay
# in one direction only is an asymmetric path, which no NTP-style method can
# detect, and it would skew the clock check by half the delay.

set -u
PORT=${PORT:-9300}
PROBES=${PROBES:-16}
BOTS=${BOTS:-10}
DURATION=${DURATION:-45}

cd "$(dirname "$0")/.." || exit 1
./build/hexfall-server --port "$PORT" --min-players 2 >build/stress-server.log 2>&1 &
server=$!
./build/hexfall-bot --port "$PORT" -n "$BOTS" --duration $((DURATION + 5)) >build/stress-bots.log 2>&1 &
bots=$!
trap 'kill $server $bots 2>/dev/null' EXIT INT TERM

./build/hexfall-netprobe --port "$PORT" -n "$PROBES" --events 48 --stall 4000 --duration "$DURATION" \
    --in-loss 5 --in-jitter 30
status=$?
echo "server: $(grep -c 'round started' build/stress-server.log) rounds, $(grep -c 'resumed' build/stress-server.log) sessions resumed"
exit $status
