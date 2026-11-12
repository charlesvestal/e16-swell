/*
 * Swell: the rings and the OLED (see ui.h).
 *
 * Rings. LED 0 sits at about 7 o'clock and the LEDs run clockwise to
 * LED 15 at the bottom, by the gap (checked on the device). A value arc uses
 * LEDs 0..14 like a knob from 7 to about 4 o'clock; LED 15, the one
 * next to the bottom gap, is the toggle.
 *
 * OLED, top to bottom:
 *   0-10   inverted header: bank, slot (* modified), layer chip, transport,
 *          BPM, clock source; the save dot right of it while unsaved
 *   13-20  the cycle: a progress bar with bar-line ticks, "bar.beat/bars"
 *   23-31  armed / jump / fading slot with its fade, or the bank name
 *   36-53  the last touch: name, mapping, value in output units (large);
 *          or the store being held
 *   55-63  play: the 16 toggles; Shift: the legend for encoder 16
 */
#include "ui.h"
#include "font.h"
#include "out.h"
#include "fields.h"

#define W E16_DISPLAY_WIDTH
#define H E16_DISPLAY_HEIGHT

static const e16_api_t *api;
static uint8_t *fb;

const char *const ui_fade_names[SW_FADES] = {"cut", "1 beat", "2 beats", "1 bar", "2 bars", "4 bars", "8 bars"};
const char *const ui_curve_names[SW_CURVE_COUNT] = {"linear", "ease", "S-curve", "hold"};
static const char *const fade_short[SW_FADES] = {"CUT", "1BT", "2BT", "1BAR", "2BAR", "4BAR", "8BAR"};

/* ---- text ---- */

char *ui_put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

char *ui_put_u(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

char *ui_put_u2(char *p, uint32_t v)
{
    *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    *p = 0;
    return p;
}

/* A name field: up to SW_NAME_LEN bytes, NUL-padded, maybe not terminated. */
static char *put_name(char *p, const char *name)
{
    for (int i = 0; i < SW_NAME_LEN && name[i]; i++) {
        char c = name[i];
        *p++ = (c >= 32 && c < 127) ? c : '?';
    }
    *p = 0;
    return p;
}

/* ---- pixels ---- */

static void clear(void)
{
    for (int i = 0; i < E16_FB_SIZE; i++)
        fb[i] = 0;
}

static void pixel(int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    uint8_t m = (uint8_t)(1u << (y & 7));
    if (on)
        fb[(y >> 3) * W + x] |= m;
    else
        fb[(y >> 3) * W + x] &= (uint8_t)~m;
}

static void fill(int x0, int y0, int w, int h, int on)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            pixel(x, y, on);
}

static void frame(int x0, int y0, int w, int h)
{
    fill(x0, y0, w, 1, 1);
    fill(x0, y0 + h - 1, w, 1, 1);
    fill(x0, y0, 1, h, 1);
    fill(x0 + w - 1, y0, 1, h, 1);
}

static int text(int x, int y, const char *s, int scale, int on)
{
    for (; *s; s++) {
        const uint8_t *cols;
        int gw = font_glyph((uint8_t)*s, &cols);
        for (int c = 0; c < gw; c++)
            for (int r = 0; r < FONT_HEIGHT; r++)
                if (cols[c] >> r & 1)
                    fill(x + c * scale, y + r * scale, scale, scale, on);
        if (gw > 0)
            x += (gw + FONT_GAP) * scale;
    }
    return x;
}

static int text_w(const char *s, int scale)
{
    return font_text_width(s, 64) * scale;
}

static int text_r(int xr, int y, const char *s, int scale, int on)
{
    int x = xr - text_w(s, scale);
    text(x, y, s, scale, on);
    return x;
}

/* An inverted label: a lit box with dark text; returns the x after it. */
static int chip(int x, int y, const char *s)
{
    int w = text_w(s, 1);
    fill(x, y - 1, w + 4, 9, 1);
    text(x + 2, y, s, 1, 0);
    return x + w + 4;
}

