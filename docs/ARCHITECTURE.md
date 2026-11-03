# 架構

## 原則

1. **server 說了算**：誰在哪裡、哪一格掉了、誰被淘汰，全部由 server 決定。client 只送「操作」，只畫 server 告訴它的東西。
2. **訊息格式只有一套**：電腦走 UDP、手機走 WebSocket，但訊息的每一個 byte 都一樣。不一樣的只有最外層的「信封」。
3. **傳輸方式可以抽換**：遊戲邏輯（`server/room.c`）只呼叫 `conn_send()`，完全不知道底下是 UDP 還是 WebSocket。

## 分層

```
 電腦 client / 機器人（C）                手機（瀏覽器 JS，第二階段）
          │                                        │
 ┌────────┴─────────────┐              ┌───────────┴───────────┐
 │ UDP 信封（12 bytes）  │              │ WebSocket 封包（TCP） │
 │ 可靠性層 reliable.c   │              │ 自己刻，不用函式庫    │
 └────────┬─────────────┘              └───────────┬───────────┘
          └──────── 同一套訊息：type | length | payload ────────┘
                                   │
                    conn_send() ── room.c（遊戲、狀態機）── sim.c（物理）
```

## 兩種訊息：可靠和不可靠

| 類別 | 訊息 | UDP 上怎麼送 | WebSocket 上怎麼送 |
|---|---|---|---|
| 可靠、照順序 | HELLO、READY、BYE、WELCOME、ROOM、TILES、TILE_EVENTS、ELIMINATED、ERROR | 可靠性層：沒收到確認就重送，接收端照順序交出 | 直接送（TCP 本來就可靠、有順序） |
| 不可靠、新的蓋舊的 | INPUT、PING、PONG、SNAPSHOT | 只送一次，掉了就算了 | 直接送；如果這條連線塞住了，就丟掉舊的快照不送 |

訊息屬於哪一類由型別決定（`hf_msg_is_reliable()`），兩種傳輸方式共用同一張表。

## UDP 可靠性層（common/reliable.c）

這就是課本 Ch.22「替 UDP 加上可靠性」的做法：

- **每個封包都有序號**，同時帶著「我收到的最新序號」加上「它前面 32 個收到了哪些」（32 bit）。所以一個封包可以一次確認對方 33 個封包，確認封包掉了也沒關係。
- **可靠訊息**放進封包送出，直到某個裝著它的封包被確認為止；逾時就重送。逾時時間的算法跟 TCP 一樣：`RTO = SRTT + 4 × RTTVAR`。
- **每個封包都有自己的送出時間**，就算裡面裝的是重送的訊息，量到的 RTT 也不會搞混（Karn 演算法要解決的問題）。
- **接收端用 128 格的視窗**把可靠訊息排好順序再交出，重複的直接丟掉。
- **不可靠訊息**（快照、輸入）不重送：下一個快照 33 ms 後就到了，重送舊的沒有意義。這正好避開 TCP「掉一個、後面全部排隊等」的問題。

測試（`tests/test_reliable.c`）用模擬的爛網路驗證：掉 30% 封包、延遲 20 到 120 ms、5% 重複，1900 則可靠訊息全部照順序、剛好收到一次。

## 連線和斷線重連

- **握手**：client 第一次送的封包，session 欄位是 0，裡面裝 HELLO。server 先用「來源位址」對應這條連線，回 WELCOME，裡面有一組 64-bit 隨機 token。之後 client 每個封包都帶這組 token。
- **換網路也不怕**：server 用 token 認人，不靠 IP。手機從 Wi-Fi 換到 4G、或玩家的路由器換了對外 port（NAT 重新綁定），server 看到 token 對、位址變了，就直接更新位址。
- **斷線**：4 秒沒收到任何封包，就判定連線中斷。玩家的位子保留 15 秒，這段時間內用 HELLO 帶舊 token 回來，就接回原本的角色。
- **心跳**：雙方每一輪（33 ms）都會送一個封包，裡面至少有確認資訊，所以不需要另外的心跳訊息。這也讓玩家那端的 NAT 對應不會過期。

## 伺服器主迴圈（server/main.c）

單一 thread、單一迴圈：

