/*
 * Host tests for the Swell core (src/core: model, store, engine,
 * out). Plain C; prints "ALL PASSED" and exits 0 on success.
 *
 * The engine runs on virtual time (microseconds): run_to() calls sw_tick
 * every `step` us, and ext_ticks() sends F8s at a given tempo with a tick
 * in between each, as the app's loop would.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "store.h"
#include "engine.h"
#include "out.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                          \
    do {                                                           \
        g_checks++;                                                \
        if (!(cond)) {                                             \
            g_fail++;                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
        }                                                          \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ---- the engine harness ---- */

static sw_bank_t B, B2;
static sw_engine_t E;
static sw_engine_t *e = &E;
static uint32_t T;     /* virtual now */
static int n_f8;       /* clock bytes sent by the engine */
static uint8_t f8_ports;

static void clock_cb(void *ctx, uint8_t ports, uint8_t byte)
{
    (void)ctx;
    if (byte == 0xF8)
        n_f8++;
    f8_ports = ports;
}

static void midi1(uint8_t b)
{
    sw_midi_in(e, T, &b, 1);
}

/* Tick every `step` us until T reaches `to`. */
static void run_to(uint32_t to, uint32_t step)
{
    while ((int32_t)(to - T) > 0) {
        uint32_t d = to - T;
        T += d < step ? d : step;
        sw_tick(e, T);
    }
}

/* Store slot s with every control at v and toggles tg. */
static void make_slot(sw_bank_t *b, int s, uint16_t v, uint16_t tg, int fade, int curve)
{
    b->snap[s].stored = 1;
    b->snap[s].fade = (uint8_t)fade;
    b->snap[s].curve = (uint8_t)curve;
    b->snap[s].toggles = tg;
    for (int c = 0; c < SW_CONTROLS; c++)
        b->snap[s].value[c] = v;
}

static void fresh(void)
{
    sw_bank_default(&B, 0);
    T = 1000;
    n_f8 = 0;
    f8_ports = 0;
    sw_init(e, &B, T, clock_cb, 0);
}

static const sw_status_t *st(void)
{
    return sw_status(e);
}

/* ---- model ---- */

static void test_model(void)
{
    CHECK(sw_fade_beats[0] == 0 && sw_fade_beats[3] == 4 && sw_fade_beats[6] == 32, "fade table");
    CHECK(sw_cycle_bars[0] == 1 && sw_cycle_bars[4] == 16, "cycle table");
    sw_bank_default(&B, 3);
    CHECK(!memcmp(B.name, "BANK 04", 8), "bank name %.8s", B.name);
    CHECK(B.cycle == 2 && B.jump_quant == SW_JQ_NONE && B.clock_out == 0 && B.bpm_x10 == 1200,
          "bank settings");
    CHECK(!memcmp(B.ctl[0].name, "A1\0\0\0\0\0\0", 8) && !memcmp(B.ctl[31].name, "B16\0\0\0\0\0", 8),
          "control names");
    CHECK(!memcmp(B.tog[15].name, "T16", 4), "toggle name");
    CHECK(B.ctl[0].number == 20 && B.ctl[15].number == 35 && B.ctl[16].number == 40 &&
              B.ctl[31].number == 55, "control numbers");
    CHECK(B.ctl[5].type == SW_T_CC7 && B.ctl[5].ports == (SW_PORT_TRS1 | SW_PORT_USB) &&
              B.ctl[5].channel == 0 && B.ctl[5].min == 0 && B.ctl[5].max == 127, "control mapping");
    CHECK(B.tog[0].cc == 80 && B.tog[15].cc == 95 && B.tog[3].on == 127 && B.tog[3].off == 0 &&
              B.tog[3].ports == 5, "toggles");
    CHECK(!B.snap[14].stored && B.snap[14].fade == 3 && B.snap[14].curve == SW_CURVE_LINEAR, "slots");
    CHECK(sw_bank_sanitize(&B) == 0, "default is clean");

    /* sanitize clamps */
    B.cycle = 9;
    B.jump_quant = 7;
    B.clock_out = 0xFF;
    B.bpm_x10 = 10;
    B.ctl[0].type = 9;
    B.ctl[1].type = SW_T_CC7;
    B.ctl[1].number = 300;
    B.ctl[1].max = 200;
    B.ctl[2].type = SW_T_CC14;
    B.ctl[2].number = 40;
    B.ctl[2].max = 20000;
    B.ctl[3].channel = 16;
    B.tog[0].cc = 200;
    B.tog[0].on = 128;
    B.snap[0].fade = 7;
    B.snap[0].curve = 4;
    B.snap[0].stored = 5;
    B.snap[0].value[3] = 20000;
    B.name[1] = 1;
    int n = sw_bank_sanitize(&B);
    CHECK(n == 17, "sanitize count %d", n);
    CHECK(B.cycle == 4 && B.jump_quant == 2 && B.clock_out == 7 && B.bpm_x10 == 400, "bank clamps");
    CHECK(B.ctl[0].type == SW_T_OFF, "type -> OFF");
    CHECK(B.ctl[1].number == 127 && B.ctl[1].max == 127, "cc7 clamps");
    CHECK(B.ctl[2].number == 31 && B.ctl[2].max == 16383, "cc14 clamps");
    CHECK(B.ctl[3].channel == 15, "channel");
    CHECK(B.tog[0].cc == 127 && B.tog[0].on == 127, "toggle clamps");
    CHECK(B.snap[0].fade == 6 && B.snap[0].curve == 3 && B.snap[0].stored == 1 &&
              B.snap[0].value[3] == SW_VMAX, "slot clamps");
    CHECK(B.name[1] == ' ', "name char");
    CHECK(sw_bank_sanitize(&B) == 0, "idempotent");
    B.bpm_x10 = 4000;
    CHECK(sw_bank_sanitize(&B) == 1 && B.bpm_x10 == 3000, "bpm high");
}

/* ---- store ---- */

