/*
 * Swell: ring and OLED drawing for the E16 front end (app.c). Pure
 * drawing: everything it shows comes in through sw_view_t, and the only
 * api calls it makes are the display and LED ones.
 */
#ifndef SW_UI_H
#define SW_UI_H
#include <stdint.h>
#include "e16_app.h"
#include "model.h"
#include "engine.h"

/* What the last touch was (the OLED's bottom band). */
enum { SW_TOUCH_NONE, SW_TOUCH_CONTROL, SW_TOUCH_TOGGLE, SW_TOUCH_SLOT, SW_TOUCH_BANK, SW_TOUCH_LAYER };

typedef struct {
    const sw_engine_t *e;
    const sw_bank_t *bank;  /* the current bank */
    int bank_index;         /* 0-15 */
    int layer;              /* 0 A / 1 B */
    int shift;              /* the Shift surface is up */
    int shift_latched;      /* ... by a Shift tap (rather than held) */
    int dirty;              /* the current bank has unsaved changes */
    uint32_t now_ms;

    int touch_kind;         /* SW_TOUCH_* */
    int touch_index;        /* control 0-31, toggle 0-15 or slot 0-14 */

    int store_slot;         /* a slot push being held in Shift mode: -1 = none */
    uint16_t store_progress;/* 0..1000 of the 600 ms hold */
    int store_done;         /* held long enough: the store happens on release */
} sw_view_t;

void ui_init(const e16_api_t *api);
/* Draws the rings (only changed LEDs are sent) and shows them. */
void ui_leds(const sw_view_t *v);
/* Draws and presents the OLED. */
void ui_oled(const sw_view_t *v);
/* Retry a leds_show() the previous frame refused (call between frames). */
void ui_leds_flush(void);

/* Small text helpers (no libc): append, return the new end (NUL kept). */
char *ui_put_s(char *p, const char *s);
char *ui_put_u(char *p, uint32_t v);
char *ui_put_u2(char *p, uint32_t v); /* two digits, zero-padded */

/* Names for the fade lengths and curves. */
extern const char *const ui_fade_names[SW_FADES];
extern const char *const ui_curve_names[SW_CURVE_COUNT];

#endif
