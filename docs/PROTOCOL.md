# Protocol (version 2)

The C implementation is in `common/protocol.[ch]` and the browser one in `web/protocol.js`. `make test` checks that they agree byte for byte: the C side writes every message type, the JavaScript side decodes them field by field, and the client-to-server messages are re-encoded in JavaScript and compared byte by byte.

## Conventions

- Every multi-byte field is **big-endian** (network byte order). JavaScript's `DataView` reads big-endian by default.
- No floating point on the wire. Positions and velocities are `int16` in hundredths of a unit (`HF_POS_SCALE`); facing is a `u16` where 65536 is a full turn; stick axes are `int8` from -127 to 127.
- Strings are a `u8` byte length followed by UTF-8 bytes. Player names are at most 24 bytes; the server strips control characters and any character cut in half by the limit.
- Decoders ignore trailing bytes after the fields they know, so a later version can append fields to a message and older clients still read it.

## Messages

```
u8 type | u16 payload length | payload
```

The same message can travel in a UDP packet or a WebSocket binary frame. A packet or frame may carry several messages back to back.

## UDP envelope (UDP only)

```
u16 magic 0x4846 ("HF") | u8 protocol version | u8 flags (0) | u64 session (0 until WELCOME)
```

followed by the reliability header (10 bytes):

```
u16 seq | u8 flags (bit 0: the ack fields are valid) | u16 ack | u32 ack_bits | u8 reliable count n
n × ( u16 reliable message id | message )
unreliable messages up to the end of the packet
```

- `ack` is the newest packet received; bit `i` of `ack_bits` means packet `ack - 1 - i` was received too.
- Packets are at most 1200 bytes (`HF_MAX_PACKET`) to stay below common path MTUs and avoid IP fragmentation.
- WebSocket connections skip this layer: TCP already guarantees delivery and order.

## Client → server

| Type | Name | Channel | Payload |
|---|---|---|---|
| 1 | HELLO | reliable | `u16 version`, `u8 kind` (0 desktop, 1 phone, 2 bot), `u64 resume_token` (0 for a new player, the old token to reclaim a slot), `str name` |
| 2 | READY | reliable | `u8 ready` |
| 3 | INPUT | unreliable | `u8 n` (at most 4, newest first), each: `u16 seq`, `i8 mx`, `i8 mz`, `u8 buttons` (pressed since the previous sample: 1 jump, 2 dive, 4 shove) |
| 4 | PING | unreliable | `u32 client_ms` |
| 5 | BYE | reliable | `u8 reason` |

`mx` and `mz` are a direction in world space. A client converts "stick up" to "the way the camera faces" before sending.

## Server → client

| Type | Name | Channel | Payload |
|---|---|---|---|
| 64 | WELCOME | reliable | `u64 token` (this connection's session id; send the latest one in HELLO to reclaim the player), `u16 player_id`, `u8 color`, `u8 tick_hz`, `u32 server_ms`, `u32 tick` |
| 65 | PONG | unreliable | `u32 client_ms` (echoed), `u32 server_ms` |
| 66 | ROOM | reliable | `u8 phase`, `u32 tick`, `u32 phase_end_tick`, `u8 n`, each player: `u16 id`, `u8 color`, `u8 flags` (1 ready, 2 alive, 4 connected), `u8 place` (1 is the winner, 0 still playing or not placed), `str name` |
| 67 | SNAPSHOT | unreliable | `u32 tick`, `u16 ack_input`, `u8 n`, each player: `u16 id`, `i16 x, y, z, vx, vy, vz`, `u16 facing`, `u8 flags` (1 grounded, 2 alive, 4 diving, 8 lying down after a dive, 16 shoving), `u8 layer`, `u8 move_ticks` (lying down: ticks until it is back on its feet) |
| 68 | TILES | reliable | `u32 tick`, `u8 layers`, `u8 ring`, `u16 bitmap length`, `bitmap`, `u8 n`, each tile: `u8 layer`, `i8 q`, `i8 r`, `u32 touch_tick` |
| 69 | TILE_EVENTS | reliable | `u32 tick`, `u8 n`, each tile: `u8 layer`, `i8 q`, `i8 r` |
| 70 | ELIMINATED | reliable | `u16 player_id`, `u8 place`, `u32 tick` |
| 71 | ERROR | reliable | `u8 code` (1 version mismatch, 2 room full, 3 kicked), `str text` |

- Phases: 0 lobby, 1 countdown, 2 playing, 3 result.
- Version 2 added the dive and shove buttons and the snapshot's move state. The envelope carries the version, so a client of another version is turned away.
- `phase_end_tick` in ROOM: in the countdown, the tick the round starts; in the result, the tick the lobby comes back; while playing, the tick the floor starts to give way (before it, nothing is touched); 0 otherwise.
- The server answers a PING in a packet of its own right away rather than at the next tick, so that `server_ms` is read and sent at the same moment and the round trip stays symmetric.
- `ack_input` in SNAPSHOT is the INPUT sequence number this client's player was simulated with on that tick; a predicting client replays its inputs after it.

## The arena

- Three floors, each a hexagon of radius 8 made of 217 hexes (`SIM_TILES`), addressed with axial coordinates `(q, r)`.
- **Canonical tile order:** q from -8 to 8, and for each q, r ascending over the tiles inside the arena. Tile 0 is `(-8, 0)` and tile 216 is `(8, 0)`. In JavaScript this is `tileOrder()`.
- The TILES bitmap holds floor 0's 217 tiles, then floor 1's, then floor 2's, one bit per tile (1 = gone), most significant bit of each byte first: 82 bytes for 651 tiles.
- A touched tile falls 24 ticks later (`SIM_FALL_TICKS`). TILE_EVENTS only says when a tile was touched; clients work out when it falls.
- Tile centres in world space: `x = 2·√3·(q + r/2)`, `z = 2·1.5·r`. Floors sit at heights 0, -12 and -24; below -33 is the slime, and reaching it means elimination.

## Reading snapshots in a browser

```js
import { messages, decode, MSG } from './protocol.js';

ws.binaryType = 'arraybuffer';
ws.onmessage = (e) => {
  for (const m of messages(new Uint8Array(e.data))) {
    const msg = decode(m.type, m.payload);
    if (m.type === MSG.SNAPSHOT) drawPlayers(msg.players);  // positions are already in world units
  }
};
```