static void random_bank(sw_bank_t *b)
{
    sw_bank_default(b, (int)(rnd() % 16));
    for (int i = 0; i < SW_NAME_LEN; i++)
        b->name[i] = (char)(0x20 + rnd() % 95);
    b->cycle = (uint8_t)(rnd() % SW_CYCLES);
    b->jump_quant = (uint8_t)(rnd() % SW_JQ_COUNT);
    b->clock_out = (uint8_t)(rnd() % 8);
    b->bpm_x10 = (uint16_t)(400 + rnd() % 2601);
    for (int c = 0; c < SW_CONTROLS; c++) {
        sw_control_t *k = &b->ctl[c];
        k->type = (uint8_t)(rnd() % SW_T_COUNT);
        k->ports = (uint8_t)(rnd() % 8);
        k->channel = (uint8_t)(rnd() % 16);
        uint16_t nh = k->type == SW_T_CC7 ? 128 : k->type == SW_T_CC14 ? 32 : 16384;
        uint16_t vh = (k->type == SW_T_CC7 || k->type == SW_T_NRPN7) ? 128 : 16384;
        k->number = (uint16_t)(rnd() % nh);
        k->min = (uint16_t)(rnd() % vh);
        k->max = (uint16_t)(rnd() % vh);
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        sw_toggle_t *g = &b->tog[t];
        g->ports = (uint8_t)(rnd() % 8);
        g->channel = (uint8_t)(rnd() % 16);
        g->cc = (uint8_t)(rnd() % 128);
        g->off = (uint8_t)(rnd() % 128);
        g->on = (uint8_t)(rnd() % 128);
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        sw_snapshot_t *n = &b->snap[s];
        n->stored = (uint8_t)(rnd() % 2);
        n->fade = (uint8_t)(rnd() % SW_FADES);
        n->curve = (uint8_t)(rnd() % SW_CURVE_COUNT);
        n->toggles = (uint16_t)rnd();
        for (int c = 0; c < SW_CONTROLS; c++)
            n->value[c] = (uint16_t)(rnd() % (SW_VMAX + 1));
    }
}

static int bank_eq(const sw_bank_t *a, const sw_bank_t *b)
{
    if (memcmp(a->name, b->name, 8) || a->cycle != b->cycle || a->jump_quant != b->jump_quant ||
        a->clock_out != b->clock_out || a->bpm_x10 != b->bpm_x10)
        return 0;
    for (int c = 0; c < SW_CONTROLS; c++) {
        const sw_control_t *x = &a->ctl[c], *y = &b->ctl[c];
        if (memcmp(x->name, y->name, 8) || x->type != y->type || x->ports != y->ports ||
            x->channel != y->channel || x->number != y->number || x->min != y->min || x->max != y->max)
            return 0;
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        const sw_toggle_t *x = &a->tog[t], *y = &b->tog[t];
        if (memcmp(x->name, y->name, 8) || x->ports != y->ports || x->channel != y->channel ||
            x->cc != y->cc || x->off != y->off || x->on != y->on)
            return 0;
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        const sw_snapshot_t *x = &a->snap[s], *y = &b->snap[s];
        if (x->stored != y->stored || x->fade != y->fade || x->curve != y->curve ||
            x->toggles != y->toggles || memcmp(x->value, y->value, sizeof x->value))
            return 0;
    }
    return 1;
}

static void test_store(void)
{
    static uint8_t blob[SW_BLOB_SIZE + 4], blob2[SW_BLOB_SIZE];
    CHECK(sw_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "crc check value");

    for (int r = 0; r < 50; r++) {
        random_bank(&B);
        CHECK(sw_store_encode(&B, blob, sizeof blob) == SW_BLOB_SIZE, "encode");
        memset(&B2, 0xA5, sizeof B2);
        CHECK(sw_store_decode(&B2, blob, SW_BLOB_SIZE) == 0, "decode");
        CHECK(bank_eq(&B, &B2), "round trip %d", r);
        CHECK(sw_store_encode(&B2, blob2, sizeof blob2) == SW_BLOB_SIZE &&
                  !memcmp(blob, blob2, SW_BLOB_SIZE), "re-encode identical");
    }

    /* exact layout spots */
    sw_bank_default(&B, 0);
    B.bpm_x10 = 0x0456;
    B.ctl[1].number = 0x0102;
    B.ctl[1].min = 3;
    B.ctl[1].max = 0x0405;
    B.tog[2].cc = 77;
    B.snap[1].stored = 1;
    B.snap[1].toggles = 0xBEEF & 0xFFFF;
    B.snap[1].value[31] = 0x1234;
    sw_store_encode(&B, blob, SW_BLOB_SIZE);
    CHECK(!memcmp(blob, "SWB1BANK 01\0", 12), "magic + name");
    CHECK(blob[12] == 2 && blob[13] == 0 && blob[14] == 0 && blob[15] == 0, "settings");
    CHECK(blob[16] == 0x56 && blob[17] == 0x04, "bpm LE");
    CHECK(!memcmp(blob + 18, "A1", 2) && blob[18 + 8] == SW_T_CC7 && blob[18 + 9] == 5, "ctl 0");
    uint8_t *c1 = blob + 18 + 18;
    CHECK(c1[11] == 0 && c1[12] == 0x02 && c1[13] == 0x01 && c1[14] == 3 && c1[15] == 0 &&
              c1[16] == 0x05 && c1[17] == 0x04, "ctl 1 numbers");
    uint8_t *t2 = blob + 594 + 2 * 14;
    CHECK(!memcmp(t2, "T3", 2) && t2[8] == 5 && t2[10] == 77 && t2[12] == 127 && t2[13] == 0, "tog 2");
    uint8_t *s1 = blob + 818 + 70;
    CHECK(s1[0] == 1 && s1[1] == 3 && s1[2] == 0 && s1[3] == 0 && s1[4] == 0xEF && s1[5] == 0xBE,
          "slot 1 head");
    CHECK(s1[6 + 62] == 0x34 && s1[6 + 63] == 0x12, "slot 1 value 31");
    uint16_t crc = sw_crc16(blob, SW_BLOB_CRC_OFS);
    CHECK(blob[1868] == (crc & 0xFF) && blob[1869] == (crc >> 8), "crc LE");

    /* errors leave the bank untouched */
    CHECK(sw_store_encode(&B, blob2, SW_BLOB_SIZE - 1) == SW_STORE_ESIZE, "encode cap");
    random_bank(&B2);
    sw_bank_t keep = B2;
    CHECK(sw_store_decode(&B2, blob, SW_BLOB_SIZE - 1) == SW_STORE_ESIZE, "short");
    CHECK(sw_store_decode(&B2, blob, SW_BLOB_SIZE + 1) == SW_STORE_ESIZE, "long");
    memcpy(blob2, blob, SW_BLOB_SIZE);
    blob2[3] = '2';
    CHECK(sw_store_decode(&B2, blob2, SW_BLOB_SIZE) == SW_STORE_EMAGIC, "magic");
    memcpy(blob2, blob, SW_BLOB_SIZE);
    blob2[1000] ^= 0x40;
    CHECK(sw_store_decode(&B2, blob2, SW_BLOB_SIZE) == SW_STORE_ECRC, "crc data");
    memcpy(blob2, blob, SW_BLOB_SIZE);
    blob2[1869] ^= 1;
    CHECK(sw_store_decode(&B2, blob2, SW_BLOB_SIZE) == SW_STORE_ECRC, "crc field");
    CHECK(!memcmp(&keep, &B2, sizeof keep), "bank untouched");

    /* a valid CRC over out-of-range fields: decoded, then sanitized */
    memcpy(blob2, blob, SW_BLOB_SIZE);
    blob2[12] = 9;               /* cycle */
    blob2[818 + 1] = 99;         /* slot 0 fade */
    blob2[18 + 8] = 42;          /* ctl 0 type */
    uint16_t c2 = sw_crc16(blob2, SW_BLOB_CRC_OFS);
    blob2[1868] = (uint8_t)c2;
    blob2[1869] = (uint8_t)(c2 >> 8);
    CHECK(sw_store_decode(&B2, blob2, SW_BLOB_SIZE) == 0, "decode out-of-range");
    CHECK(B2.cycle == 4 && B2.snap[0].fade == 6 && B2.ctl[0].type == SW_T_OFF, "sanitized on decode");
}

