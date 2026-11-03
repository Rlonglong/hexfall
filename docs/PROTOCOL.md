# 協定規格（版本 1）

C 版在 `common/protocol.[ch]`，瀏覽器版在 `web/protocol.js`。`make test` 會檢查兩邊逐 byte 一致：C 寫出每種訊息，JS 讀回來逐欄位比對，再把 client 送的訊息用 JS 重新編碼，比對 byte。

## 規則

- 所有多 byte 欄位都是 **big-endian**（網路位元組順序）。JS 的 `DataView` 預設就是 big-endian。
- 不傳浮點數。位置和速度用 `int16`，單位是 1/100 格（`HF_POS_SCALE`）；方向用 `u16`，65536 = 一整圈；搖桿用 `int8`，-127 到 127。
- 字串：`u8` 長度加 UTF-8 bytes。名字最多 24 bytes，server 會去掉控制字元，切到一半的中文字也會去掉。
- 解碼時多出來的尾巴 bytes 會被忽略，所以新版本可以在訊息最後加欄位，舊 client 照樣能讀。

## 訊息

```
u8 type | u16 payload 長度 | payload
```

同樣的訊息可以放在 UDP 封包裡，也可以放在 WebSocket binary frame 裡。一個封包或一個 frame 可以裝好幾則訊息，一則接一則。

## UDP 信封（只有 UDP 才有）

```
u16 magic 0x4846 ("HF") | u8 協定版本 | u8 flags (0) | u64 session（WELCOME 之前是 0）
```

後面接可靠性層的標頭（10 bytes）：

```
u16 seq | u8 flags (bit0: 後面兩個確認欄位有效) | u16 ack | u32 ack_bits | u8 可靠訊息數 n
n × ( u16 可靠訊息 id | 訊息 )
其餘到封包結尾：不可靠訊息
```

- `ack` 是收到的最新封包序號；`ack_bits` 的第 i 個 bit 代表 `ack - 1 - i` 也收到了。
- 封包最大 1200 bytes（`HF_MAX_PACKET`），避免被路由器切片。
- WebSocket 沒有這一層，因為 TCP 已經保證送達和順序。

## Client → Server

| type | 名稱 | 通道 | payload |
|---|---|---|---|
| 1 | HELLO | 可靠 | `u16 版本`, `u8 種類`（0 電腦、1 手機、2 機器人）, `u64 resume_token`（新玩家填 0；斷線重連填舊的 token）, `str 名字` |
| 2 | READY | 可靠 | `u8 ready` |
| 3 | INPUT | 不可靠 | `u8 n`（最多 4，最新的在前），每筆：`u16 seq`, `i8 mx`, `i8 mz`, `u8 buttons`（bit0 跳：上一筆之後按下過跳） |
| 4 | PING | 不可靠 | `u32 client_ms` |
| 5 | BYE | 可靠 | `u8 reason` |

`mx`、`mz` 是世界座標的方向。client 要自己把「搖桿往上」換算成「鏡頭面對的方向」。

## Server → Client

| type | 名稱 | 通道 | payload |
|---|---|---|---|
| 64 | WELCOME | 可靠 | `u64 token`, `u16 player_id`, `u8 顏色`, `u8 tick_hz`, `u32 server_ms`, `u32 tick` |
| 65 | PONG | 不可靠 | `u32 client_ms`（原樣傳回）, `u32 server_ms` |
| 66 | ROOM | 可靠 | `u8 phase`, `u32 tick`, `u32 phase_end_tick`, `u8 n`，每位：`u16 id`, `u8 顏色`, `u8 flags`（1 已準備、2 還活著、4 已連線）, `u8 名次`（1 是冠軍，0 是還在場上或沒比）, `str 名字` |
| 67 | SNAPSHOT | 不可靠 | `u32 tick`, `u16 ack_input`, `u8 n`，每位：`u16 id`, `i16 x, y, z, vx, vy, vz`, `u16 facing`, `u8 flags`（1 站在地上、2 還活著）, `u8 layer` |
| 68 | TILES | 可靠 | `u32 tick`, `u8 層數`, `u8 半徑`, `u16 bitmap 長度`, `bitmap`, `u8 n`，每格：`u8 layer`, `i8 q`, `i8 r`, `u32 touch_tick` |
| 69 | TILE_EVENTS | 可靠 | `u32 tick`, `u8 n`，每格：`u8 layer`, `i8 q`, `i8 r` |
| 70 | ELIMINATED | 可靠 | `u16 player_id`, `u8 名次`, `u32 tick` |
| 71 | ERROR | 可靠 | `u8 code`（1 版本不符、2 房間滿了、3 被踢出）, `str 說明` |

- phase：0 大廳、1 倒數、2 比賽中、3 結算。
- SNAPSHOT 的 `ack_input` 是「這位玩家上一輪用到的 INPUT 流水號」，client 預測時從它之後重算。

## 地圖

- 3 層，每層是半徑 8 的六角形，共 217 格（`SIM_TILES`），用 axial 座標 `(q, r)`。
- **格子的標準順序**：q 從 -8 到 8，同一個 q 之下 r 由小到大（只算地圖範圍內的）。第 0 格是 `(-8, 0)`，第 216 格是 `(8, 0)`。JS 版是 `tileOrder()`。
- TILES 的 bitmap：第 0 層的 217 格，接著第 1 層、第 2 層，每格 1 bit（1 = 已經掉了），每個 byte 從最高位開始。651 格需要 82 bytes。
- 格子被踩到後再過 16 輪（`SIM_FALL_TICKS`）就掉落。TILE_EVENTS 只告訴你什麼時候被踩，掉落時間自己算。
- 格子中心的世界座標：`x = 2 × √3 × (q + r/2)`、`z = 2 × 1.5 × r`。各層高度分別是 0、-12、-24；掉到 -33 以下就是掉進史萊姆。

## 手機端讀快照的範例

```js
import { messages, decode, MSG } from './protocol.js';

ws.binaryType = 'arraybuffer';
ws.onmessage = (e) => {
  for (const m of messages(new Uint8Array(e.data))) {
    const msg = decode(m.type, m.payload);
    if (m.type === MSG.SNAPSHOT) drawPlayers(msg.players);  // x、y、z 已經換回世界單位
  }
};
```