/* A framed bar, filled to num/den. */
static void bar(int x, int y, int w, int h, uint32_t num, uint32_t den)
{
    frame(x, y, w, h);
    if (den == 0)
        return;
    if (num > den)
        num = den;
    int fw = (int)((uint64_t)num * (uint32_t)(w - 4) / den);
    fill(x + 2, y + 2, fw, h - 4, 1);
}

/* ---- rings ---- */

#define ARC_LEDS 15    /* LEDs 0..14: the value arc */
#define TOGGLE_LED 15

static uint8_t led_now[16][16][3], led_sent[16][16][3];
static int leds_pending;

static const uint8_t layer_rgb[2][3] = {{0, 36, 28}, {46, 20, 0}}; /* A teal, B amber */
static const uint8_t ghost_rgb[3] = {14, 14, 14};
static const uint8_t toggle_rgb[3] = {40, 40, 36};
static const uint8_t stored_rgb[3] = {5, 5, 5};
static const uint8_t current_rgb[3] = {30, 30, 28};
static const uint8_t armed_rgb[3] = {44, 26, 0};
static const uint8_t fading_rgb[3] = {8, 40, 22};
static const uint8_t store_rgb[3] = {34, 34, 34};

static void ring_set(int r, int i, const uint8_t *c, int num, int den)
{
    if (i < 0 || i > 15)
        return;
    for (int k = 0; k < 3; k++)
        led_now[r][i][k] = (uint8_t)(c[k] * num / den);
}

static void ring_clear(int r)
{
    for (int i = 0; i < 16; i++)
        led_now[r][i][0] = led_now[r][i][1] = led_now[r][i][2] = 0;
}

static void ring_full(int r, const uint8_t *c, int num, int den)
{
    for (int i = 0; i < 16; i++)
        ring_set(r, i, c, num, den);
}

/* The arc LED for a position 0..SW_VMAX. */
static int arc_led(uint16_t pos)
{
    return (int)(((uint32_t)pos * (ARC_LEDS - 1) + SW_VMAX / 2) / SW_VMAX);
}

/* A triangle 0..1000 with the given period. */
static int pulse(uint32_t ms, uint32_t period)
{
    uint32_t t = ms % period;
    uint32_t half = period / 2;
    return (int)((t < half ? t : period - t) * 1000u / half);
}

static void ring_play(const sw_view_t *v, int enc, const uint16_t *targets)
{
    const sw_engine_t *e = v->e;
    int c = v->layer * SW_ENCODERS + enc;
    const uint8_t *col = layer_rgb[v->layer];
    ring_clear(enc);
    if (v->bank->ctl[c].type != SW_T_OFF) {
        int n = arc_led(e->live[c]);
        for (int i = 0; i < n; i++)
            ring_set(enc, i, col, 1, 6);
        ring_set(enc, n, col, 1, 1);
        if (targets && targets[c] != e->live[c]) {
            int g = arc_led(targets[c]);
            if (g != n)
                ring_set(enc, g, ghost_rgb, 1, 1);
        }
    }
    if (e->toggles >> enc & 1)
        ring_set(enc, TOGGLE_LED, toggle_rgb, 1, 1);
}