/* ---- engine: clock ---- */

#define BEAT_US_120 500000u /* one beat at 120 BPM */

static void test_internal_clock(void)
{
    fresh();
    CHECK(st()->clock_source == SW_CLOCK_INTERNAL && st()->running, "internal runs");
    CHECK(st()->cycle_clocks == 4 * 4 * 480, "cycle clocks");
    /* 120 BPM: 960 clocks/s; jittery ticks don't drift. */
    uint32_t t0 = T;
    while ((int32_t)(t0 + 10000000u - T) > 0) {
        uint32_t d = 1 + rnd() % 1777;
        if ((int32_t)(t0 + 10000000u - T) < (int32_t)d)
            d = t0 + 10000000u - T;
        T += d;
        sw_tick(e, T);
    }
    CHECK(st()->position == 9600, "10 s at 120 = 9600 clocks (%u)", st()->position);
    CHECK(st()->cycle_pos == 9600 % 7680, "cycle pos %u", st()->cycle_pos);
    CHECK(n_f8 == 0, "no clock out by default");

    /* odd tempo, 1 us steps for a while, exact */
    B.bpm_x10 = 1337;
    uint32_t p0 = st()->position;
    for (int i = 0; i < 300000; i++) {
        T += 1;
        sw_tick(e, T);
    }
    run_to(T + 59700000u, 997);
    /* 60 s at 133.7 BPM = 133.7 beats = 64176 clocks */
    CHECK(st()->position - p0 == 64176, "133.7 BPM over 60 s: %u", st()->position - p0);
    CHECK(st()->bpm_x10 == 1337, "bpm");

    /* wrap-safe time */
    fresh();
    T = 0xFFFFFFFFu - 200000u;
    sw_init(e, &B, T, clock_cb, 0);
    run_to(T + 1000000u, 1000);
    CHECK(st()->position == 960, "across the time wrap %u", st()->position);
}

static void test_clock_out(void)
{
    fresh();
    B.clock_out = SW_PORT_TRS2 | SW_PORT_USB;
    run_to(T + 2000000u, 333); /* one bar at 120 */
    CHECK(st()->position == 1920, "bar");
    CHECK(n_f8 == 96, "96 F8 per bar (%d)", n_f8);
    CHECK(f8_ports == (SW_PORT_TRS2 | SW_PORT_USB), "ports");
    /* external: none */
    n_f8 = 0;
    for (int i = 0; i < 96; i++) {
        midi1(0xF8);
        run_to(T + 20833, 1000);
    }
    CHECK(st()->clock_source == SW_CLOCK_EXTERNAL, "external");
    CHECK(n_f8 == 0, "no clock out when external (%d)", n_f8);
    B.clock_out = 0;
    run_to(T + 1000000u, 1000);
    CHECK(st()->clock_source == SW_CLOCK_INTERNAL, "timed out");
    CHECK(n_f8 == 0, "clock_out 0");
}

/* `n` ticks at period `per` us, sw_tick every 1 ms in between. */
static void ext_ticks(int n, uint32_t per)
{
    for (int i = 0; i < n; i++) {
        midi1(0xF8);
        run_to(T + per, 1000);
    }
}

static void test_external_clock(void)
{
    fresh();
    run_to(T + 100000u, 1000);
    uint32_t p_int = st()->position;
    /* F8 without Start: external, but the transport doesn't run */
    ext_ticks(10, 20833);
    CHECK(st()->clock_source == SW_CLOCK_EXTERNAL && !st()->running, "external, stopped");
    CHECK(st()->position == p_int, "stopped: position held %u", st()->position);
    CHECK(st()->bpm_x10 >= 1199 && st()->bpm_x10 <= 1201, "measured bpm %u", st()->bpm_x10);

    /* Start: 0, the first tick is the downbeat */
    midi1(0xFA);
    CHECK(st()->position == 0 && st()->running, "start");
    midi1(0xF8);
    CHECK(st()->position == 0, "first tick at 0");
    run_to(T + 10000u, 1000);
    CHECK(st()->position > 0 && st()->position < 20, "interpolated %u", st()->position);
    run_to(T + 30000u, 1000);
    CHECK(st()->position == 19, "capped before the next tick %u", st()->position);
    midi1(0xF8);
    CHECK(st()->position == 20, "tick 1");
    run_to(T + 20833u - 40000u + 20833u, 1000); /* catch up the timeline */
    ext_ticks(47, 20833);
    CHECK(st()->position >= 960 && st()->position < 980, "48 ticks = 2 beats %u", st()->position);

    /* Stop: back to 0, held */
    midi1(0xFC);
    CHECK(st()->position == 0 && !st()->running, "stop");
    ext_ticks(5, 20833);
    CHECK(st()->position == 0, "held while stopped");

    /* SPP 16 (= 1 bar) + Continue */
    uint8_t spp[3] = {0xF2, 16, 0};
    sw_midi_in(e, T, spp, 3);
    CHECK(st()->position == 1920, "spp");
    midi1(0xFB);
    CHECK(st()->running, "continue");
    midi1(0xF8);
    CHECK(st()->position == 1920, "first tick after continue at the SPP");
    run_to(T + 20833, 1000);
    midi1(0xF8);
    CHECK(st()->position == 1940, "then advances");
    uint8_t spp2[3] = {0xF2, 0x01, 0x01}; /* 129 */
    sw_midi_in(e, T, spp2, 3);
    CHECK(st()->position == 129u * 120u, "spp 14-bit");

    /* timeout -> internal, phase-continuous */
    run_to(T + 20833, 1000);
    midi1(0xF8);
    uint32_t p = st()->position;
    CHECK(p == 129u * 120u, "landed");
    run_to(T + 20000, 1000);
    p = st()->position; /* interpolated, capped at +19 */
    run_to(T + SW_EXT_TIMEOUT_US - 21000u, 1000);
    CHECK(st()->clock_source == SW_CLOCK_EXTERNAL, "not yet timed out");
    CHECK(st()->position == p, "held at the cap");
    run_to(T + 1000, 1000);
    CHECK(st()->clock_source == SW_CLOCK_INTERNAL, "timed out -> internal");
    CHECK(st()->position - p <= 1, "no jump on fallback (%u -> %u)", p, st()->position);
    run_to(T + 500000u, 1000);
    CHECK(st()->position - p >= 479 && st()->position - p <= 481, "then free-runs at 120 (%u)",
          st()->position - p);
}

