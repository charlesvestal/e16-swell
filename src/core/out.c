/*
 * Swell core: live values -> MIDI bytes (out.h has the contract).
 *
 * Two stages per service call:
 *   1. Latch: every dirty control / toggle is mapped to output units. If
 *      that differs from sent[] (the latest value owed or sent), it
 *      becomes the new sent[] and every port in the mask is owed it again
 *      (pend_*); if it's the same, nothing changes. The engine's dirty
 *      flags are consumed here: from now on pend_* carries the debt, and
 *      a value that changes again before it got out simply replaces the
 *      one owed, so nothing stale is ever queued.
 *   2. Send: items (controls 0..31, then toggles 0..15) are visited
 *      round-robin from rr. A port that refuses (no budget, or write()
 *      says full) is skipped for the rest of the call, so the next call
 *      starts with the first item still owed something; that's what
 *      keeps a 32-control fade on a slow TRS port fair.
 */
#include "out.h"

#define ITEMS (SW_CONTROLS + SW_TOGGLES)
#define NEVER16 0xFFFFu
#define NEVER8 0xFFu

uint16_t sw_out_value(const sw_control_t *c, uint16_t pos)
{
    if (pos > SW_VMAX)
        pos = SW_VMAX;
    int32_t d = (int32_t)c->max - (int32_t)c->min;
    int32_t n = d * (int32_t)pos; /* |d| <= 16383: fits in 28 bits */
    /* Rounded to nearest; SW_VMAX is odd, so there are no ties. */
    int32_t r = n >= 0 ? (n + (int32_t)(SW_VMAX / 2)) / (int32_t)SW_VMAX
                       : -((-n + (int32_t)(SW_VMAX / 2)) / (int32_t)SW_VMAX);
    return (uint16_t)((int32_t)c->min + r);
}

static int ctl_active(const sw_control_t *c)
{
    return c->type != SW_T_OFF && c->type < SW_T_COUNT && (c->ports & SW_PORT_ALL);
}

static uint8_t *cc(uint8_t *p, uint8_t ch, uint8_t num, uint8_t val)
{
    p[0] = (uint8_t)(0xB0 | (ch & 15));
    p[1] = num & 127;
    p[2] = val & 127;
    return p + 3;
}

/* Control c's message for output value v; returns its length. */
static int ctl_msg(const sw_control_t *c, uint16_t v, uint8_t *buf)
{
    uint8_t *p = buf, ch = c->channel;
    uint16_t num = c->number;
    switch (c->type) {
    case SW_T_CC7:
        p = cc(p, ch, (uint8_t)num, (uint8_t)v);
        break;
    case SW_T_CC14:
        p = cc(p, ch, (uint8_t)num, (uint8_t)(v >> 7));
        p = cc(p, ch, (uint8_t)(num + 32), (uint8_t)(v & 127));
        break;
    case SW_T_NRPN7:
    case SW_T_NRPN14:
        p = cc(p, ch, 99, (uint8_t)(num >> 7));
        p = cc(p, ch, 98, (uint8_t)(num & 127));
        if (c->type == SW_T_NRPN7) {
            p = cc(p, ch, 6, (uint8_t)v);
        } else {
            p = cc(p, ch, 6, (uint8_t)(v >> 7));
            p = cc(p, ch, 38, (uint8_t)(v & 127));
        }
        break;
    default:
        break;
    }
    return (int)(p - buf);
}

void sw_out_init(sw_out_t *o, uint32_t now)
{
    uint8_t *b = (uint8_t *)o;
    for (uint32_t i = 0; i < sizeof *o; i++)
        b[i] = 0;
    o->last_us = now;
    for (int p = 0; p < SW_OUT_PORTS; p++)
        o->budget[p] = SW_OUT_BURST;
    sw_out_forget(o);
}

void sw_out_forget(sw_out_t *o)
{
    for (int c = 0; c < SW_CONTROLS; c++)
        o->sent[c] = NEVER16;
    for (int t = 0; t < SW_TOGGLES; t++)
        o->sent_tog[t] = NEVER8;
}