static void ring_slot(const sw_view_t *v, int s)
{
    const sw_engine_t *e = v->e;
    const sw_status_t *st = sw_status(e);
    const sw_snapshot_t *sn = &v->bank->snap[s];
    ring_clear(s);
    if (v->store_slot == s) {
        if (v->store_done) {
            ring_full(s, fading_rgb, 1, 1);
        } else {
            int n = v->store_progress * 16 / 1000;
            ring_full(s, stored_rgb, 1, 1);
            for (int i = 0; i < n; i++)
                ring_set(s, i, store_rgb, 1, 1);
        }
        return;
    }
    if (!sn->stored)
        return;
    if (st->fading && st->fade_slot == s) {
        int n = (int)(((uint32_t)st->fade_progress * 16u + 32767u) >> 16);
        ring_full(s, stored_rgb, 1, 1);
        for (int i = 0; i < n; i++)
            ring_set(s, i, fading_rgb, 1, 1);
        return;
    }
    if (st->armed_slot == s || st->pending_jump == s) {
        /* a 3-LED segment, one turn per beat (twice as fast for a jump) */
        uint32_t per = SW_PPQN / (st->pending_jump == s ? 2u : 1u);
        int head = (int)((st->position % per) * 16u / per);
        ring_full(s, stored_rgb, 1, 1);
        ring_set(s, head, armed_rgb, 1, 1);
        ring_set(s, (head + 15) & 15, armed_rgb, 1, 3);
        ring_set(s, (head + 14) & 15, armed_rgb, 1, 8);
        return;
    }
    if (st->current_slot == s) {
        if (st->modified) {
            /* modified: pulsing, 20 % .. 100 % */
            int p = pulse(v->now_ms, 1200);
            ring_full(s, current_rgb, 200 + p * 8 / 10, 1000);
        } else {
            ring_full(s, current_rgb, 1, 1);
        }
        return;
    }
    ring_full(s, stored_rgb, 1, 1);
}

static void ring_bank(const sw_view_t *v)
{
    const uint8_t *col = layer_rgb[v->layer];
    ring_full(15, col, 1, 8);
    ring_set(15, v->bank_index, col, 1, 1);
}

static const uint8_t field_rgb[3] = {22, 22, 20};
static const uint8_t name_rgb[3] = {30, 18, 0};
static const uint8_t hold_rgb[3] = {34, 34, 34};

/* A settings page: each field's ring an arc to its value, a name field
 * a dim full ring; the field last turned brighter. */
static void ring_page(const sw_view_t *v)
{
    sw_ftarget_t t = {(sw_bank_t *)v->bank, v->page_ctl, v->page_tog};
    for (int enc = 0; enc < 16; enc++) {
        ring_clear(enc);
        int f = v->name_field >= 0 ? (enc < SW_NAME_LEN ? v->name_field : -1) : sw_field_at(v->page, enc);
        if (f < 0)
            continue;
        int bright = f == v->last_field && v->name_field < 0;
        int pos, hi;
        if (v->name_field >= 0 || !sw_field_pos(&t, f, &pos, &hi)) {
            ring_full(enc, sw_field_is_name(f) ? name_rgb : field_rgb, 1, 6);
            continue;
        }
        int n = hi > 0 ? (pos * (ARC_LEDS - 1) + hi / 2) / hi : 0;
        for (int i = 0; i < n; i++)
            ring_set(enc, i, field_rgb, 1, bright ? 3 : 6);
        ring_set(enc, n, field_rgb, bright ? 2 : 1, 1);
    }
}

void ui_leds(const sw_view_t *v)
{
    if (v->page) {
        ring_page(v);
    } else if (v->shift) {
        for (int s = 0; s < SW_SLOTS; s++)
            ring_slot(v, s);
        ring_bank(v);
    } else {
        const uint16_t *targets = sw_targets(v->e);
        for (int enc = 0; enc < SW_ENCODERS; enc++)
            ring_play(v, enc, targets);
    }
    if (!v->page && v->hold_enc >= 0) {
        /* a push held toward a page: the ring fills over the second */
        int n = v->hold_progress >= 1000 ? 16 : v->hold_progress * 16 / 1000;
        ring_clear(v->hold_enc);
        for (int i = 0; i < n; i++)
            ring_set(v->hold_enc, i, hold_rgb, 1, 1);
    }
    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 16; i++)
            if (led_now[r][i][0] != led_sent[r][i][0] || led_now[r][i][1] != led_sent[r][i][1] ||
                led_now[r][i][2] != led_sent[r][i][2]) {
                api->leds_set(r, i, led_now[r][i][0], led_now[r][i][1], led_now[r][i][2]);
                led_sent[r][i][0] = led_now[r][i][0];
                led_sent[r][i][1] = led_now[r][i][1];
                led_sent[r][i][2] = led_now[r][i][2];
                leds_pending = 1;
            }
    ui_leds_flush();
}