/* ---- engine: arm / drop / jump ---- */

/* The app's time comes from millis(): ticks at 128 BPM (19531.25 us)
 * timestamped to the millisecond must still read 128.0 +- 0.2. */
static void test_ms_tempo(void)
{
    fresh();
    uint64_t exact = (uint64_t)T * 1000u;
    for (int k = 0; k < 200; k++) {
        exact += 19531250u; /* in ns */
        uint32_t t_ms_us = (uint32_t)(exact / 1000000u) * 1000u;
        run_to(t_ms_us, 1000);
        midi1(0xF8);
        if (k > 60)
            CHECK(st()->bpm_x10 >= 1278 && st()->bpm_x10 <= 1282, "ms-quantized 128 BPM reads %u", st()->bpm_x10);
    }
}

static void test_drop_internal(void)
{
    fresh();
    B.cycle = 0; /* 1 bar = 1920 clocks = 2 s at 120 */
    make_slot(&B, 2, 10000, 0x0005, 0, SW_CURVE_LINEAR); /* cut */
    sw_bank_changed(e, T);
    run_to(T + 500000u, 1000); /* position 480 */
    sw_slot_press(e, 2, T);
    CHECK(st()->armed_slot == 2, "armed");
    CHECK(sw_targets(e) == B.snap[2].value, "targets = armed slot");
    /* 1920 clocks are at t0 + 2 s exactly (1 clock = 1041.67 us) */
    uint32_t t_bar = 1000 + 2000000u;
    run_to(t_bar - 1042, 100);
    CHECK(st()->position == 1918 && st()->armed_slot == 2 && e->live[0] == 0, "just before (%u)",
          st()->position);
    run_to(t_bar - 1, 1);
    CHECK(st()->position == 1919 && st()->armed_slot == 2, "1919, still armed");
    run_to(t_bar, 1);
    CHECK(st()->position == 1920, "boundary");
    CHECK(st()->armed_slot == -1 && e->live[0] == 10000 && e->live[31] == 10000, "dropped at 1920");
    CHECK(e->toggles == 0x0005, "toggles");
    CHECK(st()->current_slot == 2 && !st()->modified && !st()->fading, "current");
    CHECK(sw_targets(e) == NULL, "no targets");
}

static void test_drop_external(void)
{
    fresh();
    B.cycle = 0;
    make_slot(&B, 1, 8000, 0, 3, SW_CURVE_LINEAR); /* 1 bar fade */
    sw_bank_changed(e, T);
    sw_slot_press(e, 1, T);
    midi1(0xFA);
    midi1(0xF8); /* downbeat 0 is a boundary the position lands on */
    CHECK(st()->armed_slot == -1 && st()->fading && st()->fade_slot == 1, "armed before Start drops on the downbeat");
    /* finish that fade over the bar, arm again, drop at tick 192 (bar 2) */
    for (int i = 1; i <= 96; i++) {
        run_to(T + 20833, 1000);
        midi1(0xF8);
    }
    CHECK(st()->position == 1920 && !st()->fading && e->live[5] == 8000, "fade done at 1920");
    make_slot(&B, 4, 100, 0, 0, SW_CURVE_LINEAR);
    sw_slot_press(e, 4, T);
    for (int i = 97; i < 192; i++) {
        run_to(T + 20833, 1000);
        midi1(0xF8);
        if (i == 191)
            CHECK(st()->armed_slot == 4, "armed until the tick");
    }
    run_to(T + 20833, 1000);
    CHECK(st()->position == 3839 && st()->armed_slot == 4, "interpolation stops short (%u)", st()->position);
    midi1(0xF8);
    CHECK(st()->position == 3840 && st()->armed_slot == -1 && e->live[0] == 100, "dropped on tick 192");
}

static void test_stopped_external(void)
{
    fresh();
    B.cycle = 0;
    make_slot(&B, 0, 0, 0, 0, SW_CURVE_LINEAR);
    make_slot(&B, 3, 16000, 0x8000, 4, SW_CURVE_LINEAR); /* 2 bars */
    sw_bank_changed(e, T);
    ext_ticks(5, 20833); /* external, stopped */
    sw_slot_press(e, 3, T);
    ext_ticks(200, 20833);
    CHECK(st()->armed_slot == 3 && e->live[0] == 0, "stopped: stays armed");
    /* a jump starts a fade that holds at 0 */
    sw_slot_press(e, 3, T);
    CHECK(st()->armed_slot == -1 && st()->fading && st()->fade_progress == 0, "jump while stopped");
    CHECK(e->toggles == 0x8000 && st()->current_slot == 3, "toggles at fade start");
    ext_ticks(100, 20833);
    CHECK(st()->fading && st()->fade_progress == 0 && e->live[0] == 0, "fade holds while stopped");
    midi1(0xFB);
    ext_ticks(96, 20833); /* the downbeat + 95 ticks of motion */
    midi1(0xF8);          /* 96 ticks = 1 bar = half the fade */
    CHECK(st()->position == 1920 && st()->fading && e->live[0] == 8000, "half way once moving (%u)",
          e->live[0]);
    /* Stop: the fade freezes where it is */
    midi1(0xFC);
    ext_ticks(50, 20833);
    CHECK(st()->fading && e->live[0] == 8000 && st()->position == 0, "frozen by stop");
    midi1(0xFA);
    ext_ticks(97, 20833);
    CHECK(!st()->fading && e->live[0] == 16000, "finished after restart");
}

