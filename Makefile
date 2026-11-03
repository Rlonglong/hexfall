# Build:  make          Test:  make test          Sanitizers:  make asan
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -Icommon -Iclient
LDLIBS  += -lm

COMMON  := common/protocol.c common/reliable.c common/sim.c common/util.c common/netsim.c
SERVER  := server/main.c server/udp.c server/room.c
BOT     := client/bot.c client/net_client.c
HEADERS := $(wildcard common/*.h server/*.h client/*.h)

all: build/hexfall-server build/hexfall-bot

build:
	mkdir -p build

build/hexfall-server: $(SERVER) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SERVER) $(COMMON) $(LDLIBS)

build/hexfall-bot: $(BOT) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(BOT) $(COMMON) $(LDLIBS)

build/test_protocol: tests/test_protocol.c $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_protocol.c $(COMMON) $(LDLIBS)

build/test_reliable: tests/test_reliable.c $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_reliable.c $(COMMON) $(LDLIBS)

build/golden: tests/golden.c $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/golden.c $(COMMON) $(LDLIBS)

# The last step checks that the browser's decoder (web/protocol.js) reads
# exactly what the C encoder writes.
test: build/test_protocol build/test_reliable build/golden
	./build/test_protocol
	./build/test_reliable
	./build/golden build/golden.bin
	node tests/test_protocol_js.mjs build/golden.bin

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra" all test

clean:
	rm -rf build

.PHONY: all test asan clean
