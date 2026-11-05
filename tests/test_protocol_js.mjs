// Decodes the messages the C encoder wrote (tests/golden.c) with the browser
// decoder (web/protocol.js), field by field, and re-encodes the client
// messages to compare byte by byte. If this passes, a phone and the server
// agree on every message.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import {
  MSG, POS_SCALE, decode, encodeBye, encodeHello, encodeInput, encodePing, encodeReady, isGone, messages, tileOrder,
} from '../web/protocol.js';

const file = process.argv[2] ?? 'build/golden.bin';
const bytes = new Uint8Array(readFileSync(file));

const gone = new Uint8Array(82);
gone[0] = 0x80;
gone[81] = 0x20;

const expected = [
  {
    type: MSG.HELLO,
    value: { version: 2, kind: 1, resumeToken: 0x0123456789abcdefn, name: '小明' },
    encode: () => encodeHello({ kind: 1, resumeToken: 0x0123456789abcdefn, name: '小明' }),
  },
  { type: MSG.READY, value: { ready: 1 }, encode: () => encodeReady(true) },
  {
    type: MSG.INPUT,
    value: { samples: [{ seq: 65535, mx: -127, mz: 64, buttons: 1 }, { seq: 65534, mx: 0, mz: -1, buttons: 6 }] },
    encode: () => encodeInput([{ seq: 65535, mx: -127, mz: 64, buttons: 1 }, { seq: 65534, mx: 0, mz: -1, buttons: 6 }]),
  },
  { type: MSG.PING, value: { clientMs: 4000000000 }, encode: () => encodePing(4000000000) },
  { type: MSG.BYE, value: { reason: 0 }, encode: () => encodeBye(0) },
  {
    type: MSG.WELCOME,
    value: { token: 0xfedcba9876543210n, playerId: 7, color: 3, tickHz: 30, serverMs: 123456, tick: 789 },
  },
  { type: MSG.PONG, value: { clientMs: 1, serverMs: 4294967295 } },
  {
    type: MSG.ROOM,
    value: {
      phase: 2, tick: 1000, phaseEndTick: 0,
      players: [
        { id: 7, color: 3, flags: 7, place: 0, name: '小明' },
        { id: 9, color: 1, flags: 4, place: 2, name: 'Bot-01' },
      ],
    },
  },
  {
    type: MSG.SNAPSHOT,
    value: {
      tick: 1001, ackInput: 65535,
      players: [{
        id: 7, x: -1234 / POS_SCALE, y: 0, z: 2500 / POS_SCALE, vx: 800 / POS_SCALE, vy: -1300 / POS_SCALE, vz: 0,
        facing: (49152 / 65536) * 2 * Math.PI, flags: 11, layer: 1, moveTicks: 7,
      }],
    },
  },
  { type: MSG.TILE_EVENTS, value: { tick: 1002, tiles: [{ layer: 0, q: -8, r: 8 }, { layer: 2, q: 0, r: 0 }] } },
  {
    type: MSG.TILES,
    value: { tick: 1003, layers: 3, ring: 8, gone, touched: [{ layer: 1, q: 3, r: -2, touchTick: 1000 }] },
  },
  { type: MSG.ELIMINATED, value: { playerId: 9, place: 2, tick: 1004 } },
  { type: MSG.ERROR, value: { code: 2, text: 'room is full' } },
];

const got = [...messages(bytes)];
assert.equal(got.length, expected.length, 'message count');
got.forEach((m, i) => {
  const want = expected[i];
  assert.equal(m.type, want.type, `message ${i} type`);
  assert.deepEqual(decode(m.type, m.payload), want.value, `message ${i} fields`);
  assert.equal(m.payload.left, 0, `message ${i} has unread bytes`);
  if (want.encode) assert.deepEqual(want.encode(), m.raw.slice(), `message ${i} bytes differ from C`);
});

const order = tileOrder(8);
assert.equal(order.length, 217);
assert.deepEqual(order[0], [-8, 0]);
assert.deepEqual(order[216], [8, 0]);
assert.ok(isGone(gone, 0) && isGone(gone, 650) && !isGone(gone, 1));

console.log(`protocol.js: agrees with the C encoder on all ${got.length} messages`);