static void test_jump_quant(void)
{
    /* NONE: at once */
    fresh();
    make_slot(&B, 5, 4000, 0, 0, SW_CURVE_LINEAR);
    run_to(T + 123456, 1000);
    sw_jump(e, 5, T);
    CHECK(e->live[0] == 4000 && st()->pending_jump == -1, "jump none");

    /* BEAT */
    fresh();
    B.jump_quant = SW_JQ_BEAT;
    make_slot(&B, 5, 4000, 0, 0, SW_CURVE_LINEAR);
    run_to(T + 600000, 1000); /* 576 clocks */
    sw_slot_press(e, 5, T);
    sw_jump(e, 5, T);
    CHECK(st()->pending_jump == 5 && st()->armed_slot == -1 && e->live[0] == 0, "pending, arm cleared");
    CHECK(sw_targets(e) == B.snap[5].value, "targets = pending jump");
    run_to(1000 + 1000000u - 1, 1);
    CHECK(st()->position == 959 && e->live[0] == 0, "not before the beat");
    run_to(T + 1, 1);
    CHECK(st()->position == 960 && e->live[0] == 4000 && st()->pending_jump == -1, "on beat 2");

    /* BAR */
    fresh();
    B.jump_quant = SW_JQ_BAR;
    make_slot(&B, 5, 4000, 0, 0, SW_CURVE_LINEAR);
    run_to(T + 600000, 1000);
    sw_jump(e, 5, T);
    run_to(1000 + 2000000u - 1, 1);
    CHECK(e->live[0] == 0 && st()->pending_jump == 5, "not before the bar");
    run_to(T + 1, 1);
    CHECK(st()->position == 1920 && e->live[0] == 4000, "on the bar");

    /* armed slot pressed again = jump (quantized too) */
    fresh();
    B.jump_quant = SW_JQ_BEAT;
    make_slot(&B, 6, 1234, 0, 0, SW_CURVE_LINEAR);
    sw_slot_press(e, 6, T);
    sw_slot_press(e, 6, T);
    CHECK(st()->armed_slot == -1 && st()->pending_jump == 6, "press armed = jump");
    run_to(T + 600000, 1000);
    CHECK(e->live[0] == 1234, "fired on the beat");

    /* empty slot press does nothing; arming replaces */
    fresh();
    make_slot(&B, 1, 1, 0, 0, 0);
    make_slot(&B, 2, 2, 0, 0, 0);
    sw_slot_press(e, 7, T);
    CHECK(st()->armed_slot == -1 && st()->pending_jump == -1, "empty slot");
    sw_jump(e, 7, T);
    CHECK(st()->pending_jump == -1 && e->live[0] == 0, "empty jump");
    sw_slot_press(e, 1, T);
    sw_slot_press(e, 2, T);
    CHECK(st()->armed_slot == 2, "arm replaced");
    sw_cancel_arm(e);
    CHECK(st()->armed_slot == -1, "cancel arm");
}

/* Fade 1 beat (480 clocks) from 0 to 16000 with curve cv; return the value
 * at clock offset `at`. */
static uint16_t fade_at(int cv, uint32_t at)
{
    fresh();
    make_slot(&B, 1, 16000, 0x0003, 1, cv);
    sw_jump(e, 1, T);
    /* 1 clock = 1041.666 us; tick to just past `at` clocks */
    uint32_t us = (uint32_t)(((uint64_t)at * 500000u + 479) / 480u);
    run_to(T + us, 1000);
    CHECK(st()->position == at, "fade_at position %u != %u", st()->position, at);
    return e->live[7];
}

static void test_curves(void)
{
    /* endpoints */
    for (int cv = 0; cv < SW_CURVE_COUNT; cv++) {
        CHECK(fade_at(cv, 0) == 0, "curve %d start", cv);
        CHECK(e->toggles == 0x0003, "toggles applied at fade start");
        CHECK(st()->fading && st()->current_slot == 1 && !st()->modified, "fading");
        CHECK(sw_targets(e) == e->fade_to, "targets = fade");
        uint16_t end = fade_at(cv, 480);
        CHECK(end == 16000 && !st()->fading, "curve %d end %u", cv, end);
        uint16_t pre = fade_at(cv, 479);
        CHECK(st()->fading, "curve %d still fading at 479", cv);
        CHECK(cv == SW_CURVE_HOLD ? pre == 0 : pre > 15900, "curve %d at 479: %u", cv, pre);
    }
    uint16_t v;
    v = fade_at(SW_CURVE_LINEAR, 240);
    CHECK(v == 8000, "linear mid %u", v);
    CHECK(st()->fade_progress == 32768, "progress mid %u", st()->fade_progress);
    v = fade_at(SW_CURVE_LINEAR, 120);
    CHECK(v == 4000, "linear quarter %u", v);
    v = fade_at(SW_CURVE_EASE, 240);
    CHECK(v == 8000, "ease mid %u", v);
    v = fade_at(SW_CURVE_EASE, 120);
    CHECK(v == 2000, "ease quarter %u", v);
    v = fade_at(SW_CURVE_EASE, 360);
    CHECK(v == 14000, "ease 3/4 %u", v);
    v = fade_at(SW_CURVE_S, 240);
    CHECK(v == 8000, "s mid %u", v);
    v = fade_at(SW_CURVE_S, 120);
    CHECK(v == 1000, "s quarter %u", v);
    v = fade_at(SW_CURVE_S, 360);
    CHECK(v == 15000, "s 3/4 %u", v);
    v = fade_at(SW_CURVE_HOLD, 240);
    CHECK(v == 0, "hold mid %u", v);

    /* fades follow position: a bar fade at 120 takes 2 s; at a tempo
     * change mid-way it continues from the same progress */
    fresh();
    make_slot(&B, 1, 16000, 0, 3, SW_CURVE_LINEAR);
    sw_jump(e, 1, T);
    run_to(T + 1000000u, 1000);
    CHECK(e->live[0] == 8000, "half a bar fade %u", e->live[0]);
    B.bpm_x10 = 600;
    run_to(T + 1000000u, 1000);
    CHECK(e->live[0] == 12000, "slower after the tempo change %u", e->live[0]);
    run_to(T + 1000000u, 1000);
    CHECK(e->live[0] == 16000 && !st()->fading, "done");

    /* a new fade replaces a running one, from the live values */
    fresh();
    make_slot(&B, 1, 16000, 0, 3, SW_CURVE_LINEAR);
    make_slot(&B, 2, 0, 0, 1, SW_CURVE_LINEAR);
    sw_jump(e, 1, T);
    run_to(T + 1000000u, 1000);
    sw_jump(e, 2, T);
    CHECK(st()->fade_slot == 2 && e->fade_from[0] == 8000, "replace from live");
    run_to(T + 250000u, 1000);
    CHECK(e->live[0] == 4000, "heading down %u", e->live[0]);
}

