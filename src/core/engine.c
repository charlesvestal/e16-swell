/*
 * Swell core: the engine (engine.h has the contract).
 *
 * Position. st.position counts clocks (SW_PPQN per beat). It only ever
 * moves forward through advance(), which is where cycle boundaries (the
 * armed slot's drop) and quantize boundaries (a pending jump) are
 * detected: a boundary B fires when from < B <= to. The only other way it
 * moves is set_pos(), for the discontinuities (Start, Stop, Song Position,
 * the first tick after Start/Continue); after one of those, landing()
 * fires a boundary the position now sits on exactly, so a slot armed
 * before Start drops on the downbeat.
 *
 * Internal clock: the elapsed microseconds times bpm_x10 * 4 give clocks
 * in units of 1/5,000,000 clock (480 clocks/beat * bpm_x10/600 beats/s =
 * bpm_x10 * 4 / 5e6 clocks/us); the remainder stays in frac_us, so the
 * position never drifts however the loop's timing jitters.
 *
 * External clock. Each F8 is SW_CLK_PER_TICK (20) clocks. tick_pos is the
 * position of the last tick; between ticks the position is interpolated
 * from the smoothed tick period, capped at tick_pos + 19 so the boundary
 * that the next tick lands on fires on that tick and not before it. After
 * FA Start / FB Continue / F2 Song Position, the next F8 *is* the stored
 * position (it doesn't advance), as MIDI defines it.
 *
 * Fades are computed from the position: progress = (position -
 * fade_start_pos) / fade_len. A drop's fade starts at the boundary itself,
 * not wherever the loop pass happened to notice it, so it stays on the
 * grid. A discontinuity rebases fade_start_pos so the elapsed part of a
 * running fade is kept (Stop freezes a fade rather than restarting it).
 *
 * A pending jump keeps its quantum (480 or 1920 clocks) in jump_at and
 * fires on the next multiple of it, by the same rule as the drop; that way
 * it survives the position being moved under it.
 */
#include "engine.h"

#define TLT(a, b) ((int32_t)((a) - (b)) < 0)

#define Q16 65536u        /* fade progress / curve scale: 1.0 */
#define CLK_DEN 5000000u  /* internal clock accumulator units per clock */
#define BAR (4u * SW_PPQN)

/* ext_seen bits: the external transport. */
#define XF_RUN 1u   /* Start / Continue received, no Stop since */
#define XF_FIRST 2u /* the next F8 is at tick_pos (doesn't advance) */

#define ALL_CTL 0xFFFFFFFFu
#define ALL_TOG 0xFFFFu

/* ---- small helpers ---- */

