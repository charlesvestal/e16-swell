/*
 * Swell core: the data model (model.h has the overview).
 */
#include "model.h"

const uint8_t sw_fade_beats[SW_FADES] = {0, 1, 2, 4, 8, 16, 32};
const uint8_t sw_cycle_bars[SW_CYCLES] = {1, 2, 4, 8, 16};

/* ---- small helpers ---- */

static void zero(void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

/* name := s, NUL-padded to SW_NAME_LEN. */
static void set_name(char *name, const char *s)
{
    int i = 0;
    for (; i < SW_NAME_LEN && s[i]; i++)
        name[i] = s[i];
    for (; i < SW_NAME_LEN; i++)
        name[i] = 0;
}

/* "<prefix><n>" with n 1..99. */
static void num_name(char *name, char prefix, int n)
{
    char s[4];
    int k = 0;
    s[k++] = prefix;
    if (n >= 10)
        s[k++] = (char)('0' + n / 10);
    s[k++] = (char)('0' + n % 10);
    s[k] = 0;
    set_name(name, s);
}

/* Clamp *v to hi; counts a change. */
static void clamp8(uint8_t *v, uint8_t hi, int *n)
{
    if (*v > hi) {
        *v = hi;
        (*n)++;
    }
}

static void clamp16(uint16_t *v, uint16_t hi, int *n)
{
    if (*v > hi) {
        *v = hi;
        (*n)++;
    }
}

/* A name is printable ASCII, then NUL padding: anything unprintable becomes
 * a space, and everything after the first NUL becomes NUL. */
static void sanitize_name(char *name, int *n)
{
    int end = 0;
    for (int i = 0; i < SW_NAME_LEN; i++) {
        uint8_t c = (uint8_t)name[i];
        if (end) {
            if (c) {
                name[i] = 0;
                (*n)++;
            }
        } else if (!c) {
            end = 1;
        } else if (c < 0x20 || c > 0x7E) {
            name[i] = ' ';
            (*n)++;
        }
    }
}

/* ---- the factory bank ---- */

void sw_bank_default(sw_bank_t *b, int index)
{
    zero(b, sizeof *b);
    /* Named after the 1-based bank number, as the UI shows it. */
    int n = index < 0 ? 1 : index > 98 ? 99 : index + 1;
    char nm[8] = {'B', 'A', 'N', 'K', ' ', 0, 0, 0};
    nm[5] = (char)('0' + n / 10);
    nm[6] = (char)('0' + n % 10);
    set_name(b->name, nm);
    b->cycle = 2; /* 4 bars */
    b->jump_quant = SW_JQ_NONE;
    b->clock_out = 0;
    b->bpm_x10 = 1200;
    for (int c = 0; c < SW_CONTROLS; c++) {
        sw_control_t *k = &b->ctl[c];
        int layer = c / SW_ENCODERS, enc = c % SW_ENCODERS;
        num_name(k->name, layer ? 'B' : 'A', enc + 1);
        k->type = SW_T_CC7;
        k->ports = SW_PORT_TRS1 | SW_PORT_USB;
        k->channel = 0;
        k->number = (uint16_t)((layer ? 40 : 20) + enc);
        k->min = 0;
        k->max = 127;
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        sw_toggle_t *g = &b->tog[t];
        num_name(g->name, 'T', t + 1);
        g->ports = SW_PORT_TRS1 | SW_PORT_USB;
        g->channel = 0;
        g->cc = (uint8_t)(80 + t);
        g->off = 0;
        g->on = 127;
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        b->snap[s].stored = 0;
        b->snap[s].fade = 3; /* 1 bar */
        b->snap[s].curve = SW_CURVE_LINEAR;
    }
}

/* ---- sanitize ---- */

int sw_bank_sanitize(sw_bank_t *b)
{
    int n = 0;
    sanitize_name(b->name, &n);
    clamp8(&b->cycle, SW_CYCLES - 1, &n);
    clamp8(&b->jump_quant, SW_JQ_COUNT - 1, &n);
    if (b->clock_out & ~SW_PORT_ALL) {
        b->clock_out &= SW_PORT_ALL;
        n++;
    }
    if (b->bpm_x10 < 400) {
        b->bpm_x10 = 400;
        n++;
    }
    clamp16(&b->bpm_x10, 3000, &n);

    for (int c = 0; c < SW_CONTROLS; c++) {
        sw_control_t *k = &b->ctl[c];
        sanitize_name(k->name, &n);
        if (k->type >= SW_T_COUNT) { /* an unknown type sends nothing */
            k->type = SW_T_OFF;
            n++;
        }
        if (k->ports & ~SW_PORT_ALL) {
            k->ports &= SW_PORT_ALL;
            n++;
        }
        clamp8(&k->channel, 15, &n);
        uint16_t num_hi = k->type == SW_T_CC7 ? 127 : k->type == SW_T_CC14 ? 31 : 16383;
        uint16_t val_hi = (k->type == SW_T_CC7 || k->type == SW_T_NRPN7) ? 127 : 16383;
        clamp16(&k->number, num_hi, &n);
        clamp16(&k->min, val_hi, &n);
        clamp16(&k->max, val_hi, &n);
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        sw_toggle_t *g = &b->tog[t];
        sanitize_name(g->name, &n);
        if (g->ports & ~SW_PORT_ALL) {
            g->ports &= SW_PORT_ALL;
            n++;
        }
        clamp8(&g->channel, 15, &n);
        clamp8(&g->cc, 127, &n);
        clamp8(&g->off, 127, &n);
        clamp8(&g->on, 127, &n);
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        sw_snapshot_t *p = &b->snap[s];
        clamp8(&p->stored, 1, &n);
        clamp8(&p->fade, SW_FADES - 1, &n);
        clamp8(&p->curve, SW_CURVE_COUNT - 1, &n);
        for (int c = 0; c < SW_CONTROLS; c++)
            clamp16(&p->value[c], SW_VMAX, &n);
    }
    return n;
}
