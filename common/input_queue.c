#include "input_queue.h"

#include <string.h>

void inputq_clear(inputq *iq)
{
    iq->len = 0;
    iq->current = (input_sample){0};
}

void inputq_add(inputq *iq, const msg_input *m)
{
    for (int i = m->count - 1; i >= 0; i--) {  /* oldest first */
        const input_sample *s = &m->s[i];
        if (iq->have_seq && (int16_t)(uint16_t)(s->seq - iq->newest) <= 0)
            continue;  /* a repeat for redundancy, or out of date */
        if (iq->len == INPUTQ_CAP) {
            memmove(iq->q, iq->q + 1, sizeof iq->q[0] * (INPUTQ_CAP - 1));
            iq->len--;
        }
        iq->q[iq->len++] = *s;
        iq->newest = s->seq;
        iq->have_seq = 1;
    }
}

sim_input inputq_next(inputq *iq)
{
    if (iq->len > INPUTQ_TARGET + 2) {
        int drop = iq->len - INPUTQ_TARGET;
        uint8_t pressed = 0;
        for (int i = 0; i < drop; i++)
            pressed |= iq->q[i].buttons;
        memmove(iq->q, iq->q + drop, sizeof iq->q[0] * (size_t)(iq->len - drop));
        iq->len -= drop;
        iq->q[0].buttons |= pressed;
    }
    if (iq->len > 0) {
        iq->current = iq->q[0];
        memmove(iq->q, iq->q + 1, sizeof iq->q[0] * (size_t)(iq->len - 1));
        iq->len--;
        iq->applied = iq->current.seq;
    } else {
        iq->current.buttons = 0;
    }
    return inputq_to_sim(&iq->current);
}

sim_input inputq_to_sim(const input_sample *s)
{
    return (sim_input){
        .mx = s->mx / 127.0f,
        .mz = s->mz / 127.0f,
        .jump = (s->buttons & BTN_JUMP) != 0,
        .dive = (s->buttons & BTN_DIVE) != 0,
        .shove = (s->buttons & BTN_SHOVE) != 0,
    };
}
