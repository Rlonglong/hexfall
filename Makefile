# Build:  make        Test:  make test        Stress the network thread:  make stress
# Sanitizers:  make asan (memory)  /  make tsan (threads)
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -Icommon -Iclient
LDLIBS  += -lm -lpthread

COMMON  := common/protocol.c common/reliable.c common/sim.c common/util.c common/netsim.c
NETCLI  := client/net_client.c
NETTHR  := client/net_thread.c client/msg_queue.c client/clock_sync.c $(NETCLI)
WORLD   := client/world.c client/interp.c
SERVER  := server/main.c server/udp.c server/room.c
HEADERS := $(wildcard common/*.h server/*.h client/*.h)
TESTS   := build/test_protocol build/test_reliable build/test_msg_queue build/test_clock_sync build/test_world build/test_interp build/golden

# The desktop client needs raylib (brew install raylib / apt install libraylib-dev);
# everything else builds without it.
ifeq ($(shell pkg-config --exists raylib 2>/dev/null && echo yes),yes)
CLIENT  := build/hexfall-client
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib)
endif

all: build/hexfall-server build/hexfall-bot build/hexfall-netprobe $(CLIENT)

build:
	mkdir -p build

build/hexfall-server: $(SERVER) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SERVER) $(COMMON) $(LDLIBS)

build/hexfall-bot: client/bot.c $(NETCLI) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ client/bot.c $(NETCLI) $(COMMON) $(LDLIBS)

build/hexfall-netprobe: client/netprobe.c $(NETTHR) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ client/netprobe.c $(NETTHR) $(COMMON) $(LDLIBS)

build/hexfall-client: client/game.c client/render.c $(WORLD) $(NETTHR) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(RAYLIB_CFLAGS) $(CFLAGS) -o $@ client/game.c client/render.c $(WORLD) $(NETTHR) $(COMMON) $(RAYLIB_LIBS) $(LDLIBS)

build/test_%: tests/test_%.c $(WORLD) $(NETTHR) $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(WORLD) $(NETTHR) $(COMMON) $(LDLIBS)

build/golden: tests/golden.c $(COMMON) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/golden.c $(COMMON) $(LDLIBS)

# The last step checks that the browser's decoder (web/protocol.js) reads
# exactly what the C encoder writes.
test: $(TESTS)
	./build/test_protocol
	./build/test_reliable
	./build/test_msg_queue
	./build/test_clock_sync
	./build/test_world
	./build/test_interp
	./build/golden build/golden.bin
	node tests/test_protocol_js.mjs build/golden.bin

# A server, bots, and network-thread probes that stall to force resyncs.
stress: all
	sh tests/stress_net.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra" all test

tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -O1 -g -fsanitize=thread -Wall -Wextra" all build/test_msg_queue
	./build/test_msg_queue
	sh tests/stress_net.sh

clean:
	rm -rf build

.PHONY: all test stress asan tsan clean
