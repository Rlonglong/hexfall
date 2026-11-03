# Architecture

## Principles

1. **The server is the authority.** Where everybody is, which tile fell and who is out is decided by the server alone. Clients send inputs and draw what they are told.
2. **One message format.** Desktop clients use UDP and browser clients will use WebSocket, but every message is the same bytes either way. Only the outer envelope differs.
3. **Transports are interchangeable.** The game logic (`server/room.c`) only ever calls `conn_send()` and does not know whether a connection is UDP or WebSocket.

## Layers

```
 desktop client / bot (C)                  browser client (JS, planned)
          │                                          │
 ┌────────┴──────────────┐              ┌────────────┴────────────┐
 │ UDP envelope (12 B)    │              │ WebSocket frames (TCP)   │
 │ reliability layer      │              │ hand-written, no library │
 └────────┬──────────────┘              └────────────┬────────────┘
          └────────── same messages: type | length | payload ──────────┘
                                     │
               conn_send() ── room.c (game, state machine) ── sim.c (physics)
```

## Reliable and unreliable messages

| Class | Messages | Over UDP | Over WebSocket |
|---|---|---|---|
| Reliable, ordered | HELLO, READY, BYE, WELCOME, ROOM, TILES, TILE_EVENTS, ELIMINATED, ERROR | Reliability layer: resent until acknowledged, delivered in order | Sent as is; TCP already guarantees delivery and order |
| Unreliable, newest wins | INPUT, PING, PONG, SNAPSHOT | Sent once; a lost one is simply replaced by the next | Sent as is; when the connection backs up, stale snapshots are dropped instead of queued |

The class is a property of the message type (`hf_msg_is_reliable()`), shared by both transports.

## The UDP reliability layer (`common/reliable.c`)

- **Every packet carries a sequence number,** plus the newest sequence number received from the peer and a 32-bit history of the packets before it. One packet can therefore acknowledge up to 33 of the peer's packets, and a lost acknowledgement costs nothing as long as a later packet gets through.
- **Reliable messages** ride in packets and are resent after a timeout until a packet carrying them is acknowledged. The timeout is computed like TCP's: `RTO = SRTT + 4 × RTTVAR`.
- **Every packet has its own send time,** even when it carries resent messages, so RTT samples are never ambiguous (the problem Karn's algorithm solves for TCP).
- **The receiver orders reliable messages** in a 128-message window before handing them over, and discards duplicates.
- **Unreliable messages (snapshots, inputs) are never resent.** A new snapshot arrives 33 ms later, so resending an old one would only add delay. This is how a game avoids TCP's head-of-line blocking, where one lost packet holds up everything behind it.

`tests/test_reliable.c` runs two endpoints over a simulated link with 30% loss, 20–120 ms of delay (which reorders packets) and 5% duplication; 1,900 reliable messages must all arrive exactly once and in order.

## Connections and reconnecting

- **Handshake.** A client's first packets carry session 0 and a HELLO. The server matches them by source address, replies with WELCOME carrying a random 64-bit token, and from then on the client puts the token in every packet.
- **Changing networks.** The server recognises clients by token, not by address. When a phone moves from Wi-Fi to cellular, or a NAT picks a new external port, the server sees the right token from a new address and simply updates the address.
- **Losing a connection.** After 4 seconds without a packet the connection is considered lost. The player's slot is kept for 15 seconds; a HELLO carrying the latest token within that time reclaims the same player.
- **Every connection gets a fresh token,** even one that reclaims a player. Packets still in flight from the old connection then carry a token nobody owns and are dropped as stale, instead of mixing their sequence numbers and acknowledgements into the new connection's reliability state.
- **Heartbeats.** Both sides send at least one packet per tick (33 ms), carrying acknowledgements at minimum, so no separate heartbeat message is needed. This also keeps NAT mappings on the client's side alive.

## The server loop (`server/main.c`)

One thread, one loop:

```
loop:
    poll(UDP socket, until the next tick is due)
    packets → udp_receive(): envelope → reliability layer → room_on_message()
    tick due → room_tick(): simulate one tick → udp_flush(): one packet per connection
    udp_maintain(): timeouts, closing connections
```

The game runs 30 ticks per second (`SIM_TICK_HZ`). If the loop falls far behind, it skips ticks instead of trying to catch up forever.

## Room state machine (`server/room.c`)

```
LOBBY ──enough players, all ready──> COUNTDOWN ──3 s──> PLAYING ──one left──> RESULT ──5 s──> LOBBY
  ^                                     │
  └──────someone left or un-readied─────┘
```

Each player is connected, disconnected but within the grace period, or removed. During a round a player is either still on the board or eliminated with a finishing place.

## Inputs (ready for prediction and reconciliation)

- A client sends one INPUT per tick. Every input sample has a sequence number, and each INPUT repeats the newest four samples, so losing a packet loses no input.
- The server queues samples and consumes exactly one per tick. When a burst arrives after a stall, it skips the oldest samples but keeps any jump.
- Every snapshot tells the client which input sequence number its player was last simulated with (`ack_input`). A predicting client replays its inputs from that point.

## Tiles

- When a tile is stepped on, the server broadcasts once that this tile on this floor was touched at this tick (TILE_EVENTS).
- A touched tile falls exactly 16 ticks later (about 0.53 s). Every client can work that out, so the whole board never needs to be sent every tick.
- New clients, and ticks where too many tiles were touched for one event message, get the whole board instead (TILES: one bit per tile, 82 bytes for all 651 tiles).

## Files

| File | Contents |
|---|---|
| `common/wire.h` | Big-endian readers and writers; running past a buffer sets a flag and never reads or writes out of bounds |
| `common/protocol.[ch]` | Message definitions, encoders, decoders |
| `common/reliable.[ch]` | The UDP reliability layer |
| `common/sim.[ch]` | Arena and physics, shared by server and clients |
| `common/netsim.[ch]` | Fault injection: drop and delay outgoing packets |
| `server/udp.c` | UDP transport: socket, connection table, handshake, timeouts |
| `server/room.c` | The game: lobby, rounds, eliminations |
| `client/net_client.[ch]` | Desktop networking core, shared by bots and the desktop client |
| `client/bot.c` | Headless bots, also a load generator |
| `web/protocol.js` | Browser codec, byte-for-byte compatible with the C one |

## Adding the WebSocket transport

1. `server/ws.c`: a TCP listening socket in the same `poll()` loop.
2. Per-connection receive and send buffers: TCP is a byte stream, so one `recv()` may return half a frame or several frames.
3. A minimal HTTP/1.1 handler for two requests only: `GET` for the page and its scripts, and the WebSocket upgrade handshake (`Sec-WebSocket-Accept` needs SHA-1 and Base64).
4. WebSocket framing: the three payload-length encodings, unmasking client frames, ping, pong and close.
5. A `TRANSPORT_WEBSOCKET` branch in `conn_send()`: each message becomes one binary frame; when the send buffer backs up, unreliable messages are dropped.
6. No message changes and no change to `room.c`.

## Next steps

- [x] Client network thread, channels and clock synchronisation (see [CLIENT.md](CLIENT.md))
- [x] Desktop client (raylib) on top of the network thread, drawing snapshots as they arrive
- [ ] Client-side prediction of the local player, reconciled with `ack_input`
- [ ] Interpolation of remote players: none, fixed delay, adaptive delay
- [ ] Lag compensation for jumps
- [ ] Reconnect testing with bots (HELLO with a resume token)
- [ ] Monitoring panel: RTT, resends and loss per connection
