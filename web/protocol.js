// The browser side of the game protocol: the same messages as
// common/protocol.h, byte for byte. Phones send these inside WebSocket binary
// frames instead of UDP packets; nothing else about a message changes.
//
// Every multi-byte field is big-endian, which is DataView's default, and
// 64-bit fields are BigInt. `make test` checks this file against the C encoder.

export const PROTOCOL_VERSION = 2;
export const MAX_PLAYERS = 32;
export const NAME_MAX = 24;
export const INPUT_REDUNDANCY = 4;
export const POS_SCALE = 100;
export const MAX_MSG = 1100;

export const MSG = Object.freeze({
  HELLO: 1, READY: 2, INPUT: 3, PING: 4, BYE: 5,
  WELCOME: 64, PONG: 65, ROOM: 66, SNAPSHOT: 67, TILES: 68, TILE_EVENTS: 69, ELIMINATED: 70, ERROR: 71,
});
const UNRELIABLE = new Set([MSG.INPUT, MSG.PING, MSG.PONG, MSG.SNAPSHOT]);
export const isReliable = (type) => !UNRELIABLE.has(type);

export const CLIENT_KIND = Object.freeze({ DESKTOP: 0, PHONE: 1, BOT: 2 });
export const PHASE = Object.freeze({ LOBBY: 0, COUNTDOWN: 1, PLAYING: 2, RESULT: 3 });
export const ERROR_CODE = Object.freeze({ VERSION: 1, FULL: 2, KICKED: 3 });
export const BTN = Object.freeze({ JUMP: 1, DIVE: 2, SHOVE: 4 });
export const SNAP_FLAG = Object.freeze({ GROUNDED: 1, ALIVE: 2, DIVING: 4, DOWN: 8, SHOVING: 16 });
export const RP = Object.freeze({ READY: 1, ALIVE: 2, CONNECTED: 4 });
export const SP = Object.freeze({ GROUNDED: 1, ALIVE: 2 });

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// Keeps at most `max` bytes without splitting a UTF-8 character.
function cutUtf8(bytes, max) {
  if (bytes.length <= max) return bytes;
  let n = max;
  while (n > 0 && (bytes[n] & 0xc0) === 0x80) n--;
  return bytes.subarray(0, n);
}

