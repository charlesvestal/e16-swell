/*
 * Swell core: the engine. Clock following, the cycle, arm / drop / jump,
 * fades with hands-on override, the live values.
 *
 * Portable C99, no malloc, no hardware. The app and the host tests drive
 * it the same way:
 *
 *   sw_init(e, bank, now);
 *   sw_midi_in(e, now, bytes, len);   every received message (only the
 *                                     realtime ones and SPP matter)
 *   sw_tick(e, now);                  every loop pass
 *   sw_turn / sw_toggle / sw_slot_press / sw_jump / sw_store ...
 *
 * Time `now` is a free-running uint32_t in microseconds (wraps; every
 * comparison is wrap-safe).
 *
 * Musical position is in clocks, SW_PPQN per beat (quarter note), counted
 * from transport start. 4/4 is assumed: a bar is 4 beats.
 *
 * Clock source:
 *   - EXTERNAL while a MIDI clock tick (F8) arrived in the last
 *     SW_EXT_TIMEOUT_US. The position advances only while the transport
 *     runs (after FA Start = position 0, or FB Continue); FC Stop resets
 *     the position to 0 and stops it; F2 Song Position sets it (1 SPP unit
 *     = 6 MIDI clocks = 1/16 note). Between ticks the position is
 *     interpolated with the measured tempo, but never past the next tick.
 *   - INTERNAL otherwise: the position free-runs at the bank's bpm_x10,
 *     continuing phase-continuously from wherever it was. While INTERNAL
 *     and bank->clock_out != 0, the engine emits F8 at 24 ppqn through
 *     clock_fn (and nothing else: no start/stop).
 *
 * Arm / drop / jump (slot = 0..SW_SLOTS-1):
 *   - sw_slot_press(): an empty slot does nothing. Pressing the armed slot
 *     again = sw_jump() to it. Otherwise it becomes the armed slot
 *     (replacing any other). The armed slot's fade starts when the
 *     position next crosses a cycle boundary (a multiple of the cycle
 *     length; a boundary the position lands on exactly counts as crossed).
 *     While the position doesn't move (external clock, stopped) nothing
 *     fires; the slot stays armed.
 *   - sw_jump(): starts the fade at once (jump_quant NONE) or at the next
 *     beat / bar boundary. Clears any armed slot.
 *   - A fade start: the target's toggles are applied at once; every
 *     control moves from its live value at that moment to the target
 *     value along the slot's curve over sw_fade_beats[fade] beats measured
 *     in position (so it follows tempo changes); fade 0 = cut at once.
 *     current_slot becomes the target, modified = 0. A fade starting
 *     while another runs replaces it, starting from the live values.
 *     If the position is not advancing (external, stopped), a fade with
 *     length > 0 starts but holds at progress 0 until it moves.
 *   - sw_turn() on a control while a fade runs takes that control out of
 *     the fade (it stays where the hand puts it) and sets modified.
 *
 * sw_store(): copies the live values and toggles into the slot, marks it
 * stored (fade and curve unchanged), current_slot = slot, modified = 0.
 * Returns 1, so the caller knows the bank changed and needs saving.
 */
#ifndef SW_ENGINE_H
#define SW_ENGINE_H
#include <stdint.h>
#include "model.h"

#define SW_PPQN 480u
#define SW_CLK_PER_TICK (SW_PPQN / 24u)   /* clocks per MIDI clock tick */
#define SW_EXT_TIMEOUT_US 500000u
#define SW_STEP_COARSE 128                /* internal units per encoder step */
#define SW_STEP_FINE 8
#define SW_TICK_HIST 48                   /* ticks the external tempo is measured over */

enum { SW_CLOCK_INTERNAL, SW_CLOCK_EXTERNAL };

/* Emits a clock byte (F8) on the given SW_PORT_* mask. */
typedef void (*sw_clock_fn)(void *ctx, uint8_t ports, uint8_t byte);