```
loop:
    poll(UDP socket, 等到下一輪該開始的時間)
    有封包 → udp_receive()：拆信封 → 可靠性層 → room_on_message()
    時間到了 → room_tick()：跑一輪遊戲 → udp_flush()：每條連線送出封包
    udp_maintain()：逾時、關閉
```

遊戲每秒跑 30 輪（`SIM_TICK_HZ`）。落後太多時會跳過幾輪，不會越追越慢。

## 房間狀態機（server/room.c）

```
LOBBY ──人數夠、全部準備好──> COUNTDOWN ──3 秒──> PLAYING ──剩一人──> RESULT ──5 秒──> LOBBY
  ^                              │
  └────有人離開或取消準備──────────┘
```

玩家狀態：已連線、斷線保留中（15 秒）、已移除；比賽中另外分成「還在場上」和「已淘汰（名次）」。

## 輸入（為之後的預測和校正鋪路）

- client 每一輪送一個 INPUT，每筆輸入有流水號，並且重複附上最新的 4 筆，掉一個封包不會少操作。
- server 把輸入排隊，每一輪剛好用掉一筆。排太多（網路卡住後一次湧進來）就跳過舊的，但保留跳躍。
- 每個快照都會告訴 client「你的輸入我用到第幾號」（`ack_input`）。第二階段 client 做預測時，就從這一號之後的輸入重算一次。

## 地板

- 被踩到的格子，server 只廣播一次「第幾層的哪一格在第幾輪被踩」（TILE_EVENTS）。
- 掉落時間是固定的：被踩後 16 輪（約 0.53 秒）。每個 client 都能自己算出它什麼時候掉，不用每一輪傳整張地圖。
- 剛加入、或同一輪被踩太多格時，改送整張地圖（TILES：每格 1 bit，651 格只要 82 bytes）。

## 檔案

| 檔案 | 內容 |
|---|---|
| `common/wire.h` | 讀寫 big-endian 的工具，跑超過緩衝區只會設旗標，不會越界 |
| `common/protocol.[ch]` | 訊息定義、編碼、解碼 |
| `common/reliable.[ch]` | UDP 可靠性層 |
| `common/sim.[ch]` | 地圖和物理，server 跟 client 共用 |
| `common/netsim.[ch]` | 故意掉封包、加延遲，用來測試和 demo |
| `server/udp.c` | UDP 傳輸：socket、連線表、握手、逾時 |
| `server/room.c` | 遊戲：大廳、回合、淘汰 |
| `client/net_client.[ch]` | 電腦端連線核心，機器人和之後的電腦版遊戲共用 |
| `client/bot.c` | 機器人，也是壓力測試工具 |
| `web/protocol.js` | 手機端的訊息編解碼，跟 C 版逐 byte 一致 |

## 第二階段：加上 WebSocket（手機）要做的事

1. `server/ws.c`：在同一個 `poll()` 裡多開一個 TCP listen socket。
2. 每條 TCP 連線要有自己的接收緩衝和傳送緩衝：TCP 是位元組串流，一次 `recv()` 可能只收到半個封包，也可能收到好幾個。
3. 最小 HTTP：只處理 `GET`（送網頁和 JS 檔）和 WebSocket 升級握手（`Sec-WebSocket-Accept` 要用 SHA-1 和 Base64）。
4. WebSocket 封包：三種長度表示法、client 送來的資料要解遮罩、ping、pong、close。
5. `conn_send()` 加上 `TRANSPORT_WEBSOCKET` 這個分支：每則訊息包成一個 binary frame 送出；傳送緩衝塞住時丟掉不可靠的訊息。
6. HELLO、WELCOME 這些訊息完全不用改，`room.c` 一行都不用動。

## 接下來（第一階段剩下的）

- [ ] 電腦版遊戲 client（raylib）：用 `net_client` 連線，畫 3D 地板和豆子
- [ ] client 預測自己的角色，收到快照後依 `ack_input` 校正
- [ ] 別人的角色用內插顯示
- [ ] 對時（PING/PONG 已經有了），延遲補償判定跳躍
- [ ] 機器人支援斷線重連測試（帶 resume token 重新 HELLO）
- [ ] 監控面板：每條連線的 RTT、重送數、掉封包率