void ui_leds_flush(void)
{
    if (leds_pending && api->leds_show())
        leds_pending = 0;
}

/* ---- OLED ---- */

static char *put_slot(char *p, int slot)
{
    *p++ = 'S';
    if (slot < 0)
        return ui_put_s(p, "--");
    return ui_put_u2(p, (uint32_t)slot + 1);
}

/* "CC 20  ch 1" etc.: what a control sends. */
static char *put_mapping(char *p, const sw_control_t *c)
{
    switch (c->type) {
    case SW_T_CC7:
        p = ui_put_s(p, "CC ");
        p = ui_put_u(p, c->number);
        break;
    case SW_T_CC14:
        p = ui_put_s(p, "CC14 ");
        p = ui_put_u(p, c->number);
        break;
    case SW_T_NRPN7:
        p = ui_put_s(p, "NRPN ");
        p = ui_put_u(p, c->number);
        break;
    case SW_T_NRPN14:
        p = ui_put_s(p, "NRPN14 ");
        p = ui_put_u(p, c->number);
        break;
    default:
        return ui_put_s(p, "not sent");
    }
    if (!c->ports)
        return ui_put_s(p, " (no port)");
    p = ui_put_s(p, " ch");
    return ui_put_u(p, (uint32_t)c->channel + 1);
}

static void draw_header(const sw_view_t *v, const sw_status_t *st)
{
    char s[24], *p;
    fill(0, 0, 120, 11, 1);
    p = ui_put_s(s, "B");
    p = ui_put_u2(p, (uint32_t)v->bank_index + 1);
    *p++ = ' ';
    p = put_slot(p, st->current_slot);
    if (st->current_slot >= 0 && st->modified)
        *p++ = '*';
    *p = 0;
    int x = text(2, 2, s, 1, 0);
    /* the layer: a dark chip in the bar */
    x += 4;
    fill(x, 1, 9, 9, 0);
    text(x + 2, 2, v->layer ? "B" : "A", 1, 1);

    p = s;
    *p++ = (char)(st->running ? 133 : 135); /* play / stop */
    *p++ = ' ';
    p = ui_put_u(p, st->bpm_x10 / 10u);
    *p++ = '.';
    p = ui_put_u(p, st->bpm_x10 % 10u);
    p = ui_put_s(p, st->clock_source == SW_CLOCK_EXTERNAL ? " EXT" : " INT");
    text_r(118, 2, s, 1, 0);

    if (v->dirty) { /* the save dot: a 4x4 disc right of the bar */
        fill(123, 4, 4, 2, 1);
        fill(124, 3, 2, 4, 1);
    }
}

static void draw_cycle(const sw_status_t *st)
{
    char s[16], *p;
    uint32_t bar_clk = SW_PPQN * 4u;
    uint32_t cyc = st->cycle_clocks ? st->cycle_clocks : bar_clk;
    uint32_t pos = st->cycle_pos % cyc;
    uint32_t bars = cyc / bar_clk;
    if (bars == 0)
        bars = 1;
    p = ui_put_u(s, pos / bar_clk + 1);
    *p++ = '.';
    p = ui_put_u(p, (pos / SW_PPQN) % 4u + 1);
    *p++ = '/';
    ui_put_u(p, bars);
    int tx = W - text_w("16.4/16", 1);
    text_r(W, 13, s, 1, 1);

    int bw = tx - 4;
    bar(0, 13, bw, 8, pos, cyc);
    /* ticks at the bar lines (beats too when the cycle is one bar), the
     * fill's full height: lit ahead of the fill and while the fill's front
     * column is on them, dark only once the fill has moved past (going
     * dark the moment the fill reached one made the bar seem to step back) */
    uint32_t ticks = bars > 1 ? bars : 4;
    int fe = 2 + (cyc ? (int)((uint64_t)(pos > cyc ? cyc : pos) * (uint32_t)(bw - 4) / cyc) : 0);
    if (ticks <= 16) {
        for (uint32_t k = 1; k < ticks; k++) {
            int x = 2 + (int)((uint32_t)(bw - 4) * k / ticks);
            fill(x, 15, 1, 4, x >= fe - 1);
        }
    }
}