typedef struct {
    int current_slot;     /* last recalled / stored slot, -1 = none */
    int modified;         /* live state differs from current_slot by hand */
    int armed_slot;       /* -1 = none */
    int fading;           /* a fade is running */
    int fade_slot;        /* its target slot */
    uint16_t fade_progress; /* 0..65535 */
    int pending_jump;     /* a jump waiting for its quantize boundary: slot, else -1 */
    int clock_source;     /* SW_CLOCK_* */
    int running;          /* the position is advancing */
    uint16_t bpm_x10;     /* the tempo in use (measured when external) */
    uint32_t position;    /* clocks since start */
    uint32_t cycle_clocks;/* the cycle length in clocks */
    uint32_t cycle_pos;   /* position % cycle_clocks */
} sw_status_t;

typedef struct {
    /* ---- the public state: read freely, don't write ---- */
    uint16_t live[SW_CONTROLS];     /* live positions 0..SW_VMAX */
    uint16_t toggles;               /* live toggle bits */
    /* Change flags for out.c: set by the engine whenever live[c] /
     * toggle t changes; out.c clears them when it has sent the value. */
    uint32_t ctl_dirty;
    uint16_t tog_dirty;

    /* ---- internal (exposed for sizing) ---- */
    sw_bank_t *bank;
    sw_clock_fn clock_fn;
    void *clock_ctx;
    sw_status_t st;
    uint32_t last_us, last_tick_us, tick_period_us, frac_us, clock_out_acc;
    uint32_t tick_pos;              /* position at the last external tick */
    uint32_t fade_start_pos, fade_len, jump_at;
    uint32_t fade_mask;             /* controls still in the fade */
    uint16_t fade_from[SW_CONTROLS], fade_to[SW_CONTROLS];
    uint8_t fade_curve, ext_seen;
    /* the last SW_TICK_HIST + 1 tick times: with millisecond timestamps a
     * per-tick average can't get the tempo closer than ~0.5 % */
    uint32_t tick_hist[SW_TICK_HIST + 1];
    uint8_t tick_n, tick_i;
} sw_engine_t;

/* Starts with every live value at 0, toggles off, nothing current; if slot
 * 0 is stored it is recalled as a cut (fade 0) at once. clock_fn may be
 * NULL. If slot 0 was recalled every control and toggle starts dirty
 * (all are sent); otherwise none is, and nothing is sent until a value
 * changes. */
void sw_init(sw_engine_t *e, sw_bank_t *bank, uint32_t now, sw_clock_fn clock_fn, void *ctx);

/* A new bank: cancels arm / fade / pending jump, then, if its slot 0 is
 * stored, jumps there with slot 0's own fade and curve (ignoring jump
 * quantize). Otherwise live values stay and nothing is sent. */
void sw_set_bank(sw_engine_t *e, sw_bank_t *bank, uint32_t now);

/* The current bank's config was replaced in place (editor commit): keeps
 * live values, cancels arm / fade / pending jump only if their slot is no
 * longer stored, marks every control and toggle dirty (the mapping may
 * have changed). */
void sw_bank_changed(sw_engine_t *e, uint32_t now);

void sw_midi_in(sw_engine_t *e, uint32_t now, const uint8_t *msg, int len);
void sw_tick(sw_engine_t *e, uint32_t now);

/* delta encoder steps; fine != 0 uses SW_STEP_FINE instead of
 * SW_STEP_COARSE. Clamped to 0..SW_VMAX. */
void sw_turn(sw_engine_t *e, int ctl, int delta, int fine, uint32_t now);
/* Sets a control's live value directly (editor), with the same override
 * and modified rules as sw_turn(). */
void sw_set_value(sw_engine_t *e, int ctl, uint16_t value, uint32_t now);
/* Flips (on < 0) or sets toggle t; sets modified. */
void sw_toggle(sw_engine_t *e, int t, int on);

void sw_slot_press(sw_engine_t *e, int slot, uint32_t now);
void sw_jump(sw_engine_t *e, int slot, uint32_t now);
void sw_cancel_arm(sw_engine_t *e);
int sw_store(sw_engine_t *e, int slot);

/* The status as of the last call. */
const sw_status_t *sw_status(const sw_engine_t *e);
/* The values the controls are heading to (the armed slot's, else the
 * running fade's, else the pending jump's), or NULL when there's none:
 * the rings' ghost dots. */
const uint16_t *sw_targets(const sw_engine_t *e);

#endif
