#ifndef HF_RENDER_H
#define HF_RENDER_H

#include <stdint.h>

#include "net_thread.h"
#include "world.h"

/* Drawing with raylib. Reads the world and the network status; changes
 * neither, and never touches the network. What it keeps for itself (camera,
 * particles, how far each bean has turned) is only for looks. */

typedef struct {
    int show_network;  /* the network panel */
    int overview;      /* watch the whole floor instead of following our bean */
    int ready;         /* our own ready flag as the server last reported it */
} render_options;

void render_init(void);  /* after InitWindow() */
void render_frame(const world *w, const net_status *st, const render_options *opt, uint64_t now_ms);
void render_close(void);  /* before CloseWindow() */

#endif