static void draw_slot_line(const sw_view_t *v, const sw_status_t *st)
{
    char s[40], *p;
    const int y = 24;
    int slot = -1;
    const char *tag = 0;
    if (st->fading) {
        slot = st->fade_slot;
        tag = "FADE";
    } else if (st->pending_jump >= 0) {
        slot = st->pending_jump;
        tag = "JUMP";
    } else if (st->armed_slot >= 0) {
        slot = st->armed_slot;
        tag = "ARM";
    }
    if (!tag) {
        put_name(s, v->bank->name);
        text(0, y, s, 1, 1);
        int n = 0;
        for (int i = 0; i < SW_SLOTS; i++)
            n += v->bank->snap[i].stored != 0;
        p = ui_put_u(s, (uint32_t)n);
        ui_put_s(p, "/15 slots");
        text_r(W, y, s, 1, 1);
        return;
    }
    int x = chip(0, y, tag);
    p = put_slot(s, slot);
    x = text(x + 4, y, s, 1, 1);
    const sw_snapshot_t *sn = &v->bank->snap[slot >= 0 && slot < SW_SLOTS ? slot : 0];
    if (st->fading) {
        bar(x + 6, y, W - x - 6, 7, st->fade_progress, 65535u);
    } else {
        p = ui_put_s(s, ui_fade_names[sn->fade < SW_FADES ? sn->fade : 0]);
        p = ui_put_s(p, " ");
        ui_put_s(p, ui_curve_names[sn->curve < SW_CURVE_COUNT ? sn->curve : 0]);
        text_r(W, y, s, 1, 1);
    }
}

/* The band: a name (small), a detail line under it, a big value right. */
static void band(const char *name, const char *detail, const char *big)
{
    text(0, 37, name, 1, 1);
    text(0, 46, detail, 1, 1);
    int sc = text_w(big, 2) <= W - 50 ? 2 : 1;
    text_r(W, sc == 2 ? 38 : 46, big, sc, 1);
}

static void draw_touch(const sw_view_t *v)
{
    char name[24], detail[32], big[16], *p;
    name[0] = detail[0] = big[0] = 0;
    const sw_engine_t *e = v->e;

    if (v->store_slot >= 0) {
        int x = v->store_done ? chip(0, 37, "RELEASE: STORE") : text(0, 37, "STORE", 1, 1);
        text(x + 4, 37, "\x80", 1, 1);
        p = put_slot(big, v->store_slot);
        text_r(W, 36, big, 2, 1);
        bar(0, 47, 80, 7, v->store_done ? 1000u : v->store_progress, 1000u);
        return;
    }

    int i = v->touch_index;
    switch (v->touch_kind) {
    case SW_TOUCH_CONTROL: {
        const sw_control_t *c = &v->bank->ctl[i];
        put_name(name, c->name);
        put_mapping(detail, c);
        ui_put_u(big, sw_out_value(c, e->live[i]));
        break;
    }
    case SW_TOUCH_TOGGLE: {
        const sw_toggle_t *t = &v->bank->tog[i];
        int on = e->toggles >> i & 1;
        put_name(name, t->name);
        p = ui_put_s(detail, "CC ");
        p = ui_put_u(p, t->cc);
        p = ui_put_s(p, " = ");
        ui_put_u(p, on ? t->on : t->off);
        ui_put_s(big, on ? "ON" : "OFF");
        break;
    }
    case SW_TOUCH_SLOT: {
        const sw_snapshot_t *sn = &v->bank->snap[i];
        p = ui_put_s(name, "SLOT ");
        p = ui_put_u(p, (uint32_t)i + 1);
        ui_put_s(p, sn->stored ? "" : "  (empty)");
        p = ui_put_s(detail, "curve ");
        ui_put_s(p, ui_curve_names[sn->curve < SW_CURVE_COUNT ? sn->curve : 0]);
        ui_put_s(big, fade_short[sn->fade < SW_FADES ? sn->fade : 0]);
        break;
    }
    case SW_TOUCH_BANK:
        ui_put_s(name, "BANK");
        put_name(detail, v->bank->name);
        p = ui_put_s(big, "B");
        ui_put_u2(p, (uint32_t)v->bank_index + 1);
        break;
    case SW_TOUCH_LAYER:
        ui_put_s(name, "LAYER");
        ui_put_s(detail, v->layer ? "encoders: B1-B16" : "encoders: A1-A16");
        ui_put_s(big, v->layer ? "B" : "A");
        break;
    default:
        ui_put_s(name, "SWELL");
        ui_put_s(detail, "Shift: slots");
        break;
    }
    band(name, detail, big);
}

