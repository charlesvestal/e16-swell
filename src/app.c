/*
 * Swell: a snapshot-morphing performance controller for the E16
 * (docs/manual.md is the manual, docs/design.md the design). This is
 * the E16 front end: input, files and glue around the portable engine in
 * core/ (model, store, engine, out, proto). ui.c draws.
 *
 * Play mode (Shift up):
 *   turn            control layer*16 + i (overrides a running fade)
 *   push + turn     fine steps (the push then doesn't toggle)
 *   push, release   toggle i (when there was no turn)
 * Shift held (or latched by a Shift tap):
 *   enc 1-15        slots: release < 600 ms = arm (again = jump),
 *                   hold >= 600 ms + release = store; turn = fade length,
 *                   push + turn = curve
 *   enc 16          push = layer A/B, turn = bank
 * Settings pages (core/fields.h): hold a push 1 s without turning; in
 * play mode, the knob page (that control and toggle), in Shift mode on
 * encoder 16, the bank page. A turn changes the field under it (pushed:
 * coarse, or fine for the tempo); a push on a name field opens the name
 * editor (encoders 1-8 the characters), on a blank cell of the knob page
 * shows the other half of it, and any other push or a Shift tap goes
 * back. The engine runs on meanwhile.
 * Leaving is the launcher's: hold Shift on its own for 5 s, then push
 * encoder 1 at its prompt; Swell saves what's unsaved, then exits. A
 * Shift tap latches the Shift surface, for hands-free slot work.
 *
 * Each pass: MIDI in (clock -> engine, our SysEx -> proto, its reply out
 * on USB, never dropped), the surface, sw_tick, sw_out_service, an EVENT
 * if due, drawing at ~30 Hz, then any delayed save.
 */
#include "e16_app.h"
#include "engine.h"
#include "fields.h"
#include "model.h"
#include "out.h"
#include "proto.h"
#include "store.h"
#include "ui.h"

