#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_thread.h"
#include "protocol.h"
#include "raylib.h"
#include "render.h"
#include "sim.h"
#include "util.h"
#include "world.h"

/* The desktop client's main thread: window, keyboard, game state, drawing.
 * Everything about the network happens on the network thread; this thread
 * only calls net_send() and drains the event and snapshot channels.
 *
 * Step 1: each snapshot is drawn as it arrives, with no smoothing, and our
 * own player moves only when the server says so. */

#define BATCH 32
#define INPUT_PERIOD (1.0 / SIM_TICK_HZ)

typedef struct {
    uint16_t seq;
    input_sample history[HF_INPUT_REDUNDANCY];  /* newest first */
    int count;
    int jump_pressed;  /* latched between input ticks so a quick tap is never missed */
    double carry;      /* time not yet turned into input ticks */
} input_state;

typedef struct {
    const char *file;
    double after;  /* seconds after start */
} timed_shot;

static mq_item batch[BATCH];

static void send_ready(net_thread *net, int ready)
{
    msg_ready m = {.ready = (uint8_t)ready};
    uint8_t buf[16];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_ready(&w, &m);
    net_send(net, buf, w.len);
}

/* The camera always looks toward -z, so "up" on the keyboard is -z in the world. */
static void send_input(net_thread *net, input_state *in)
{
    float x = 0, z = 0;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))
        z -= 1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))
        z += 1;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))
        x -= 1;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))
        x += 1;
    float len = sqrtf(x * x + z * z);
    if (len > 1) {
        x /= len;
        z /= len;
    }

    memmove(&in->history[1], &in->history[0], sizeof in->history[0] * (HF_INPUT_REDUNDANCY - 1));
    in->history[0] = (input_sample){
        .seq = ++in->seq,
        .mx = (int8_t)lrintf(x * 127),
        .mz = (int8_t)lrintf(z * 127),
        .buttons = in->jump_pressed ? BTN_JUMP : 0,
    };
    in->jump_pressed = 0;
    if (in->count < HF_INPUT_REDUNDANCY)
        in->count++;

    msg_input m = {.count = (uint8_t)in->count};
    memcpy(m.s, in->history, sizeof m.s);
    uint8_t buf[64];
    wbuf w;
    wb_init(&w, buf, sizeof buf);
    hf_put_input(&w, &m);
    net_send(net, buf, w.len);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--host H] [--port P] [--name NAME]\n"
            "          [--auto-ready] [--screenshot FILE --after SEC]...   (unattended checks)\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *port = "8080";
    const char *name = getenv("USER") ? getenv("USER") : "player";
    int auto_ready = 0, shots = 0, shots_taken = 0;
    timed_shot shot[8];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && v)
            host = argv[++i];
        else if (!strcmp(a, "--port") && v)
            port = argv[++i];
        else if (!strcmp(a, "--name") && v)
            name = argv[++i];
        else if (!strcmp(a, "--auto-ready"))
            auto_ready = 1;
        else if (!strcmp(a, "--screenshot") && v && shots < 8)
            shot[shots++] = (timed_shot){argv[++i], 0};
        else if (!strcmp(a, "--after") && v && shots > 0)
            shot[shots - 1].after = atof(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    net_config cfg = {.host = host, .port = port, .name = name, .kind = CLIENT_DESKTOP};
    net_thread *net = net_start(&cfg);
    if (!net) {
        fprintf(stderr, "cannot reach %s:%s\n", host, port);
        return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "Hex Fall");
    SetTargetFPS(60);
    render_init();

    static world w;
    world_init(&w);
    input_state in = {0};
    render_options opt = {.show_network = 1};
    uint64_t snap_cursor = 0, start_ms = hf_now_ms();
    int ready_sent_in_lobby = 0;

    while (!WindowShouldClose()) {
        uint64_t now_ms = hf_now_ms();

        /* 1. Everything the network thread received since the last frame. */
        size_t got;
        while ((got = net_events(net, batch, BATCH)) > 0)
            for (size_t i = 0; i < got; i++)
                world_apply_event(&w, batch[i].data, batch[i].len, now_ms);
        while ((got = net_snapshots(net, &snap_cursor, batch, BATCH, NULL)) > 0)
            for (size_t i = 0; i < got; i++)
                world_apply_snapshot(&w, batch[i].data, batch[i].len, batch[i].at_ms);
        net_status st;
        net_get_status(net, &st);
        const world_player *me = world_player_by_id(&w, w.me);
        opt.ready = me && (me->flags & RP_READY);

        /* 2. Keyboard. Inputs go out at the server's tick rate, whatever the frame rate. */
        if (IsKeyPressed(KEY_SPACE))
            in.jump_pressed = 1;
        if (IsKeyPressed(KEY_F1))
            opt.show_network = !opt.show_network;
        if (IsKeyPressed(KEY_C))
            opt.overview = !opt.overview;
        if (w.phase != PHASE_LOBBY)
            ready_sent_in_lobby = 0;
        if (w.welcomed && w.phase == PHASE_LOBBY) {
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                send_ready(net, !opt.ready);
            } else if (auto_ready && !opt.ready && !ready_sent_in_lobby) {
                send_ready(net, 1);
                ready_sent_in_lobby = 1;
            }
        }
        in.carry += GetFrameTime();
        if (in.carry > 0.25)
            in.carry = INPUT_PERIOD;  /* after a long stall, do not fire a burst of stale inputs */
        while (in.carry >= INPUT_PERIOD) {
            if (st.state == NET_CONNECTED)
                send_input(net, &in);
            in.carry -= INPUT_PERIOD;
        }

        /* 3. Draw. */
        render_frame(&w, &st, &opt, now_ms);

        /* Screenshots for unattended checks; the last one ends the program. */
        if (shots_taken < shots && (double)(now_ms - start_ms) >= shot[shots_taken].after * 1000.0) {
            Image img = LoadImageFromScreen();
            ExportImage(img, shot[shots_taken++].file);
            UnloadImage(img);
            if (shots_taken == shots)
                break;
        }
    }

    render_close();
    CloseWindow();
    net_stop(net);
    return 0;
}
