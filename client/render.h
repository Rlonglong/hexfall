#ifndef HF_RENDER_H
#define HF_RENDER_H

#include <stdint.h>

#include "interp.h"
#include "net_thread.h"
#include "predict.h"
#include "world.h"

/* Drawing with raylib. Reads the world (room, board, notices), the
 * interpolation's view (where each player is, and at which moment), the
 * prediction (where our own player is) and the network status; changes
 * none of them, and never touches the network.
 * What it keeps for itself (camera, particles, how far each bean has
 * turned) is only for looks. */

typedef struct {
    int show_network;  /* the network panel */
    int overview;      /* watch the whole floor instead of following our bean */
    int ready;         /* our own ready flag as the server last reported it */
    float yaw;         /* the camera's angle around the vertical: 0 looks from +z toward -z */
    float pitch;       /* how steeply the chase camera looks down, radians */
    int mouse_look;    /* the mouse is held by the window and turns the camera */
} render_options;

void render_init(void);  /* after InitWindow() */
void render_frame(const world *w, const interp *ip, const predict *pr, const net_status *st,
                  const render_options *opt, uint64_t now_ms);
void render_close(void);  /* before CloseWindow() */

#endif