static void test_override_and_store(void)
{
    fresh();
    make_slot(&B, 1, 16000, 0, 3, SW_CURVE_LINEAR);
    sw_jump(e, 1, T);
    run_to(T + 500000u, 1000);
    CHECK(e->live[3] == 4000, "quarter");
    e->ctl_dirty = 0;
    sw_turn(e, 3, 2, 0, T);
    CHECK(e->live[3] == 4256 && (e->ctl_dirty & (1u << 3)), "turned + dirty");
    CHECK(st()->modified && st()->fading, "modified, fade goes on");
    sw_set_value(e, 4, 100, T);
    CHECK(e->live[4] == 100, "set value");
    run_to(T + 500000u, 1000);
    CHECK(e->live[3] == 4256 && e->live[4] == 100, "out of the fade");
    CHECK(e->live[2] == 8000 && e->live[5] == 8000, "the others go on");
    run_to(T + 1000000u, 1000);
    CHECK(!st()->fading && e->live[2] == 16000 && e->live[3] == 4256, "end");
    sw_turn(e, 3, -1000, 1, T);
    CHECK(e->live[3] == 0, "clamp low");
    sw_turn(e, 3, 1000, 0, T);
    CHECK(e->live[3] == SW_VMAX, "clamp high");
    sw_turn(e, 3, -1, 1, T);
    CHECK(e->live[3] == SW_VMAX - 8, "fine step");

    /* store */
    sw_toggle(e, 9, 1);
    CHECK(e->toggles == (1u << 9) && st()->modified, "toggle");
    CHECK(sw_store(e, 4) == 1, "store returns 1");
    CHECK(B.snap[4].stored && B.snap[4].value[3] == SW_VMAX - 8 && B.snap[4].toggles == (1u << 9),
          "stored live");
    CHECK(B.snap[4].fade == 3 && B.snap[4].curve == 0, "fade/curve unchanged");
    CHECK(st()->current_slot == 4 && !st()->modified, "current, clean");
    sw_toggle(e, 9, -1);
    CHECK(e->toggles == 0 && st()->modified, "flip");
    CHECK(sw_store(e, 15) == 0 && sw_store(e, -1) == 0, "bad slot");
    /* a turn that changes nothing outside a fade doesn't set modified */
    sw_store(e, 4);
    sw_turn(e, 3, 0, 0, T);
    CHECK(!st()->modified, "no change, not modified");
    /* store during a fade stops it */
    sw_jump(e, 1, T);
    run_to(T + 100000u, 1000);
    CHECK(st()->fading, "fading");
    sw_store(e, 6);
    uint16_t v = e->live[0];
    run_to(T + 3000000u, 1000);
    CHECK(!st()->fading && e->live[0] == v && st()->current_slot == 6, "store froze the fade");
}

static void test_init_and_banks(void)
{
    /* init recalls slot 0 as a cut even when its fade is long */
    sw_bank_default(&B, 0);
    make_slot(&B, 0, 5000, 0x00F0, 6, SW_CURVE_S);
    T = 77;
    sw_init(e, &B, T, 0, 0);
    CHECK(e->live[0] == 5000 && e->toggles == 0x00F0 && !st()->fading && st()->current_slot == 0,
          "init recalls slot 0");
    CHECK(e->ctl_dirty == 0xFFFFFFFFu && e->tog_dirty == 0xFFFF, "all dirty at init");
    sw_bank_default(&B, 0);
    sw_init(e, &B, T, 0, 0);
    CHECK(e->live[0] == 0 && st()->current_slot == -1, "empty slot 0");
    CHECK(e->ctl_dirty == 0 && e->tog_dirty == 0, "empty slot 0: nothing sent at init");
    run_to(T + 100000, 1000); /* NULL clock_fn is fine */

    /* set_bank: cancels, jumps to slot 0 with its own fade, ignoring quantize */
    fresh();
    make_slot(&B, 1, 3000, 0, 3, 0);
    make_slot(&B, 2, 3000, 0, 3, 0);
    sw_jump(e, 1, T);
    sw_slot_press(e, 2, T);
    sw_bank_default(&B2, 1);
    B2.jump_quant = SW_JQ_BAR;
    B2.cycle = 0;
    make_slot(&B2, 0, 9000, 0x0002, 1, SW_CURVE_LINEAR);
    sw_set_bank(e, &B2, T);
    CHECK(st()->armed_slot == -1 && st()->pending_jump == -1, "set_bank cancels");
    CHECK(st()->fading && st()->fade_slot == 0 && st()->current_slot == 0 && e->toggles == 0x0002,
          "jumps to slot 0");
    CHECK(st()->cycle_clocks == 1920, "new cycle");
    run_to(T + 500000u, 1000);
    CHECK(e->live[0] == 9000 && !st()->fading, "slot 0 fade 1 beat");
    /* a bank with nothing in slot 0: values stay */
    sw_bank_default(&B, 2);
    e->ctl_dirty = 0;
    sw_set_bank(e, &B, T);
    CHECK(e->live[0] == 9000 && e->ctl_dirty == 0 && st()->current_slot == -1, "empty bank keeps live");

    /* bank_changed: keeps what's still stored, cancels what isn't */
    fresh();
    make_slot(&B, 1, 3000, 0, 3, 0);
    make_slot(&B, 2, 3000, 0, 3, 0);
    sw_jump(e, 1, T);
    sw_slot_press(e, 2, T);
    run_to(T + 100000, 1000);
    e->ctl_dirty = 0;
    e->tog_dirty = 0;
    uint16_t v = e->live[0];
    sw_bank_changed(e, T);
    CHECK(st()->armed_slot == 2 && st()->fading, "still stored: kept");
    CHECK(e->ctl_dirty == 0xFFFFFFFFu && e->tog_dirty == 0xFFFF && e->live[0] == v, "all dirty, live kept");
    B.snap[1].stored = 0;
    B.snap[2].stored = 0;
    sw_bank_changed(e, T);
    CHECK(st()->armed_slot == -1 && !st()->fading && st()->current_slot == -1, "gone: cancelled");
    CHECK(sw_targets(e) == NULL, "no targets");
}

static void test_dirty(void)
{
    fresh();
    e->ctl_dirty = 0;
    e->tog_dirty = 0;
    make_slot(&B, 1, 0, 0, 0, 0);
    B.snap[1].value[7] = 50;
    sw_jump(e, 1, T);
    CHECK(e->ctl_dirty == (1u << 7) && e->tog_dirty == 0, "only changed controls dirty (%08x)", e->ctl_dirty);
    sw_toggle(e, 3, 1);
    CHECK(e->tog_dirty == (1u << 3), "toggle dirty");
    e->tog_dirty = 0;
    sw_toggle(e, 3, 1);
    CHECK(e->tog_dirty == 0, "same toggle: not dirty");
}