export class Writer {
  constructor(capacity = MAX_MSG) {
    this.buf = new Uint8Array(capacity);
    this.view = new DataView(this.buf.buffer);
    this.len = 0;
  }
  #need(n) {
    if (this.len + n > this.buf.length) throw new RangeError('message too large');
  }
  u8(v) { this.#need(1); this.view.setUint8(this.len, v); this.len += 1; return this; }
  i8(v) { this.#need(1); this.view.setInt8(this.len, v); this.len += 1; return this; }
  u16(v) { this.#need(2); this.view.setUint16(this.len, v); this.len += 2; return this; }
  i16(v) { this.#need(2); this.view.setInt16(this.len, v); this.len += 2; return this; }
  u32(v) { this.#need(4); this.view.setUint32(this.len, v >>> 0); this.len += 4; return this; }
  u64(v) { this.#need(8); this.view.setBigUint64(this.len, BigInt.asUintN(64, BigInt(v))); this.len += 8; return this; }
  bytes(b) { this.#need(b.length); this.buf.set(b, this.len); this.len += b.length; return this; }
  str(s) {
    const b = cutUtf8(encoder.encode(s), 255);
    return this.u8(b.length).bytes(b);
  }
  // A message is u8 type, u16 payload length, payload.
  begin(type) { this.u8(type); const at = this.len; this.u16(0); return at; }
  end(at) { this.view.setUint16(at, this.len - at - 2); return this; }
  result() { return this.buf.slice(0, this.len); }
}

export class Reader {
  constructor(bytes, offset = 0, length = bytes.length - offset) {
    this.src = bytes; // not `bytes`: that name is the method below
    this.offset = offset;
    this.view = new DataView(bytes.buffer, bytes.byteOffset + offset, length);
    this.pos = 0;
    this.end = length;
  }
  get left() { return this.end - this.pos; }
  #need(n) {
    if (this.pos + n > this.end) throw new RangeError('truncated message');
  }
  u8() { this.#need(1); return this.view.getUint8(this.pos++); }
  i8() { this.#need(1); return this.view.getInt8(this.pos++); }
  u16() { this.#need(2); const v = this.view.getUint16(this.pos); this.pos += 2; return v; }
  i16() { this.#need(2); const v = this.view.getInt16(this.pos); this.pos += 2; return v; }
  u32() { this.#need(4); const v = this.view.getUint32(this.pos); this.pos += 4; return v; }
  u64() { this.#need(8); const v = this.view.getBigUint64(this.pos); this.pos += 8; return v; }
  bytes(n) {
    this.#need(n);
    const b = this.src.subarray(this.offset + this.pos, this.offset + this.pos + n);
    this.pos += n;
    return b;
  }
  str() { return decoder.decode(this.bytes(this.u8())); }
  sub(n) {
    this.#need(n);
    const r = new Reader(this.src, this.offset + this.pos, n);
    this.pos += n;
    return r;
  }
}

// Splits a buffer (one WebSocket frame, say) into messages.
export function* messages(bytes) {
  const r = new Reader(bytes);
  while (r.left > 0) {
    const start = r.pos;
    const type = r.u8();
    const payload = r.sub(r.u16());
    yield { type, payload, raw: bytes.subarray(start, r.pos) };
  }
}

// ---- client -> server ----

export function encodeHello({ kind = CLIENT_KIND.PHONE, resumeToken = 0n, name = '' } = {}) {
  const w = new Writer();
  const at = w.begin(MSG.HELLO);
  w.u16(PROTOCOL_VERSION).u8(kind).u64(resumeToken).str(name);
  return w.end(at).result();
}

export function encodeReady(ready = true) {
  const w = new Writer();
  const at = w.begin(MSG.READY);
  w.u8(ready ? 1 : 0);
  return w.end(at).result();
}

// samples: newest first, [{ seq, mx, mz, buttons }], mx/mz in -127..127 (world axes).
export function encodeInput(samples) {
  const w = new Writer();
  const at = w.begin(MSG.INPUT);
  const list = samples.slice(0, INPUT_REDUNDANCY);
  w.u8(list.length);
  for (const s of list) w.u16(s.seq & 0xffff).i8(s.mx).i8(s.mz).u8(s.buttons);
  return w.end(at).result();
}

export function encodePing(clientMs) {
  const w = new Writer();
  const at = w.begin(MSG.PING);
  w.u32(clientMs);
  return w.end(at).result();
}

export function encodeBye(reason = 0) {
  const w = new Writer();
  const at = w.begin(MSG.BYE);
  w.u8(reason);
  return w.end(at).result();
}

// ---- decoding (both directions) ----

const turns = (u16) => (u16 / 65536) * 2 * Math.PI;

function tileRef(r) {
  return { layer: r.u8(), q: r.i8(), r: r.i8() };
}

export function decode(type, r) {
  switch (type) {
    case MSG.HELLO:
      return { version: r.u16(), kind: r.u8(), resumeToken: r.u64(), name: r.str() };
    case MSG.READY:
      return { ready: r.u8() };
    case MSG.INPUT: {
      const samples = [];
      for (let n = r.u8(); n > 0; n--) samples.push({ seq: r.u16(), mx: r.i8(), mz: r.i8(), buttons: r.u8() });
      return { samples };
    }
    case MSG.PING:
      return { clientMs: r.u32() };
    case MSG.BYE:
      return { reason: r.u8() };
    case MSG.WELCOME:
      return { token: r.u64(), playerId: r.u16(), color: r.u8(), tickHz: r.u8(), serverMs: r.u32(), tick: r.u32() };
    case MSG.PONG:
      return { clientMs: r.u32(), serverMs: r.u32() };
    case MSG.ROOM: {
      const room = { phase: r.u8(), tick: r.u32(), phaseEndTick: r.u32(), players: [] };
      for (let n = r.u8(); n > 0; n--)
        room.players.push({ id: r.u16(), color: r.u8(), flags: r.u8(), place: r.u8(), name: r.str() });
      return room;
    }
    case MSG.SNAPSHOT: {
      const snap = { tick: r.u32(), ackInput: r.u16(), players: [] };
      for (let n = r.u8(); n > 0; n--) {
        snap.players.push({
          id: r.u16(),
          x: r.i16() / POS_SCALE, y: r.i16() / POS_SCALE, z: r.i16() / POS_SCALE,
          vx: r.i16() / POS_SCALE, vy: r.i16() / POS_SCALE, vz: r.i16() / POS_SCALE,
          facing: turns(r.u16()),
          flags: r.u8(),
          layer: r.u8(),
          moveTicks: r.u8(),
        });
      }
      return snap;
    }
    case MSG.TILES: {
      const tiles = { tick: r.u32(), layers: r.u8(), ring: r.u8() };
      tiles.gone = r.bytes(r.u16()).slice();
      tiles.touched = [];
      for (let n = r.u8(); n > 0; n--) tiles.touched.push({ ...tileRef(r), touchTick: r.u32() });
      return tiles;
    }
    case MSG.TILE_EVENTS: {
      const ev = { tick: r.u32(), tiles: [] };
      for (let n = r.u8(); n > 0; n--) ev.tiles.push(tileRef(r));
      return ev;
    }
    case MSG.ELIMINATED:
      return { playerId: r.u16(), place: r.u8(), tick: r.u32() };
    case MSG.ERROR:
      return { code: r.u8(), text: r.str() };
    default:
      return null; // a type this version does not know: skip it
  }
}

// ---- the board ----

// Canonical tile order, as in common/sim.h: q ascending, then r ascending.
export function tileOrder(ring = 8) {
  const order = [];
  for (let q = -ring; q <= ring; q++) {
    const r0 = Math.max(-ring, -q - ring);
    const r1 = Math.min(ring, -q + ring);
    for (let r = r0; r <= r1; r++) order.push([q, r]);
  }
  return order;
}

// Bit i of the TILES bitmap, most significant bit of each byte first.
export const isGone = (gone, i) => (gone[i >> 3] & (0x80 >> (i & 7))) !== 0;