/* ---- libc bits the compiler may call ---- */

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b) {
        while (n--)
            *a++ = *b++;
    } else {
        while (n--)
            a[n] = b[n];
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

/* ---- state ---- */

#define HOLD_STORE_MS 600u
#define HOLD_EDIT_MS 1000u      /* a push held this long, no turn: a settings page */
#define EDIT_SHOW_MS 150u       /* ... and the ring shows the hold after this */
#define FIELD_SHOW_MS 2000u     /* the header shows the field last turned this long */
#define STORE_SHOW_MS 120u      /* a slot push shows the store fill after this */
#define SHIFT_TAP_MS 350u       /* a Shift tap (nothing else touched) latches Shift */
#define BANK_SETTLE_MS 60u      /* steps on enc 1-15 right after a bank change are dropped */
#define DRAW_MS 33u
#define SAVE_QUIET_MS 2000u     /* no change for this long ... */
#define SAVE_GAP_MS 15000u      /* ... and this long since that file's last write */
#define SAVE_BUSY_RETRY_MS 1000u
#define SAVE_ERR_RETRY_MS 15000u

static const e16_api_t *api;
static sw_bank_t banks[SW_BANKS];
static sw_engine_t eng;
static sw_out_t out;
static sw_proto_t proto;
static int cur_bank, layer;
static uint8_t blob[SW_BLOB_SIZE + 1];
static uint16_t map_crc; /* the current bank's control/toggle mapping, to spot a changed one */
static uint8_t edit_gen[SW_BANKS]; /* 1-255, moved on by every change: the EVENT's dirty byte */

static const int port_map[SW_OUT_PORTS] = {E16_MIDI_TRS_1, E16_MIDI_TRS_2, E16_MIDI_USB};

/* ---- time ---- */

static uint32_t t_us, last_ms;

static uint32_t now_us(void)
{
    uint32_t ms = api->millis();
    t_us += (ms - last_ms) * 1000u;
    last_ms = ms;
    return t_us;
}

static int due(uint32_t at)
{
    return (int32_t)(last_ms - at) >= 0;
}

/* ---- MIDI out ---- */

static int write_fn(void *ctx, int port, const uint8_t *bytes, int len)
{
    (void)ctx;
    if (port < 0 || port >= SW_OUT_PORTS || len <= 0)
        return 0;
    return api->midi_write(port_map[port], bytes, (uint32_t)len) ? 1 : 0;
}

static uint32_t clock_drops;

static void clock_fn(void *ctx, uint8_t ports, uint8_t byte)
{
    (void)ctx;
    for (int p = 0; p < SW_OUT_PORTS; p++)
        if (ports >> p & 1)
            if (!api->midi_write(port_map[p], &byte, 1))
                clock_drops++;
}

/* Replies to the editor: sent on USB in order, kept until midi_write
 * takes them. The editor waits for each reply, so one is the norm; the
 * queue only fills if USB stalls, and then MIDI in waits (see the loop). */
#define RQ 4
static uint8_t rq_buf[RQ][SW_PROTO_MAX];
static int rq_len[RQ];
static int rq_head, rq_count;

static void reply_flush(void)
{
    while (rq_count) {
        if (!api->midi_write(E16_MIDI_USB, rq_buf[rq_head], (uint32_t)rq_len[rq_head]))
            return;
        rq_head = (rq_head + 1) % RQ;
        rq_count--;
    }
}

static void handle_sysex(const uint8_t *msg, int len, uint32_t now)
{
    int slot = (rq_head + rq_count) % RQ;
    int n = sw_proto_handle(&proto, msg, len, rq_buf[slot], SW_PROTO_MAX, now);
    if (n <= 0)
        return;
    rq_len[slot] = n;
    rq_count++;
    reply_flush();
}

/* ---- saving ---- */

typedef struct {
    uint8_t dirty, written;
    uint32_t change_ms, write_ms, retry_ms;
} save_t;

static save_t bank_save[SW_BANKS], state_save;
static int save_rr;

static char *bank_file(char *p, int b)
{
    p = ui_put_s(p, "bank");
    p = ui_put_u2(p, (uint32_t)b);
    return ui_put_s(p, ".bin");
}

static void log2s(const char *a, const char *b)
{
    char s[96], *p = ui_put_s(s, "swell: ");
    p = ui_put_s(p, a);
    ui_put_s(p, b);
    api->log(s);
}

static void log_err(const char *what, const char *name, int err)
{
    char s[96], *p = ui_put_s(s, "swell: ");
    p = ui_put_s(p, what);
    p = ui_put_s(p, name);
    p = ui_put_s(p, " error ");
    if (err < 0) {
        *p++ = '-';
        err = -err;
    }
    ui_put_u(p, (uint32_t)err);
    api->log(s);
}

static void mark(save_t *s)
{
    s->dirty = 1;
    s->change_ms = last_ms;
}

static uint16_t mapping_crc(const sw_bank_t *b)
{
    /* ctl and tog are adjacent; hash them separately to not rely on that */
    uint16_t a = sw_crc16((const uint8_t *)b->ctl, sizeof b->ctl);
    uint16_t t = sw_crc16((const uint8_t *)b->tog, sizeof b->tog);
    return (uint16_t)(a ^ (uint16_t)(t * 31u));
}

static void bank_dirty(int b)
{
    if (b < 0 || b >= SW_BANKS)
        return;
    mark(&bank_save[b]);
    edit_gen[b] = (uint8_t)(edit_gen[b] % 255u + 1u);
    if (b == cur_bank) {
        /* an editor commit may have changed what a control sends: then
         * the out layer must not dedupe against the old mapping */
        uint16_t c = mapping_crc(&banks[b]);
        if (c != map_crc) {
            map_crc = c;
            sw_out_forget(&out);
        }
    }
}

static int write_bank(int b)
{
    char name[16];
    bank_file(name, b);
    int n = sw_store_encode(&banks[b], blob, sizeof blob);
    if (n != SW_BLOB_SIZE)
        return E16_EARGS;
    return api->file_write(name, blob, (uint32_t)n);
}

static int write_state(void)
{
    uint8_t s[6] = {'S', 'W', 'S', '1', (uint8_t)cur_bank, (uint8_t)layer};
    return api->file_write("state.bin", s, sizeof s);
}

/* One write attempt at most per call (each blocks for tens of ms). */
static int save_one(save_t *s, int (*fn)(int), int arg, const char *name)
{
    if (!s->dirty || !due(s->retry_ms))
        return 0;
    if (last_ms - s->change_ms < SAVE_QUIET_MS)
        return 0;
    if (s->written && last_ms - s->write_ms < SAVE_GAP_MS)
        return 0;
    int r = fn(arg);
    now_us();
    if (r == 0) {
        s->dirty = 0;
        s->written = 1;
        s->write_ms = last_ms;
    } else if (r == E16_EBUSY) {
        s->retry_ms = last_ms + SAVE_BUSY_RETRY_MS;
    } else {
        log_err("save ", name, r);
        s->retry_ms = last_ms + SAVE_ERR_RETRY_MS;
    }
    return 1;
}

static int state_fn(int unused)
{
    (void)unused;
    return write_state();
}

static void save_service(void)
{
    char name[16];
    for (int k = 0; k < SW_BANKS; k++) {
        int b = (save_rr + k) % SW_BANKS;
        bank_file(name, b);
        if (save_one(&bank_save[b], write_bank, b, name)) {
            save_rr = (b + 1) % SW_BANKS;
            return;
        }
    }
    save_one(&state_save, state_fn, 0, "state.bin");
}

/* ---- loading ---- */

static int read_file(const char *path, uint8_t *buf, uint32_t cap)
{
    int fd = api->file_open(path, E16_FILE_READ);
    if (fd < 0)
        return fd;
    uint32_t got = 0;
    while (got < cap) {
        int r = api->file_read(fd, buf + got, cap - got);
        if (r <= 0) {
            if (r < 0)
                got = 0;
            break;
        }
        got += (uint32_t)r;
    }
    api->file_close(fd);
    return (int)got;
}

static void load_all(void)
{
    int loaded = 0, missing = 0, bad = 0;
    for (int b = 0; b < SW_BANKS; b++) {
        char path[40], *p = ui_put_s(path, "/apps/swell/");
        bank_file(p, b);
        int n = read_file(path, blob, sizeof blob);
        int r = n > 0 ? sw_store_decode(&banks[b], blob, (uint32_t)n) : -100;
        if (r == 0) {
            loaded++;
            bank_save[b].written = 1; /* (as far as the rate goes) */
            bank_save[b].write_ms = last_ms - SAVE_GAP_MS;
            continue;
        }
        sw_bank_default(&banks[b], b);
        if (n <= 0) {
            missing++;
        } else {
            bad++;
            log_err("bad bank file, using the default: ", path, r);
        }
    }
    char s[96], *p = ui_put_s(s, "swell: banks loaded ");
    p = ui_put_u(p, (uint32_t)loaded);
    p = ui_put_s(p, ", default ");
    p = ui_put_u(p, (uint32_t)(missing + bad));
    p = ui_put_s(p, " (missing ");
    p = ui_put_u(p, (uint32_t)missing);
    p = ui_put_s(p, ", bad ");
    p = ui_put_u(p, (uint32_t)bad);
    ui_put_s(p, ")");
    api->log(s);

    int n = read_file("/apps/swell/state.bin", blob, 16);
    if (n == 6 && blob[0] == 'S' && blob[1] == 'W' && blob[2] == 'S' && blob[3] == '1') {
        cur_bank = blob[4] < SW_BANKS ? blob[4] : 0;
        layer = blob[5] ? 1 : 0;
    } else if (n > 0) {
        log2s("bad state.bin, ", "starting at bank 1, layer A");
    }
}

/* ---- bank and layer ---- */

static int touch_kind = SW_TOUCH_NONE, touch_index;
static uint32_t settle_until;

static void select_bank(int b, uint32_t now)
{
    if (b < 0 || b >= SW_BANKS)
        return;
    cur_bank = b;
    sw_set_bank(&eng, &banks[b], now);
    sw_out_forget(&out); /* a new mapping */
    map_crc = mapping_crc(&banks[b]);
    mark(&state_save);
    if (touch_kind == SW_TOUCH_CONTROL || touch_kind == SW_TOUCH_TOGGLE || touch_kind == SW_TOUCH_SLOT)
        touch_kind = SW_TOUCH_BANK;
}

static void set_layer(int l)
{
    layer = l ? 1 : 0;
    mark(&state_save);
}

/* proto host callbacks */
static void h_bank_dirty(void *u, int b)
{
    (void)u;
    bank_dirty(b);
}

static int h_is_dirty(void *u, int b)
{
    (void)u;
    /* unsaved: the edit count, so the editor sees each change, not just
     * the first (it re-reads the bank when this byte changes) */
    return b >= 0 && b < SW_BANKS && bank_save[b].dirty ? edit_gen[b] : 0;
}

static void h_select_bank(void *u, int b, uint32_t now)
{
    (void)u;
    select_bank(b, now);
}

static void h_set_layer(void *u, int l)
{
    (void)u;
    set_layer(l);
}

/* ---- leaving ---- */

/* Save everything unsaved at once, as far as the erase budget allows
 * (the launcher is about to end the app). */
static void save_now(void)
{
    for (int b = 0; b < SW_BANKS; b++)
        if (bank_save[b].dirty) {
            int r = write_bank(b);
            now_us();
            if (r == 0) {
                bank_save[b].dirty = 0;
                bank_save[b].written = 1;
                bank_save[b].write_ms = last_ms;
            } else {
                char name[16];
                bank_file(name, b);
                log_err("not saved: ", name, r);
            }
        }
    if (state_save.dirty && write_state() == 0) {
        state_save.dirty = 0;
        state_save.written = 1;
        state_save.write_ms = last_ms;
    }
}


/* ---- the surface ---- */

static uint8_t down[16], press_shift[16], turned[16], consumed[16];
static uint32_t press_ms[16];
static int shift_raw, shift_latched, shift_used;
static uint32_t shift_t0;

static int shift_mode(void)
{
    return shift_raw || shift_latched;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void slot_edit(int s, int d, int curve)
{
    sw_snapshot_t *sn = &banks[cur_bank].snap[s];
    if (curve) {
        int c = clampi(sn->curve + d, 0, SW_CURVE_COUNT - 1);
        if (c == sn->curve)
            return;
        sn->curve = (uint8_t)c;
    } else {
        int f = clampi(sn->fade + d, 0, SW_FADES - 1);
        if (f == sn->fade)
            return;
        sn->fade = (uint8_t)f;
    }
    bank_dirty(cur_bank);
}

/* ---- the settings pages ---- */

static int page = SW_PAGE_NONE, page_ctl, page_tog;
static int name_field = -1;            /* the name editor's field, or -1 */
static int page_half;                  /* the half of the grid on the OLED */
static int last_field = -1;
static uint32_t last_field_ms;

static sw_ftarget_t page_target(void)
{
    sw_ftarget_t t = {&banks[cur_bank], page_ctl, page_tog};
    return t;
}

static void page_open(int pg, int enc)
{
    page = pg;
    page_ctl = pg == SW_PAGE_KNOB ? layer * SW_ENCODERS + enc : 0;
    page_tog = pg == SW_PAGE_KNOB ? enc : 0;
    name_field = -1;
    last_field = -1;
    page_half = 0;
    api->log(pg == SW_PAGE_KNOB ? "swell: knob page" : "swell: bank page");
}

static void page_close(void)
{
    if (name_field >= 0) {
        name_field = -1;
        return;
    }
    page = SW_PAGE_NONE;
}

static void page_turn(int e, int d, int alt)
{
    sw_ftarget_t t = page_target();
    int f, changed;
    if (name_field >= 0) {
        if (e >= SW_NAME_LEN)
            return;
        f = name_field;
        changed = sw_name_turn(sw_field_name(&t, f), e, d);
    } else {
        f = sw_field_at(page, e);
        if (f < 0)
            return;
        page_half = e / 8; /* the OLED shows the half being turned */
        changed = sw_field_turn(&t, f, d, alt);
    }
    last_field = f;
    last_field_ms = last_ms;
    if (changed)
        bank_dirty(cur_bank);
}

static void page_push(int e)
{
    int f = sw_field_at(page, e);
    if (name_field < 0 && sw_field_is_name(f)) {
        name_field = f;
        last_field = f;
        last_field_ms = last_ms;
    } else if (name_field < 0 && f < 0 && page == SW_PAGE_KNOB) {
        /* a blank cell: the other half of the grid on the OLED */
        page_half = !page_half;
        last_field = -1;
    } else {
        page_close();
    }
}

/* A push held long enough with no turn opens a page. */
static void page_holds(void)
{
    for (int e = 0; e < E16_ENC_COUNT; e++) {
        if (!down[e] || turned[e] || consumed[e] || last_ms - press_ms[e] < HOLD_EDIT_MS)
            continue;
        if (!press_shift[e]) {
            page_open(SW_PAGE_KNOB, e);
        } else if (e == 15) {
            page_open(SW_PAGE_BANK, e);
        } else {
            continue;
        }
        consumed[e] = 1; /* its release is not a toggle, layer or close */
        return;
    }
}

static void surface(uint32_t now)
{
    int sh = api->shift_pressed();
    if (sh && !shift_raw) {
        shift_t0 = last_ms;
        shift_used = 0;
    } else if (!sh && shift_raw) {
        if (!shift_used && last_ms - shift_t0 < SHIFT_TAP_MS) {
            if (page)
                page_close(); /* a tap backs out of a page instead */
            else
                shift_latched = !shift_latched;
        }
    }
    shift_raw = sh;
    int smode = shift_mode();
    int settle = !due(settle_until);
    int bank_moved = 0;

    for (int e = 0; e < E16_ENC_COUNT; e++) {
        int d = api->enc_read(e);
        int dn = api->enc_pressed(e) ? 1 : 0;

        if (dn && !down[e]) {
            down[e] = 1;
            press_ms[e] = last_ms;
            press_shift[e] = (uint8_t)smode;
            turned[e] = consumed[e] = 0;
            if (shift_raw)
                shift_used = 1;
        }

        if (page) {
            if (d) {
                if (shift_raw)
                    shift_used = 1;
                if (down[e])
                    turned[e] = 1;
                page_turn(e, d, down[e]);
            }
            if (!dn && down[e]) {
                down[e] = 0;
                if (!consumed[e] && !turned[e])
                    page_push(e);
            }
            continue;
        }

        if (d && settle && e < 15)
            d = 0;
        /* Shift + push 16 is the layer: a click of turn that comes with
         * the push mustn't change the bank (or cancel the push) */
        if (d && e == 15 && down[e] && press_shift[e])
            d = 0;
        if (d) {
            if (shift_raw)
                shift_used = 1;
            int m = down[e] ? press_shift[e] : smode;
            if (down[e])
                turned[e] = 1;
            if (!m) {
                int c = layer * SW_ENCODERS + e;
                sw_turn(&eng, c, d, down[e], now);
                touch_kind = SW_TOUCH_CONTROL;
                touch_index = c;
            } else if (e < SW_SLOTS) {
                slot_edit(e, d > 0 ? 1 : -1, down[e]);
                touch_kind = SW_TOUCH_SLOT;
                touch_index = e;
            } else {
                int b = clampi(cur_bank + (d > 0 ? 1 : -1), 0, SW_BANKS - 1);
                if (b != cur_bank) {
                    select_bank(b, now);
                    bank_moved = 1;
                }
                touch_kind = SW_TOUCH_BANK;
            }
        }

        if (!dn && down[e]) {
            down[e] = 0;
            if (consumed[e] || turned[e])
                continue;
            if (press_shift[e]) {
                if (e < SW_SLOTS && last_ms - press_ms[e] >= HOLD_STORE_MS) {
                    /* hold to store, on release: a turn before then makes
                     * it a curve edit instead, however long the push */
                    if (sw_store(&eng, e))
                        bank_dirty(cur_bank);
                    touch_kind = SW_TOUCH_SLOT;
                    touch_index = e;
                } else if (e < SW_SLOTS) {
                    sw_slot_press(&eng, e, now);
                    touch_kind = SW_TOUCH_SLOT;
                    touch_index = e;
                } else {
                    set_layer(!layer);
                    touch_kind = SW_TOUCH_LAYER;
                }
            } else {
                sw_toggle(&eng, e, -1);
                touch_kind = SW_TOUCH_TOGGLE;
                touch_index = e;
            }
        }
    }

    if (!page)
        page_holds();

    if (bank_moved) {
        /* steps that came in on the slot encoders meanwhile mean nothing */
        for (int e = 0; e < SW_SLOTS; e++)
            (void)api->enc_read(e);
        settle_until = last_ms + BANK_SETTLE_MS;
    }
}

/* ---- drawing ---- */

static void draw(void)
{
    sw_view_t v;
    v.e = &eng;
    v.bank = &banks[cur_bank];
    v.bank_index = cur_bank;
    v.layer = layer;
    v.shift = shift_mode();
    v.shift_latched = shift_latched && !shift_raw;
    v.dirty = bank_save[cur_bank].dirty;
    v.now_ms = last_ms;
    v.touch_kind = touch_kind;
    v.touch_index = touch_index;
    v.store_slot = -1;
    v.store_progress = 0;
    v.store_done = 0;
    v.page = page;
    v.page_ctl = page_ctl;
    v.page_tog = page_tog;
    v.name_field = name_field;
    v.page_half = page_half;
    v.last_field = last_field >= 0 && last_ms - last_field_ms < FIELD_SHOW_MS ? last_field : -1;
    v.hold_enc = -1;
    v.hold_progress = 0;
    if (!page)
        for (int e = 0; e < E16_ENC_COUNT; e++)
            if (down[e] && !turned[e] && !consumed[e] && (!press_shift[e] || e == 15) &&
                last_ms - press_ms[e] >= EDIT_SHOW_MS) {
                /* in Shift mode the slots' own store fill shows instead */
                v.hold_enc = e;
                v.hold_progress = (uint16_t)((last_ms - press_ms[e]) * 1000u / HOLD_EDIT_MS);
                break;
            }
    for (int e = 0; e < SW_SLOTS; e++)
        if (down[e] && press_shift[e] && !turned[e] && !consumed[e] &&
            last_ms - press_ms[e] >= STORE_SHOW_MS) {
            uint32_t t = last_ms - press_ms[e];
            v.store_slot = e;
            v.store_done = t >= HOLD_STORE_MS;
            v.store_progress = (uint16_t)(t >= HOLD_STORE_MS ? 1000u : t * 1000u / HOLD_STORE_MS);
            break;
        }
    ui_leds(&v);
    ui_oled(&v);
}

/* ---- main ---- */

static uint8_t ev_buf[SW_PROTO_MAX];

int app_main(const e16_api_t *a)
{
    api = a;
    last_ms = api->millis();
    t_us = 0;
    ui_init(api);

    load_all();
    uint32_t now = now_us();
    sw_init(&eng, &banks[cur_bank], now, clock_fn, 0);
    sw_out_init(&out, now);
    map_crc = mapping_crc(&banks[cur_bank]);

    sw_proto_host_t host;
    host.banks = banks;
    host.engine = &eng;
    host.current_bank = &cur_bank;
    host.layer = &layer;
    host.user = 0;
    host.bank_dirty = h_bank_dirty;
    host.is_dirty = h_is_dirty;
    host.select_bank = h_select_bank;
    host.set_layer = h_set_layer;
    sw_proto_init(&proto, &host);

    /* whatever was pushed or turned on the way in doesn't count */
    for (int e = 0; e < E16_ENC_COUNT; e++) {
        (void)api->enc_read(e);
        down[e] = api->enc_pressed(e) ? 1 : 0;
        consumed[e] = down[e];
    }
    shift_raw = api->shift_pressed();
    shift_used = 1;

    api->log("swell: Shift (hold or tap) = slots; hold Shift alone 5 s to leave");

    uint32_t t_draw = last_ms - DRAW_MS;
    for (;;) {
        now = now_us();

        /* 1. MIDI in; a reply still owed goes first */
        reply_flush();
        e16_midi_msg_t m;
        while (rq_count < RQ && api->midi_read(&m)) {
            if (m.len == 0)
                continue;
            if (m.data[0] == 0xF0) {
                if (m.port == E16_MIDI_USB)
                    handle_sysex(m.data, (int)m.len, now_us());
            } else if (m.len <= 3) {
                sw_midi_in(&eng, now_us(), m.data, (int)m.len);
            }
        }

        /* 2. the surface */
        now = now_us();
        surface(now);

        /* 3, 4 */
        now = now_us();
        sw_tick(&eng, now);
        sw_out_service(&out, &eng, now, write_fn, 0);

        /* 5. an EVENT for the editor's live view */
        int n = sw_proto_event(&proto, last_ms, ev_buf, sizeof ev_buf);
        if (n > 0) {
            if (rq_count || !api->midi_write(E16_MIDI_USB, ev_buf, (uint32_t)n))
                sw_proto_event_unsent(&proto);
        }

        /* 6. ~30 Hz drawing */
        if (last_ms - t_draw >= DRAW_MS) {
            t_draw = last_ms;
            draw();
        } else {
            ui_leds_flush();
        }

        /* 7. delayed saves; and leaving, when the user confirmed the
         * launcher's leave prompt: everything unsaved now, then exit */
        if (E16_API_HAS(api, exit_pending) && api->exit_pending()) {
            save_now();
            api->exit(0);
        }
        save_service();
    }
}
