# Hex Fall（蜂巢墜落）

網路程式設計期末專題，第 19 組。多人即時淘汰賽：踩過的六角地板會掉落，最後站著的人獲勝。

- 第一階段（現在）：server 和電腦 client 用 C 寫，走 UDP，自己做可靠性。
- 第二階段：手機用瀏覽器加入，server 自己實作 WebSocket。訊息格式從第一天就跟手機共用。

架構說明在 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，協定規格在 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

## 建置與測試

需要 C 編譯器（macOS 的 clang 或 Linux 的 gcc），以及 Node.js（只有 `make test` 的最後一步需要）。

```bash
make
```

```bash
make test
```

```bash
make asan
```

`make asan` 會打開 AddressSanitizer 和 UndefinedBehaviorSanitizer 重新建置，再跑一次全部測試，用來抓手寫解析程式的越界讀寫。

## 執行

開 server（UDP 8080 port，至少 2 人才開賽）：

```bash
./build/hexfall-server --port 8080 --min-players 2
```

放 10 個機器人進去玩：

```bash
./build/hexfall-bot --port 8080 -n 10
```

模擬很差的網路（server 和機器人都可以加，同時套用就是雙向）：

```bash
./build/hexfall-server --port 8080 --loss 10 --delay 40 --jitter 40
```

從別台電腦連：`--host 你的IP或網域`。從宿舍外面連進來，路由器要另外開 **UDP** 8080 的轉發（之前設的 80、8080 是 TCP）。
