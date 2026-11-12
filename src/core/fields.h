/*
 * Swell core: the settings pages on the E16 (docs/design.md, "Setting up
 * on the E16"). Each page is a 4x4 grid of fields, one per encoder: the
 * knob page (one control and the toggle on the same encoder) and the bank
 * page. This file knows what each field is, how a turn changes it and how
 * it reads; app.c and ui.c do the gestures and the drawing.
 *
 * Portable C99, no libc, no hardware. Every change goes through
 * sw_bank_sanitize, so a bank stays legal whatever the turns.
 */
#ifndef SW_FIELDS_H
#define SW_FIELDS_H
#include <stdint.h>
#include "model.h"

enum { SW_PAGE_NONE, SW_PAGE_KNOB, SW_PAGE_BANK };

enum {
    /* the knob page: control ctl */
    SW_F_TYPE, SW_F_CH, SW_F_NUM, SW_F_PORT, SW_F_MIN, SW_F_MAX, SW_F_NAME,
    /* ... and toggle tog */
    SW_F_TCC, SW_F_TCH, SW_F_TPORT, SW_F_TOFF, SW_F_TON, SW_F_TNAME,
    /* the bank page */
    SW_F_CYCLE, SW_F_JUMPQ, SW_F_BPM, SW_F_CLOCK, SW_F_ALLCH, SW_F_ALLPORT, SW_F_BNAME,
    SW_F_COUNT
};

/* What a page's field edits: the bank, and on the knob page a control
 * (0-31) and a toggle (0-15). */
typedef struct {
    sw_bank_t *bank;
    int ctl, tog;
} sw_ftarget_t;

/* The field on encoder enc (0-15) of a page, or -1 for an empty cell. */
int sw_field_at(int page, int enc);

/* A name field (NAME, TNAME, BNAME): pushing it opens the name editor. */
int sw_field_is_name(int field);

/* The 4-character label ("TYPE") and the long one for the header
 * ("type"). */
const char *sw_field_label(int field);
const char *sw_field_long(int field);

/* The value as text: short (<= 5 characters, for the grid) or long (for
 * the header). out must hold 24 bytes. */
void sw_field_text(const sw_ftarget_t *t, int field, int long_form, char *out);

/* Where the value sits in its range, for the ring: *pos 0..*hi. Returns 0
 * for a field with no position (a name, or ALL with nothing to set). */
int sw_field_pos(const sw_ftarget_t *t, int field, int *pos, int *hi);

/* Turn a field by delta steps; alt (the encoder pushed while turning) is
 * coarse for the big ranges and fine for the tempo. Returns 1 if the bank
 * changed. */
int sw_field_turn(const sw_ftarget_t *t, int field, int delta, int alt);

/* The name a name field edits (SW_NAME_LEN bytes, NUL-padded). */
char *sw_field_name(const sw_ftarget_t *t, int field);

/* Turn character pos (0-7) of a name through the character set (space,
 * A-Z, 0-9, a few signs, a-z; wrapping). Spaces at the end become NUL
 * padding. Returns 1 if the name changed. */
int sw_name_turn(char *name, int pos, int delta);

#endif