static void draw_bottom(const sw_view_t *v)
{
    if (v->shift) {
        fill(0, 55, W, 9, 1);
        int x = 2;
        if (v->shift_latched) {
            char lk[2] = {(char)142, 0}; /* lock: Shift latched */
            x = text(x, 56, lk, 1, 0) + 3;
        }
        /* the longest wording that fits after the lock icon */
        static const char *const legs[] = {"16  push: layer   turn: bank", "16 push:layer turn:bank",
                                           "16 push:A/B turn:bank", "16: A/B, bank"};
        const char *leg = legs[3];
        for (int i = 0; i < 4; i++)
            if (x + text_w(legs[i], 1) <= W - 2) {
                leg = legs[i];
                break;
            }
        text(x, 56, leg, 1, 0);
        return;
    }
    /* the 16 toggles: a block when on, a line when off */
    for (int t = 0; t < SW_TOGGLES; t++) {
        int x = t * 8 + 1;
        if (v->e->toggles >> t & 1)
            fill(x, 57, 6, 6, 1);
        else
            fill(x, 62, 6, 1, 1);
    }
}

/* ---- the settings pages ---- */

static char *put_ctl_title(char *p, const sw_view_t *v)
{
    int c = v->page_ctl;
    *p++ = c < SW_ENCODERS ? 'A' : 'B';
    p = ui_put_u(p, (uint32_t)(c % SW_ENCODERS) + 1);
    *p++ = ' ';
    return put_name(p, v->bank->ctl[c].name);
}

static void page_header(const sw_view_t *v, const sw_ftarget_t *t)
{
    char s[48], *p = s;
    *p = 0;
    if (v->last_field >= 0) {
        /* the field last turned, in full */
        char val[24];
        sw_field_text(t, v->last_field, 1, val);
        p = ui_put_s(p, sw_field_long(v->last_field));
        p = ui_put_s(p, " ");
        ui_put_s(p, val);
        if (text_w(s, 1) > W - 4) { /* too wide: the short label */
            p = ui_put_s(s, sw_field_label(v->last_field));
            p = ui_put_s(p, " ");
            ui_put_s(p, val);
        }
    } else if (v->page == SW_PAGE_KNOB && v->page_half) {
        p = ui_put_s(p, "TOGGLE ");
        p = ui_put_u(p, (uint32_t)v->page_tog + 1);
        *p++ = ' ';
        put_name(p, v->bank->tog[v->page_tog].name);
    } else if (v->page == SW_PAGE_KNOB) {
        put_ctl_title(p, v);
    } else {
        p = ui_put_s(p, "BANK ");
        p = ui_put_u2(p, (uint32_t)v->bank_index + 1);
        *p++ = ' ';
        put_name(p, v->bank->name);
    }
    fill(0, 0, W, 10, 1);
    text(2, 1, s, 1, 0);
}