/* ---- out ---- */

typedef struct {
    int port, len;
    uint8_t b[12];
} wrec_t;
static wrec_t W[20000];
static int nw;
static int refuse[3];

static int wr(void *ctx, int port, const uint8_t *bytes, int len)
{
    (void)ctx;
    if (refuse[port])
        return 0;
    if (nw < 20000) {
        W[nw].port = port;
        W[nw].len = len;
        memcpy(W[nw].b, bytes, (size_t)len);
        nw++;
    }
    return 1;
}

static sw_out_t O;

static void test_out_map(void)
{
    sw_control_t c = {{0}, SW_T_CC7, 1, 0, 0, 0, 127};
    CHECK(sw_out_value(&c, 0) == 0 && sw_out_value(&c, SW_VMAX) == 127, "cc7 ends");
    CHECK(sw_out_value(&c, 8192) == 64 && sw_out_value(&c, 8191) == 63, "cc7 round");
    CHECK(sw_out_value(&c, 64) == 0 && sw_out_value(&c, 65) == 1, "round to nearest (64.5 units = 1/2)");
    c.min = 100;
    c.max = 20;
    CHECK(sw_out_value(&c, 0) == 100 && sw_out_value(&c, SW_VMAX) == 20, "inverted ends");
    CHECK(sw_out_value(&c, 8192) == 60, "inverted mid %u", sw_out_value(&c, 8192));
    CHECK(sw_out_value(&c, 102) == 100 && sw_out_value(&c, 103) == 99, "inverted rounding");
    c.type = SW_T_CC14;
    c.min = 0;
    c.max = 16383;
    for (uint32_t p = 0; p <= SW_VMAX; p += 97)
        CHECK(sw_out_value(&c, (uint16_t)p) == p, "14-bit identity");
    c.min = 16383;
    c.max = 0;
    CHECK(sw_out_value(&c, 1000) == 15383, "14-bit inverted");
    c.min = 1000;
    c.max = 3000;
    CHECK(sw_out_value(&c, 8192) == 2000, "14-bit range %u", sw_out_value(&c, 8192));
    c.min = c.max = 77;
    CHECK(sw_out_value(&c, 5000) == 77, "flat");
}

static void out_fresh(void)
{
    fresh();
    nw = 0;
    refuse[0] = refuse[1] = refuse[2] = 0;
    for (int c = 0; c < SW_CONTROLS; c++)
        B.ctl[c].ports = 0;
    for (int t = 0; t < SW_TOGGLES; t++)
        B.tog[t].ports = 0;
    sw_out_init(&O, T);
    sw_out_service(&O, e, T, wr, 0); /* clears init's dirty */
    nw = 0;
}

static int wmatch(int i, int port, const uint8_t *b, int len)
{
    return i < nw && W[i].port == port && W[i].len == len && !memcmp(W[i].b, b, (size_t)len);
}

static void test_out_formats(void)
{
    out_fresh();
    sw_control_t *k = B.ctl;
    k[0] = (sw_control_t){{0}, SW_T_CC7, SW_PORT_USB, 2, 74, 0, 127};
    k[1] = (sw_control_t){{0}, SW_T_CC14, SW_PORT_TRS1, 3, 7, 0, 16383};
    k[2] = (sw_control_t){{0}, SW_T_NRPN7, SW_PORT_TRS2, 15, 0x1234 & 0x3FFF, 0, 127};
    k[3] = (sw_control_t){{0}, SW_T_NRPN14, SW_PORT_USB, 0, 300, 0, 16383};
    k[4] = (sw_control_t){{0}, SW_T_OFF, SW_PORT_USB, 0, 1, 0, 127};
    B.tog[5] = (sw_toggle_t){{0}, SW_PORT_USB, 9, 64, 10, 90};
    sw_set_value(e, 0, SW_VMAX, T);
    sw_set_value(e, 1, 0x1ABC, T);
    sw_set_value(e, 2, SW_VMAX, T);
    sw_set_value(e, 3, 0x2F01, T);
    sw_set_value(e, 4, 999, T);
    sw_toggle(e, 5, 1);
    sw_out_service(&O, e, T + 1, wr, 0);
    CHECK(nw == 5, "5 messages (%d)", nw);
    uint8_t m0[] = {0xB2, 74, 127};
    uint8_t m1[] = {0xB3, 7, 0x1ABC >> 7, 0xB3, 39, 0x1ABC & 127};
    uint8_t m2[] = {0xBF, 99, 0x1234 >> 7, 0xBF, 98, 0x1234 & 127, 0xBF, 6, 127};
    uint8_t m3[] = {0xB0, 99, 2, 0xB0, 98, 44, 0xB0, 6, 0x2F01 >> 7, 0xB0, 38, 0x2F01 & 127};
    uint8_t mt[] = {0xB9, 64, 90};
    CHECK(wmatch(0, 2, m0, 3), "CC7");
    CHECK(wmatch(1, 0, m1, 6), "CC14");
    CHECK(wmatch(2, 1, m2, 9), "NRPN7");
    CHECK(wmatch(3, 2, m3, 12), "NRPN14");
    CHECK(wmatch(4, 2, mt, 3), "toggle on");
    CHECK(e->ctl_dirty == 0 && e->tog_dirty == 0, "dirty consumed");
    nw = 0;
    sw_toggle(e, 5, 0);
    sw_out_service(&O, e, T + 2, wr, 0);
    uint8_t mo[] = {0xB9, 64, 10};
    CHECK(nw == 1 && wmatch(0, 2, mo, 3), "toggle off");

    /* dedupe on output units: 16383 -> 16370 is still 127 on a CC7 */
    nw = 0;
    sw_set_value(e, 0, 16370, T);
    sw_out_service(&O, e, T + 3, wr, 0);
    CHECK(nw == 0, "same output value not resent");
    sw_set_value(e, 0, 0, T);
    sw_out_service(&O, e, T + 4, wr, 0);
    CHECK(nw == 1 && W[0].b[2] == 0, "changed value sent");
    /* forget: dirty resent even if unchanged */
    nw = 0;
    sw_out_forget(&O);
    sw_bank_changed(e, T);
    sw_out_service(&O, e, T + 5, wr, 0);
    CHECK(nw == 5, "resent after forget (%d)", nw);
    nw = 0;
    sw_bank_changed(e, T);
    sw_out_service(&O, e, T + 6, wr, 0);
    CHECK(nw == 0, "bank_changed without forget: deduped");

    /* multi-port: both ports get it, ports 0 sends nothing */
    out_fresh();
    B.ctl[9].ports = SW_PORT_ALL;
    sw_set_value(e, 9, SW_VMAX, T);
    sw_set_value(e, 10, SW_VMAX, T);
    sw_out_service(&O, e, T + 1, wr, 0);
    CHECK(nw == 3 && W[0].port == 0 && W[1].port == 1 && W[2].port == 2, "all ports");
    CHECK(O.pend_ctl[10] == 0, "ports 0: nothing owed");
}

