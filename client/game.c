#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interp.h"
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
 * Step 2: players are drawn a little in the past, between two snapshots
 * (interp.c). I switches between no smoothing, a fixed delay and an
 * adaptive one; N makes the network worse on purpose, to compare them. Our
 * own player still moves only when the server says so (step 3 changes that). */

#define BATCH 32
#define INPUT_PERIOD (1.0 / SIM_TICK_HZ)
#define TURN_PER_PIXEL 0.006f  /* camera turn for a mouse drag, radians */
#define TURN_PER_SECOND 2.2f   /* camera turn while Q or E is held */

/* Disturbances for received packets (net_set_inbound), cycled with N. */
static const netsim_cfg BAD_NETWORKS[] = {
    {.loss = 0, .delay_ms = 0, .jitter_ms = 0},
    {.loss = 0, .delay_ms = 20, .jitter_ms = 60},       /* 20-80 ms: everyday jitter */
    {.loss = 0.05, .delay_ms = 40, .jitter_ms = 120},  /* 40-160 ms and 5% lost: a bad hotspot */
};
#define BAD_NETWORK_COUNT ((int)(sizeof BAD_NETWORKS / sizeof BAD_NETWORKS[0]))

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

/* Keys are read relative to the camera, which looks along (-sin yaw, -cos
 * yaw), so W is always "away from the camera". The server wants world
 * directions, so the stick is turned by the camera's angle before it is
 * sent; what it means then no longer depends on the camera. */
static void send_input(net_thread *net, input_state *in, float yaw)
{
    float x = 0, z = 0;  /* x: right on screen, z: toward the camera */
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
    float c = cosf(yaw), s = sinf(yaw), wx = x * c + z * s, wz = z * c - x * s;

    memmove(&in->history[1], &in->history[0], sizeof in->history[0] * (HF_INPUT_REDUNDANCY - 1));
    in->history[0] = (input_sample){
        .seq = ++in->seq,
        .mx = (int8_t)lrintf(wx * 127),
        .mz = (int8_t)lrintf(wz * 127),
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

static int parse_mode(const char *s, interp_mode *out)
{
    for (int m = 0; m < INTERP_MODES; m++)
        if (!strcmp(s, interp_mode_name((interp_mode)m))) {
            *out = (interp_mode)m;
            return 1;
        }
    return 0;
}

/* DELAY:JITTER[:LOSS%], e.g. 40:120:5 */
static int parse_inbound(const char *s, netsim_cfg *out)
{
    int delay = 0, jitter = 0;
    double loss = 0;
    if (sscanf(s, "%d:%d:%lf", &delay, &jitter, &loss) < 2 || delay < 0 || jitter < 0 || loss < 0 || loss > 100)
        return 0;
    *out = (netsim_cfg){.loss = loss / 100, .delay_ms = delay, .jitter_ms = jitter};
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--host H] [--port P] [--name NAME]\n"
            "          [--interp none|fixed|adaptive] [--inbound DELAY:JITTER[:LOSS%%]]\n"
            "          [--auto-ready] [--screenshot FILE --after SEC]...   (unattended checks)\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *port = "8080";
    const char *name = getenv("USER") ? getenv("USER") : "player";
    int auto_ready = 0, shots = 0, shots_taken = 0, bad_network = 0;
    interp_mode mode = INTERP_ADAPTIVE;
    netsim_cfg inbound = BAD_NETWORKS[0];
    timed_shot shot[8];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && v)
            host = argv[++i];
        else if (!strcmp(a, "--port") && v)
            port = argv[++i];
        else if (!strcmp(a, "--name") && v)
            name = argv[++i];
        else if (!strcmp(a, "--interp") && v && parse_mode(v, &mode))
            i++;
        else if (!strcmp(a, "--inbound") && v && parse_inbound(v, &inbound))
            i++;
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
    net_set_inbound(net, inbound);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "Hex Fall");
    SetTargetFPS(60);
    render_init();

    static world w;
    static interp ip;
    world_init(&w);
    interp_init(&ip, mode);
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
        /* Each snapshot goes to both: the world keeps only the newest, the
         * interpolation keeps a history and also uses ones that came out of order. */
        while ((got = net_snapshots(net, &snap_cursor, batch, BATCH, NULL)) > 0)
            for (size_t i = 0; i < got; i++) {
                static world_snapshot s;
                if (!world_decode_snapshot(batch[i].data, batch[i].len, &s))
                    continue;
                world_apply_snapshot(&w, &s, batch[i].at_ms);
                interp_add(&ip, &s, batch[i].at_ms);
            }
        net_status st;
        net_get_status(net, &st);
        const world_player *me = world_player_by_id(&w, w.me);
        opt.ready = me && (me->flags & RP_READY);

        /* 2. Keyboard and mouse. Inputs go out at the server's tick rate, whatever the frame rate. */
        float dt = GetFrameTime();
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            opt.yaw -= GetMouseDelta().x * TURN_PER_PIXEL;
        if (IsKeyDown(KEY_Q))
            opt.yaw += TURN_PER_SECOND * dt;
        if (IsKeyDown(KEY_E))
            opt.yaw -= TURN_PER_SECOND * dt;
        if (IsKeyPressed(KEY_I))
            interp_set_mode(&ip, (interp_mode)((ip.mode + 1) % INTERP_MODES));
        if (IsKeyPressed(KEY_N)) {
            bad_network = (bad_network + 1) % BAD_NETWORK_COUNT;
            net_set_inbound(net, BAD_NETWORKS[bad_network]);
        }
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
        in.carry += dt;
        if (in.carry > 0.25)
            in.carry = INPUT_PERIOD;  /* after a long stall, do not fire a burst of stale inputs */
        while (in.carry >= INPUT_PERIOD) {
            if (st.state == NET_CONNECTED)
                send_input(net, &in, opt.yaw);
            in.carry -= INPUT_PERIOD;
        }

        /* 3. Draw, at the moment the interpolation picks. */
        interp_frame(&ip, now_ms);
        render_frame(&w, &ip, &st, &opt, now_ms);

        /* Screenshots for unattended checks; the last one ends the program. */
        if (shots_taken < shots && (double)(now_ms - start_ms) >= shot[shots_taken].after * 1000.0) {
            Image img = LoadImageFromScreen();
            ExportImage(img, shot[shots_taken++].file);
            UnloadImage(img);
            if (shots_taken == shots)
                break;
        }
    }

    hf_log("smoothing %s: delay %.0f ms, extrapolated %.1f%% of %llu frames (stopped %.1f%%), stutter %.2f ms, "
           "snapshots too late %llu of %llu",
           interp_mode_name(ip.mode), ip.delay_ms, ip.frames ? 100.0 * (double)ip.frames_extrapolated / (double)ip.frames : 0,
           (unsigned long long)ip.frames, ip.frames ? 100.0 * (double)ip.frames_stopped / (double)ip.frames : 0,
           ip.stutter_ms, (unsigned long long)ip.too_late, (unsigned long long)ip.snapshots);
    render_close();
    CloseWindow();
    net_stop(net);
    return 0;
}