static void refill(sw_out_t *o, uint32_t now)
{
    uint32_t dt = now - o->last_us;
    o->last_us = now;
    for (int p = 0; p < SW_OUT_PORTS; p++) {
        uint32_t rate = p == 2 ? SW_OUT_RATE_USB : SW_OUT_RATE_TRS;
        uint64_t acc = (uint64_t)dt * rate + o->frac[p];
        uint64_t add = acc / 1000000u;
        o->frac[p] = (uint32_t)(acc % 1000000u);
        uint64_t b = o->budget[p] + add;
        if (b >= SW_OUT_BURST) {
            b = SW_OUT_BURST;
            o->frac[p] = 0;
        }
        o->budget[p] = (uint32_t)b;
    }
}

static void latch(sw_out_t *o, sw_engine_t *e)
{
    const sw_bank_t *bk = e->bank;
    uint32_t d = e->ctl_dirty;
    e->ctl_dirty = 0;
    for (int c = 0; d; c++, d >>= 1) {
        if (!(d & 1u))
            continue;
        const sw_control_t *k = &bk->ctl[c];
        if (!ctl_active(k)) {
            o->pend_ctl[c] = 0;
            o->sent[c] = NEVER16; /* resend once it's active again */
            continue;
        }
        uint16_t v = sw_out_value(k, e->live[c]);
        if (v != o->sent[c]) {
            o->sent[c] = v;
            o->pend_ctl[c] = k->ports & SW_PORT_ALL;
        }
    }
    uint16_t td = e->tog_dirty;
    e->tog_dirty = 0;
    for (int t = 0; td; t++, td >>= 1) {
        if (!(td & 1u))
            continue;
        const sw_toggle_t *g = &bk->tog[t];
        if (!(g->ports & SW_PORT_ALL)) {
            o->pend_tog[t] = 0;
            o->sent_tog[t] = NEVER8;
            continue;
        }
        uint8_t v = (uint8_t)((e->toggles >> t) & 1u);
        if (v != o->sent_tog[t]) {
            o->sent_tog[t] = v;
            o->pend_tog[t] = g->ports & SW_PORT_ALL;
        }
    }
}

void sw_out_service(sw_out_t *o, sw_engine_t *e, uint32_t now, sw_write_fn write, void *ctx)
{
    refill(o, now);
    latch(o, e);

    const sw_bank_t *bk = e->bank;
    uint8_t blocked = 0, msg[12];
    int first_left = -1;
    for (int k = 0; k < ITEMS && blocked != SW_PORT_ALL; k++) {
        int i = (o->rr + k) % ITEMS;
        uint8_t *pend;
        int len;
        if (i < SW_CONTROLS) {
            const sw_control_t *c = &bk->ctl[i];
            pend = &o->pend_ctl[i];
            if (!*pend)
                continue;
            if (!ctl_active(c)) { /* the mapping changed under it */
                *pend = 0;
                continue;
            }
            *pend &= c->ports;
            len = ctl_msg(c, o->sent[i], msg);
        } else {
            const sw_toggle_t *g = &bk->tog[i - SW_CONTROLS];
            pend = &o->pend_tog[i - SW_CONTROLS];
            if (!*pend)
                continue;
            *pend &= g->ports;
            uint8_t on = o->sent_tog[i - SW_CONTROLS] == 1;
            len = (int)(cc(msg, g->channel, g->cc, on ? g->on : g->off) - msg);
        }
        for (int p = 0; p < SW_OUT_PORTS; p++) {
            uint8_t bit = (uint8_t)(1u << p);
            if (!(*pend & bit) || (blocked & bit))
                continue;
            if (o->budget[p] >= (uint32_t)len && write(ctx, p, msg, len)) {
                o->budget[p] -= (uint32_t)len;
                o->bytes_out[p] += (uint32_t)len;
                *pend &= (uint8_t)~bit;
            } else {
                blocked |= bit;
            }
        }
        if (*pend && first_left < 0)
            first_left = i;
    }
    if (first_left >= 0)
        o->rr = (uint8_t)first_left;
}