/* Half the grid at a time (encoders 1-8 or 9-16), so the lines have room:
 * two rows of four cells, a label over its value. */
static void draw_page(const sw_view_t *v)
{
    sw_ftarget_t t = {(sw_bank_t *)v->bank, v->page_ctl, v->page_tog};
    page_header(v, &t);
    int half = v->page == SW_PAGE_KNOB ? v->page_half : 0;
    for (int k = 0; k < 8; k++) {
        int enc = half * 8 + k;
        int f = sw_field_at(v->page, enc);
        if (f < 0)
            continue;
        int x = (k % 4) * 32 + 2, y = 14 + (k / 4) * 25;
        char val[24];
        sw_field_text(&t, f, 0, val);
        text(x, y, sw_field_label(f), 1, 1);
        fill(x, y + 9, 26, 1, 1); /* a rule between the label and the value */
        if (f == v->last_field) {
            fill(x - 2, y + 11, 31, 10, 1);
            text(x, y + 12, val, 1, 0);
        } else {
            text(x, y + 12, val, 1, 1);
        }
    }
    if (v->page == SW_PAGE_KNOB) {
        /* which half: a marker at the right edge, top or bottom */
        fill(W - 2, half ? 37 : 14, 2, 23, 1);
    }
}

static void draw_name_editor(const sw_view_t *v)
{
    sw_ftarget_t t = {(sw_bank_t *)v->bank, v->page_ctl, v->page_tog};
    char s[40], *p;
    p = ui_put_s(s, sw_field_long(v->name_field));
    if (v->name_field != SW_F_BNAME) {
        p = ui_put_s(p, v->name_field == SW_F_NAME ? " " : " T");
        if (v->name_field == SW_F_NAME) {
            *p++ = v->page_ctl < SW_ENCODERS ? 'A' : 'B';
            ui_put_u(p, (uint32_t)(v->page_ctl % SW_ENCODERS) + 1);
        } else {
            ui_put_u(p, (uint32_t)v->page_tog + 1);
        }
    }
    fill(0, 0, W, 10, 1);
    text(2, 1, s, 1, 0);
    /* the 8 characters, one per encoder 1-8, in a row of 16-pixel cells */
    const char *name = sw_field_name(&t, v->name_field);
    for (int i = 0; i < SW_NAME_LEN; i++) {
        char c[2] = {name[i] && name[i] >= 32 && name[i] < 127 ? name[i] : ' ', 0};
        int x = i * 16;
        int w = text_w(c, 2);
        text(x + (16 - w) / 2, 18, c, 2, 1);
        fill(x + 3, 36, 10, 1, 1); /* a line under each cell */
        char n[3];
        ui_put_u(n, (uint32_t)i + 1);
        text(x + (16 - text_w(n, 1)) / 2, 39, n, 1, 1);
    }
    text(0, 55, "enc 1-8: letters  push: done", 1, 1);
}

void ui_oled(const sw_view_t *v)
{
    const sw_status_t *st = sw_status(v->e);
    clear();
    if (v->page) {
        if (v->name_field >= 0)
            draw_name_editor(v);
        else
            draw_page(v);
        api->display_present();
        return;
    }
    draw_header(v, st);
    draw_cycle(st);
    draw_slot_line(v, st);
    for (int x = 0; x < W; x += 2)
        pixel(x, 33, 1);
    draw_touch(v);
    draw_bottom(v);
    api->display_present();
}

void ui_init(const e16_api_t *a)
{
    api = a;
    fb = api->display_fb();
    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 16; i++)
            led_sent[r][i][0] = led_sent[r][i][1] = led_sent[r][i][2] = 0xFF; /* force the first frame */
}
