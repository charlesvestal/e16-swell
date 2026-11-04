/*
 * Swell core: the data model. A snapshot-morphing performance controller
 * (docs/design.md).
 *
 * Portable C99: no libc beyond <stdint.h>, no malloc, no hardware.
 *
 * A bank holds the mapping of 32 continuous controls (2 layers x 16
 * encoders) and 16 toggles (the encoder pushes), plus 15 snapshot slots.
 * Values are held internally as 14-bit positions 0..SW_VMAX regardless of
 * the control's output type; out.c maps a position onto the control's
 * min..max in output units.
 */
#ifndef SW_MODEL_H
#define SW_MODEL_H
#include <stdint.h>

#define SW_LAYERS 2
#define SW_ENCODERS 16
#define SW_CONTROLS (SW_LAYERS * SW_ENCODERS) /* control c = layer * 16 + encoder */
#define SW_TOGGLES 16                         /* toggle t = encoder t's push */
#define SW_SLOTS 15                           /* snapshot slots per bank */
#define SW_BANKS 16
#define SW_NAME_LEN 8                         /* names: NUL-padded, not NUL-terminated when full */
#define SW_VMAX 16383u                        /* the top internal position */

/* Output types. */
enum { SW_T_OFF, SW_T_CC7, SW_T_CC14, SW_T_NRPN7, SW_T_NRPN14, SW_T_COUNT };

/* Output port mask bits (also out.c's port numbers 0..2). */
#define SW_PORT_TRS1 1u
#define SW_PORT_TRS2 2u
#define SW_PORT_USB 4u
#define SW_PORT_ALL 7u

/* Fade curves. HOLD: the value stays where it was until the fade's end,
 * then cuts to the target (a delayed cut). */
enum { SW_CURVE_LINEAR, SW_CURVE_EASE, SW_CURVE_S, SW_CURVE_HOLD, SW_CURVE_COUNT };

/* Fade lengths: an index into sw_fade_beats[] = {0, 1, 2, 4, 8, 16, 32}
 * (cut, 1 beat, 2 beats, 1 bar, 2 bars, 4 bars, 8 bars; 4/4 bars). */
#define SW_FADES 7
extern const uint8_t sw_fade_beats[SW_FADES];

/* Jump quantize. */
enum { SW_JQ_NONE, SW_JQ_BEAT, SW_JQ_BAR, SW_JQ_COUNT };

/* Allowed cycle lengths in bars. */
#define SW_CYCLES 5
extern const uint8_t sw_cycle_bars[SW_CYCLES]; /* {1, 2, 4, 8, 16} */

typedef struct {
    char name[SW_NAME_LEN];
    uint8_t type;     /* SW_T_* */
    uint8_t ports;    /* SW_PORT_* mask */
    uint8_t channel;  /* 0-15 */
    uint16_t number;  /* CC7: 0-127; CC14: 0-31 (LSB at number+32); NRPN: 0-16383 */
    uint16_t min;     /* output units: 0-127 for 7-bit types, 0-16383 for 14-bit; */
    uint16_t max;     /* min > max is allowed (inverted) */
} sw_control_t;

typedef struct {
    char name[SW_NAME_LEN];
    uint8_t ports;    /* SW_PORT_* mask; 0 = the toggle sends nothing */
    uint8_t channel;  /* 0-15 */
    uint8_t cc;       /* 0-127 */
    uint8_t off, on;  /* the CC values sent for off / on, 0-127 */
} sw_toggle_t;

typedef struct {
    uint8_t stored;   /* 0 = empty slot */
    uint8_t fade;     /* index into sw_fade_beats[] */
    uint8_t curve;    /* SW_CURVE_* */
    uint16_t toggles; /* bit t = toggle t on */
    uint16_t value[SW_CONTROLS]; /* 0..SW_VMAX */
} sw_snapshot_t;

typedef struct {
    char name[SW_NAME_LEN];
    uint8_t cycle;      /* index into sw_cycle_bars[] */
    uint8_t jump_quant; /* SW_JQ_* */
    uint8_t clock_out;  /* SW_PORT_* mask: where the internal clock is sent (0 = nowhere) */
    uint16_t bpm_x10;   /* internal tempo, 400-3000 (40.0-300.0 BPM) */
    sw_control_t ctl[SW_CONTROLS];
    sw_toggle_t tog[SW_TOGGLES];
    sw_snapshot_t snap[SW_SLOTS];
} sw_bank_t;

/* The factory bank `index` (0-based): named "BANK nn"; layer A sends CC
 * 20-35, layer B CC 40-55, both CC7 0-127 on channel 1 to TRS 1 + USB;
 * toggles CC 80-95 (off 0, on 127) to the same ports; cycle 4 bars, no
 * jump quantize, clock out off, 120.0 BPM; every slot empty with fade
 * 1 bar, linear. Controls are named "A1".."A16", "B1".."B16"; toggles
 * "T1".."T16". */
void sw_bank_default(sw_bank_t *b, int index);

/* Clamp every field of b into its legal range (used after decoding and
 * after an editor write); returns the number of fields it had to change. */
int sw_bank_sanitize(sw_bank_t *b);

#endif