static void zero(void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

static int slot_ok(const sw_engine_t *e, int s)
{
    return s >= 0 && s < SW_SLOTS && e->bank->snap[s].stored;
}

static uint32_t cycle_len(const sw_engine_t *e)
{
    uint8_t i = e->bank->cycle;
    return (uint32_t)sw_cycle_bars[i < SW_CYCLES ? i : SW_CYCLES - 1] * BAR;
}

static uint16_t bank_bpm(const sw_engine_t *e)
{
    uint16_t b = e->bank->bpm_x10;
    return b < 400 ? 400 : b > 3000 ? 3000 : b;
}

static void set_live(sw_engine_t *e, int c, uint16_t v)
{
    if (e->live[c] != v) {
        e->live[c] = v;
        e->ctl_dirty |= 1u << c;
    }
}

static void set_toggles(sw_engine_t *e, uint16_t bits)
{
    e->tog_dirty |= (uint16_t)(e->toggles ^ bits);
    e->toggles = bits;
}

/* The fade curves, p and the result in Q16 (0..65536):
 *   LINEAR  p
 *   EASE    quadratic ease-in-out: 2p^2 below the midpoint, mirrored
 *           above (0.125 at a quarter, slope 2 at the midpoint)
 *   S       cubic ease-in-out: 4p^3 below the midpoint, mirrored above;
 *           a steeper S with longer flat ends (0.0625 at a quarter,
 *           slope 3 at the midpoint)
 *   HOLD    0 until the end, then 1
 * Every curve gives 0 at 0 and 1 at 1, and all but HOLD 0.5 at 0.5. */
static uint32_t curve(uint8_t cv, uint32_t p)
{
    if (p >= Q16)
        return Q16;
    int hi = p > Q16 / 2;
    uint64_t q = hi ? Q16 - p : p; /* the lower half, mirrored */
    uint32_t r;
    switch (cv) {
    case SW_CURVE_EASE:
        r = (uint32_t)((q * q) >> 15);            /* 2q^2 */
        break;
    case SW_CURVE_S:
        r = (uint32_t)((q * q * q) >> 30);        /* 4q^3 */
        break;
    case SW_CURVE_HOLD:
        return 0;
    default:
        return p;
    }
    return hi ? Q16 - r : r;
}

/* from + (to - from) * k / Q16, rounded to nearest. */
static uint16_t lerp(uint16_t from, uint16_t to, uint32_t k)
{
    int32_t d = (int32_t)to - (int32_t)from;
    int32_t n = d * (int32_t)k; /* |d| < 2^14, k <= 2^16: fits */
    int32_t r = n >= 0 ? (n + (int32_t)(Q16 / 2)) / (int32_t)Q16
                       : -((-n + (int32_t)(Q16 / 2)) / (int32_t)Q16);
    return (uint16_t)(from + r);
}

/* ---- fades ---- */

static void stop_fade(sw_engine_t *e)
{
    e->st.fading = 0;
    e->st.fade_slot = -1;
    e->st.fade_progress = 0;
    e->fade_mask = 0;
}

/* Move the faded controls to where the position says; ends the fade. */
static void update_fade(sw_engine_t *e)
{
    if (!e->st.fading)
        return;
    int32_t el = (int32_t)(e->st.position - e->fade_start_pos);
    uint32_t p;
    if (el < 0)
        p = 0;
    else if ((uint32_t)el >= e->fade_len)
        p = Q16;
    else
        p = (uint32_t)(((uint64_t)(uint32_t)el * Q16) / e->fade_len);
    uint32_t k = curve(e->fade_curve, p);
    for (int c = 0; c < SW_CONTROLS; c++)
        if (e->fade_mask & (1u << c))
            set_live(e, c, lerp(e->fade_from[c], e->fade_to[c], k));
    if (p >= Q16) {
        stop_fade(e);
        e->st.fade_progress = 0;
    } else {
        e->st.fade_progress = (uint16_t)p;
    }
}

/* Start the fade to slot s (stored) at position start_pos. */
static void start_fade(sw_engine_t *e, int s, uint32_t start_pos, int cut)
{
    const sw_snapshot_t *n = &e->bank->snap[s];
    set_toggles(e, n->toggles);
    e->st.current_slot = s;
    e->st.modified = 0;
    uint8_t f = n->fade < SW_FADES ? n->fade : SW_FADES - 1;
    uint32_t len = cut ? 0 : (uint32_t)sw_fade_beats[f] * SW_PPQN;
    stop_fade(e);
    if (!len) {
        for (int c = 0; c < SW_CONTROLS; c++)
            set_live(e, c, n->value[c]);
        return;
    }
    for (int c = 0; c < SW_CONTROLS; c++) {
        e->fade_from[c] = e->live[c];
        e->fade_to[c] = n->value[c];
    }
    e->fade_mask = ALL_CTL;
    e->fade_start_pos = start_pos;
    e->fade_len = len;
    e->fade_curve = n->curve;
    e->st.fading = 1;
    e->st.fade_slot = s;
    update_fade(e);
}

/* ---- the position ---- */

/* The next multiple of q strictly after a is within (a, b]? Then *at = it. */
static int crosses(uint32_t a, uint32_t b, uint32_t q, uint32_t *at)
{
    uint32_t nb = (a / q + 1u) * q;
    if ((uint32_t)(b - a) >= (uint32_t)(nb - a)) {
        *at = nb;
        return 1;
    }
    return 0;
}

/* A boundary fired at position `at`: the pending jump first, then the
 * armed slot (armed later, so its fade wins if both fire together). */
static void fire_jump(sw_engine_t *e, uint32_t at)
{
    int s = e->st.pending_jump;
    e->st.pending_jump = -1;
    if (slot_ok(e, s))
        start_fade(e, s, at, 0);
}

static void fire_arm(sw_engine_t *e, uint32_t at)
{
    int s = e->st.armed_slot;
    e->st.armed_slot = -1;
    if (slot_ok(e, s))
        start_fade(e, s, at, 0);
}

/* Move the position forward to `to` (to after position, wrap-safe),
 * firing the boundaries in between. */
static void advance(sw_engine_t *e, uint32_t to)
{
    uint32_t from = e->st.position, at;
    if (!TLT(from, to))
        return;
    e->st.position = to;
    if (e->st.pending_jump >= 0 && crosses(from, to, e->jump_at, &at))
        fire_jump(e, at);
    if (e->st.armed_slot >= 0 && crosses(from, to, cycle_len(e), &at))
        fire_arm(e, at);
}

/* A discontinuity: put the position at p; a running fade keeps its
 * elapsed part. */
static void set_pos(sw_engine_t *e, uint32_t p)
{
    if (e->st.fading) {
        int32_t el = (int32_t)(e->st.position - e->fade_start_pos);
        if (el < 0)
            el = 0;
        if ((uint32_t)el > e->fade_len)
            el = (int32_t)e->fade_len;
        e->fade_start_pos = p - (uint32_t)el;
    }
    e->st.position = p;
}

/* The position has just arrived at a point (the first tick after Start /
 * Continue): a boundary it sits on exactly counts as crossed. */
static void landing(sw_engine_t *e)
{
    uint32_t p = e->st.position;
    if (e->st.pending_jump >= 0 && p % e->jump_at == 0)
        fire_jump(e, p);
    if (e->st.armed_slot >= 0 && p % cycle_len(e) == 0)
        fire_arm(e, p);
}

static void update_status(sw_engine_t *e)
{
    sw_status_t *s = &e->st;
    if (s->clock_source == SW_CLOCK_EXTERNAL) {
        s->running = (e->ext_seen & XF_RUN) != 0;
        if (e->tick_period_us) {
            uint32_t b = (25000000u + e->tick_period_us / 2) / e->tick_period_us;
            s->bpm_x10 = (uint16_t)(b > 0xFFFFu ? 0xFFFFu : b);
        }
    } else {
        s->running = 1;
        s->bpm_x10 = bank_bpm(e);
    }
    s->cycle_clocks = cycle_len(e);
    s->cycle_pos = s->position % s->cycle_clocks;
}

static void go_internal(sw_engine_t *e)
{
    e->st.clock_source = SW_CLOCK_INTERNAL;
    e->ext_seen = 0;
    e->tick_period_us = 0;
    e->tick_n = 0;
    e->frac_us = 0;
}

/* A tick at `now` into the history; the period is the span of the history
 * over the intervals in it (up to SW_TICK_HIST). */
static void tick_measure(sw_engine_t *e, uint32_t now)
{
    e->tick_hist[e->tick_i] = now;
    e->tick_i = (uint8_t)((e->tick_i + 1u) % (SW_TICK_HIST + 1u));
    if (e->tick_n < SW_TICK_HIST + 1u)
        e->tick_n++;
    if (e->tick_n < 2)
        return;
    uint32_t oldest = e->tick_hist[(e->tick_i + (SW_TICK_HIST + 1u) - e->tick_n) % (SW_TICK_HIST + 1u)];
    uint32_t n = e->tick_n - 1u;
    e->tick_period_us = ((now - oldest) + n / 2u) / n;
    if (!e->tick_period_us)
        e->tick_period_us = 1;
}

/* The internal clock over dt microseconds, plus its F8 output. */
static void run_internal(sw_engine_t *e, uint32_t dt)
{
    uint64_t acc = (uint64_t)dt * bank_bpm(e) * 4u + e->frac_us;
    uint32_t n = (uint32_t)(acc / CLK_DEN);
    e->frac_us = (uint32_t)(acc % CLK_DEN);
    if (!n)
        return;
    uint32_t from = e->st.position;
    advance(e, from + n);
    uint8_t ports = e->bank->clock_out & SW_PORT_ALL;
    if (ports && e->clock_fn) {
        /* One F8 per multiple of SW_CLK_PER_TICK crossed. */
        uint32_t k = (from + n) / SW_CLK_PER_TICK - from / SW_CLK_PER_TICK;
        while (k--)
            e->clock_fn(e->clock_ctx, ports, 0xF8);
    }
}

static void ext_tick(sw_engine_t *e, uint32_t now)
{
    if (e->st.clock_source != SW_CLOCK_EXTERNAL) {
        e->st.clock_source = SW_CLOCK_EXTERNAL;
        e->tick_period_us = 0;
        e->tick_n = 0;
        if (!(e->ext_seen & XF_FIRST))
            e->tick_pos = e->st.position; /* carry on from here */
    }
    tick_measure(e, now);
    e->last_tick_us = now;
    if (!(e->ext_seen & XF_RUN))
        return;
    if (e->ext_seen & XF_FIRST) {
        e->ext_seen &= (uint8_t)~XF_FIRST;
        set_pos(e, e->tick_pos);
        landing(e);
    } else {
        e->tick_pos += SW_CLK_PER_TICK;
        advance(e, e->tick_pos);
    }
}

/* ---- the API ---- */

void sw_init(sw_engine_t *e, sw_bank_t *bank, uint32_t now, sw_clock_fn clock_fn, void *ctx)
{
    zero(e, sizeof *e);
    e->bank = bank;
    e->clock_fn = clock_fn;
    e->clock_ctx = ctx;
    e->st.current_slot = -1;
    e->st.armed_slot = -1;
    e->st.fade_slot = -1;
    e->st.pending_jump = -1;
    e->st.clock_source = SW_CLOCK_INTERNAL;
    e->jump_at = SW_PPQN;
    e->last_us = now;
    e->last_tick_us = now;
    /* Nothing goes out at start unless slot 0 is recalled: an empty
     * bank mustn't zero every CC on the rig. */
    if (slot_ok(e, 0)) {
        e->ctl_dirty = ALL_CTL;
        e->tog_dirty = ALL_TOG;
        start_fade(e, 0, 0, 1);
    }
    update_status(e);
}

void sw_set_bank(sw_engine_t *e, sw_bank_t *bank, uint32_t now)
{
    (void)now;
    e->bank = bank;
    e->st.armed_slot = -1;
    e->st.pending_jump = -1;
    stop_fade(e);
    e->st.current_slot = -1;
    e->st.modified = 0;
    if (slot_ok(e, 0))
        start_fade(e, 0, e->st.position, 0);
    update_status(e);
}

void sw_bank_changed(sw_engine_t *e, uint32_t now)
{
    (void)now;
    if (!slot_ok(e, e->st.armed_slot))
        e->st.armed_slot = -1;
    if (!slot_ok(e, e->st.pending_jump))
        e->st.pending_jump = -1;
    if (e->st.fading && !slot_ok(e, e->st.fade_slot))
        stop_fade(e);
    if (!slot_ok(e, e->st.current_slot)) {
        e->st.current_slot = -1;
        e->st.modified = 0;
    }
    e->ctl_dirty = ALL_CTL;
    e->tog_dirty = ALL_TOG;
    update_status(e);
}

void sw_midi_in(sw_engine_t *e, uint32_t now, const uint8_t *msg, int len)
{
    if (len < 1)
        return;
    switch (msg[0]) {
    case 0xF8:
        ext_tick(e, now);
        break;
    case 0xFA: /* Start: position 0; the next tick is the downbeat */
        set_pos(e, 0);
        e->tick_pos = 0;
        e->ext_seen |= XF_RUN | XF_FIRST;
        break;
    case 0xFB: /* Continue: from where Song Position (or Stop) put it */
        e->tick_pos = e->st.position;
        e->ext_seen |= XF_RUN | XF_FIRST;
        break;
    case 0xFC: /* Stop: back to 0, held there */
        e->ext_seen &= (uint8_t)~(XF_RUN | XF_FIRST);
        set_pos(e, 0);
        e->tick_pos = 0;
        break;
    case 0xF2: /* Song Position: 1 unit = 6 MIDI clocks */
        if (len >= 3) {
            uint32_t spp = (uint32_t)(msg[1] & 0x7F) | ((uint32_t)(msg[2] & 0x7F) << 7);
            uint32_t p = spp * 6u * SW_CLK_PER_TICK;
            set_pos(e, p);
            e->tick_pos = p;
            e->ext_seen |= XF_FIRST;
        }
        break;
    default:
        return;
    }
    update_fade(e);
    update_status(e);
}

void sw_tick(sw_engine_t *e, uint32_t now)
{
    uint32_t dt = now - e->last_us;
    e->last_us = now;
    if (e->st.clock_source == SW_CLOCK_EXTERNAL &&
        (uint32_t)(now - e->last_tick_us) >= SW_EXT_TIMEOUT_US)
        go_internal(e); /* the position carries on from where it is */

    if (e->st.clock_source == SW_CLOCK_INTERNAL) {
        run_internal(e, dt);
    } else if ((e->ext_seen & (XF_RUN | XF_FIRST)) == XF_RUN && e->tick_period_us) {
        uint32_t el = now - e->last_tick_us;
        uint64_t ip = ((uint64_t)el * SW_CLK_PER_TICK) / e->tick_period_us;
        if (ip > SW_CLK_PER_TICK - 1)
            ip = SW_CLK_PER_TICK - 1;
        advance(e, e->tick_pos + (uint32_t)ip);
    }
    update_fade(e);
    update_status(e);
}

/* A hand (encoder or editor) puts control c at v. */
static void hand_set(sw_engine_t *e, int c, int32_t v)
{
    if (c < 0 || c >= SW_CONTROLS)
        return;
    if (v < 0)
        v = 0;
    if (v > (int32_t)SW_VMAX)
        v = SW_VMAX;
    if (e->fade_mask & (1u << c)) {
        e->fade_mask &= ~(1u << c);
        e->st.modified = 1;
    }
    if (e->live[c] != (uint16_t)v) {
        set_live(e, c, (uint16_t)v);
        e->st.modified = 1;
    }
}

void sw_turn(sw_engine_t *e, int ctl, int delta, int fine, uint32_t now)
{
    (void)now;
    if (ctl < 0 || ctl >= SW_CONTROLS)
        return;
    int32_t step = fine ? SW_STEP_FINE : SW_STEP_COARSE;
    if (delta > 1000)
        delta = 1000;
    if (delta < -1000)
        delta = -1000;
    hand_set(e, ctl, (int32_t)e->live[ctl] + delta * step);
}

void sw_set_value(sw_engine_t *e, int ctl, uint16_t value, uint32_t now)
{
    (void)now;
    hand_set(e, ctl, value);
}

void sw_toggle(sw_engine_t *e, int t, int on)
{
    if (t < 0 || t >= SW_TOGGLES)
        return;
    uint16_t bit = (uint16_t)(1u << t);
    uint16_t bits = on < 0 ? (uint16_t)(e->toggles ^ bit)
                    : on   ? (uint16_t)(e->toggles | bit)
                           : (uint16_t)(e->toggles & ~bit);
    if (bits != e->toggles) {
        set_toggles(e, bits);
        e->st.modified = 1;
    }
}

void sw_slot_press(sw_engine_t *e, int slot, uint32_t now)
{
    if (!slot_ok(e, slot))
        return;
    if (e->st.armed_slot == slot)
        sw_jump(e, slot, now);
    else
        e->st.armed_slot = slot;
}

void sw_jump(sw_engine_t *e, int slot, uint32_t now)
{
    (void)now;
    if (!slot_ok(e, slot))
        return;
    e->st.armed_slot = -1;
    uint8_t q = e->bank->jump_quant;
    if (q == SW_JQ_BEAT || q == SW_JQ_BAR) {
        e->st.pending_jump = slot;
        e->jump_at = q == SW_JQ_BEAT ? SW_PPQN : BAR;
    } else {
        e->st.pending_jump = -1;
        start_fade(e, slot, e->st.position, 0);
    }
    update_status(e);
}

void sw_cancel_arm(sw_engine_t *e)
{
    e->st.armed_slot = -1;
}

int sw_store(sw_engine_t *e, int slot)
{
    if (slot < 0 || slot >= SW_SLOTS)
        return 0;
    sw_snapshot_t *n = &e->bank->snap[slot];
    for (int c = 0; c < SW_CONTROLS; c++)
        n->value[c] = e->live[c];
    n->toggles = e->toggles;
    n->stored = 1;
    /* What was stored is what's live now: a running fade stops here
     * (otherwise it would carry the live state straight away from the
     * slot just marked current and unmodified). */
    stop_fade(e);
    e->st.current_slot = slot;
    e->st.modified = 0;
    return 1;
}

const sw_status_t *sw_status(const sw_engine_t *e)
{
    return &e->st;
}

const uint16_t *sw_targets(const sw_engine_t *e)
{
    if (slot_ok(e, e->st.armed_slot))
        return e->bank->snap[e->st.armed_slot].value;
    if (e->st.fading)
        return e->fade_to;
    if (slot_ok(e, e->st.pending_jump))
        return e->bank->snap[e->st.pending_jump].value;
    return 0;
}
