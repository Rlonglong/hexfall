# Hex Fall

A real-time multiplayer knockout game. Players run across stacked floors of hexagonal tiles; every tile you step on falls away half a second later, and the last player standing wins.

The project is a study in game networking on plain sockets, with no networking libraries:

- **An authoritative server in C.** Clients only send their inputs; the server runs the simulation and decides who is where, which tile fell and who is out.
- **UDP with a hand-rolled reliability layer:** per-packet sequence numbers, a 32-bit acknowledgement history, and a retransmission timeout estimated the way TCP does it.
- **One message format for every transport.** Desktop clients use UDP; browser clients will connect through a hand-written WebSocket server. Both speak exactly the same message bytes, and the test suite checks the C and JavaScript codecs against each other.

Built as the final project for the Network Programming course at NYCU.

## Status

- [x] Server: UDP transport, reliability layer, room state machine, shared simulation
- [x] Headless bot client, which doubles as a load generator
- [x] Fault injection (packet loss, delay, jitter) for testing and demos
- [x] Client network thread with NTP-style clock sync, stress-tested and checked with ThreadSanitizer
- [x] Desktop client (raylib), step 1: plays a full round, drawing the server's state as it arrives
- [ ] Desktop client: interpolation, prediction and reconciliation
- [ ] Browser client over a hand-written WebSocket server

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the design; [docs/PROTOCOL.md](docs/PROTOCOL.md) specifies every message.

## Build and test

Requirements: a C compiler (clang or gcc) and make. The desktop client also needs [raylib](https://www.raylib.com/) 5.5 or newer, found with `pkg-config` (`brew install raylib` on macOS); without it the client is skipped and everything else still builds. Node.js is needed only for the last test step.

```bash
make
```

```bash
make test
```

`make asan` rebuilds everything with AddressSanitizer and UndefinedBehaviorSanitizer and runs the tests again, including a fuzz pass over the message decoders. `make stress` runs a server, bots and many client network threads that stall on purpose; `make tsan` runs the threaded tests under ThreadSanitizer.

## Run

Start a server on UDP port 8080; a round starts once at least two players are ready:

```bash
./build/hexfall-server --port 8080 --min-players 2
```

Add ten bots:

```bash
./build/hexfall-bot --port 8080 -n 10
```

Simulate a bad network. Both programs accept these flags; give them to both to disturb traffic in both directions:

```bash
./build/hexfall-server --port 8080 --loss 10 --delay 40 --jitter 40
```

Play with the desktop client (WASD or arrow keys to move, Space to jump, Enter to get ready, C to switch camera, F1 for the network panel):

```bash
./build/hexfall-client --port 8080 --name alice
```

Connect from another machine with `--host <address or domain>`.

## Hosting a public server

1. Run the server on a machine with a public IP address, or behind a router you control.
2. On the router, forward **UDP** port 8080 to that machine. The browser client will additionally need a TCP port.
3. Allow the port through the host's firewall.
4. Optionally, point a DNS `A` record at the public address so clients can use a name.

Anything listening on a public port gets probed by scanners. The server drops malformed packets, caps every length it reads, and times out peers that never finish the handshake; `make asan` fuzzes the decoders.