static void test_out_rate(void)
{
    /* a port refusing: nothing stale, only the latest goes out */
    out_fresh();
    B.ctl[0].ports = SW_PORT_TRS1 | SW_PORT_USB;
    refuse[0] = 1;
    for (int v = 1; v <= 100; v++) {
        sw_set_value(e, 0, (uint16_t)(v * 160), T); /* 1.24 units apart */
        T += 1000;
        sw_out_service(&O, e, T, wr, 0);
    }
    int usb = 0, trs = 0;
    for (int i = 0; i < nw; i++)
        W[i].port == 0 ? trs++ : usb++;
    CHECK(trs == 0 && usb == 100, "USB got every value, TRS none (%d, %d)", usb, trs);
    CHECK(O.pend_ctl[0] == SW_PORT_TRS1, "TRS still owed");
    nw = 0;
    refuse[0] = 0;
    T += 1000;
    sw_out_service(&O, e, T, wr, 0);
    CHECK(nw == 1 && W[0].port == 0 && W[0].b[2] == sw_out_value(&B.ctl[0], 100 * 160),
          "TRS gets only the latest (%d msgs)", nw);
    CHECK(O.pend_ctl[0] == 0, "clean");

    /* the budget: 2000 B/s on TRS, burst 96 */
    out_fresh();
    for (int c = 0; c < SW_CONTROLS; c++)
        B.ctl[c].ports = SW_PORT_TRS1;
    nw = 0;
    sw_out_init(&O, T);
    for (int c = 0; c < SW_CONTROLS; c++)
        sw_set_value(e, c, 5000, T);
    sw_out_service(&O, e, T, wr, 0);
    CHECK(nw == 32, "burst of 96 bytes covers 32 CC7s (%d)", nw);
    /* a 32-control CC14 fade for 1 s at a 1 ms loop: every control
     * changes every pass (1000/s), far more than ~2000 bytes = 333 msgs */
    for (int c = 0; c < SW_CONTROLS; c++) {
        B.ctl[c].type = SW_T_CC14;
        B.ctl[c].number = (uint16_t)(20 + c % 12); /* per[] below uses b[1] - 20 */
        B.ctl[c].max = 16383;
    }
    for (int c = 0; c < SW_CONTROLS; c++)
        B.ctl[c].channel = (uint8_t)(c / 12);
    make_slot(&B, 1, 16000, 0, 6, SW_CURVE_LINEAR); /* 8 bars */
    sw_jump(e, 1, T);
    nw = 0;
    uint32_t t0 = T;
    for (int i = 0; i < 1000; i++) {
        T += 1000;
        sw_tick(e, T);
        sw_out_service(&O, e, T, wr, 0);
    }
    int bytes = 0, per[SW_CONTROLS] = {0};
    for (int i = 0; i < nw; i++) {
        bytes += W[i].len;
        per[(W[i].b[0] & 15) * 12 + W[i].b[1] - 20]++;
    }
    (void)t0;
    CHECK(bytes >= 1990 && bytes <= 2096, "rate %d bytes/s", bytes);
    int lo = 1 << 30, hi = 0;
    for (int c = 0; c < SW_CONTROLS; c++) {
        if (per[c] < lo)
            lo = per[c];
        if (per[c] > hi)
            hi = per[c];
    }
    CHECK(lo > 0 && hi - lo <= 1, "round-robin fair: %d..%d per control", lo, hi);
    CHECK(O.bytes_out[0] >= (uint32_t)bytes, "stats");

    /* the latest value each time: every message carries the current value */
    nw = 0;
    T += 1000;
    sw_tick(e, T);
    sw_out_service(&O, e, T, wr, 0);
    for (int i = 0; i < nw; i++) {
        int c = (W[i].b[0] & 15) * 12 + W[i].b[1] - 20;
        uint16_t v = sw_out_value(&B.ctl[c], e->live[c]);
        CHECK(W[i].b[2] == (v >> 7) && W[i].b[5] == (v & 127), "latest value");
    }

    /* the fade's end gets everywhere eventually */
    for (int i = 0; i < 40000; i++) {
        T += 1000;
        sw_tick(e, T);
        sw_out_service(&O, e, T, wr, 0);
    }
    CHECK(!st()->fading, "fade over");
    int clean = 1;
    for (int c = 0; c < SW_CONTROLS; c++)
        clean &= O.pend_ctl[c] == 0 && O.sent[c] == sw_out_value(&B.ctl[c], 16000);
    CHECK(clean, "all clean at the target");

    /* USB is faster: 20000 B/s */
    out_fresh();
    for (int c = 0; c < SW_CONTROLS; c++) {
        B.ctl[c].ports = SW_PORT_USB;
        B.ctl[c].type = SW_T_CC14;
        B.ctl[c].number = (uint16_t)(c % 32);
        B.ctl[c].channel = (uint8_t)(c / 32);
        B.ctl[c].max = 16383;
    }
    make_slot(&B, 1, 16000, 0, 6, SW_CURVE_LINEAR);
    sw_jump(e, 1, T);
    sw_out_service(&O, e, T, wr, 0);
    nw = 0;
    int ub = 0;
    for (int i = 0; i < 100; i++) {
        T += 1000;
        sw_tick(e, T);
        sw_out_service(&O, e, T, wr, 0);
    }
    for (int i = 0; i < nw; i++)
        ub += W[i].len;
    CHECK(ub >= 1990 && ub <= 2096, "USB 100 ms at 20000 B/s: %d bytes", ub);
}

int main(void)
{
    printf("sw_bank_t: %zu bytes, sw_engine_t: %zu, sw_out_t: %zu\n", sizeof(sw_bank_t),
           sizeof(sw_engine_t), sizeof(sw_out_t));
    test_model();
    test_store();
    test_internal_clock();
    test_clock_out();
    test_external_clock();
    test_ms_tempo();
    test_drop_internal();
    test_drop_external();
    test_stopped_external();
    test_jump_quant();
    test_curves();
    test_override_and_store();
    test_init_and_banks();
    test_dirty();
    test_out_map();
    test_out_formats();
    test_out_rate();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
