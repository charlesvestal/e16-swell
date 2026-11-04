/*
 * Host tests for the Swell SysEx editor protocol (src/core/proto.c),
 * run against the real engine and store. Plain C, run by test/run_all.sh;
 * prints "ALL PASSED" and exits 0 on success.
 *
 * Covers: 7-in-8 packing (round trips at the edge lengths, exact bytes,
 * bad input), every command's reply bytes, every NAK, WRITE/COMMIT
 * staging (partial, other bank, corrupt blob leaves the bank alone), the
 * EVENT layout, rate (66 ms, only on change, once a second anyway) and
 * the unsent retry, and that foreign SysEx is ignored.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

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

/* ---- the host ---- */

static sw_bank_t banks[SW_BANKS];
static sw_engine_t eng;
static int cur_bank, cur_layer;
static int dirty_calls, dirty_last = -1;
static int dirty_flag[SW_BANKS];
static int select_calls;
static sw_proto_t P;

static void h_dirty(void *u, int b)
{
    (void)u;
    dirty_calls++;
    dirty_last = b;
    dirty_flag[b] = 1;
}
static int h_is_dirty(void *u, int b)
{
    (void)u;
    return dirty_flag[b];
}
static void h_select(void *u, int b, uint32_t now)
{
    (void)u;
    select_calls++;
    cur_bank = b;
    sw_set_bank(&eng, &banks[b], now);
}
static void h_layer(void *u, int l)
{
    (void)u;
    cur_layer = l;
}

static void reset(void)
{
    for (int i = 0; i < SW_BANKS; i++)
        sw_bank_default(&banks[i], i);
    cur_bank = 0;
    cur_layer = 0;
    dirty_calls = 0;
    dirty_last = -1;
    memset(dirty_flag, 0, sizeof dirty_flag);
    select_calls = 0;
    sw_init(&eng, &banks[0], 0, NULL, NULL);
    sw_proto_host_t h = {banks, &eng, &cur_bank, &cur_layer, NULL, h_dirty, h_is_dirty, h_select, h_layer};
    sw_proto_init(&P, &h);
}

/* ---- messages ---- */

static uint8_t rep[512];
static int rep_len;
static uint8_t rraw[512];
static int rraw_len;
static int rcmd, rseq;

/* Parse rep into rcmd / rseq / rraw; 0 if it isn't a well-formed message of ours. */
static int parse(const uint8_t *m, int n)
{
    rcmd = rseq = -1;
    rraw_len = -1;
    if (n < 7 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x53 || m[3] != 0x57 || m[n - 1] != 0xF7)
        return 0;
    rcmd = m[4];
    rseq = m[5];
    for (int i = 1; i < n - 1; i++)
        if (m[i] & 0x80)
            return 0;
    rraw_len = sw_unpack7(m + 6, n - 7, rraw, sizeof rraw);
    return rraw_len >= 0;
}

static uint32_t g_now_us;

/* Send cmd/seq with a raw payload; returns the reply length and parses it. */
static int req(int cmd, int seq, const uint8_t *raw, int n)
{
    uint8_t m[600];
    m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x53, m[3] = 0x57, m[4] = (uint8_t)cmd, m[5] = (uint8_t)seq;
    int k = sw_pack7(raw, n, m + 6, (int)sizeof m - 7);
    if (k < 0) {
        fprintf(stderr, "req: payload too long\n");
        exit(2);
    }
    m[6 + k] = 0xF7;
    memset(rep, 0xEE, sizeof rep);
    rep_len = sw_proto_handle(&P, m, 7 + k, rep, SW_PROTO_MAX, g_now_us);
    if (rep_len > 0)
        parse(rep, rep_len);
    return rep_len;
}

static int is_nak(int cmd, int seq, int err)
{
    return rep_len > 0 && rcmd == SW_CMD_NAK && rseq == seq && rraw_len == 2 && rraw[0] == cmd &&
           rraw[1] == err;
}

static int is_ok(int cmd, int seq)
{
    return rep_len > 0 && rcmd == cmd && rseq == seq && rep_len <= SW_PROTO_MAX;
}

static int action(int act, int a, int b)
{
    uint8_t r[4] = {(uint8_t)act, (uint8_t)a, (uint8_t)b, (uint8_t)(b >> 8)};
    return req(SW_CMD_ACTION, 9, r, 4);
}

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- packing ---- */

static void test_pack(void)
{
    static const int lens[] = {0, 1, 6, 7, 8, 13, 14, 15, 128, 131, 156, 255};
    uint32_t s = 12345;
    for (unsigned li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        int n = lens[li];
        uint8_t raw[300], enc[400], dec[300];
        for (int i = 0; i < n; i++) {
            s = s * 1103515245u + 12345u;
            raw[i] = (uint8_t)(s >> 16);
        }
        if (n > 0)
            raw[0] = 0xFF;
        int need = n + (n + 6) / 7;
        int k = sw_pack7(raw, n, enc, sizeof enc);
        CHECK(k == need, "pack7 len %d -> %d, want %d", n, k, need);
        int hi = 0;
        for (int i = 0; i < k; i++)
            hi |= enc[i] & 0x80;
        CHECK(!hi, "pack7 len %d: top bit set", n);
        int d = sw_unpack7(enc, k, dec, sizeof dec);
        CHECK(d == n && memcmp(raw, dec, (size_t)n) == 0, "unpack7 round trip len %d (%d)", n, d);
        /* exact fit works, one short doesn't */
        CHECK(sw_pack7(raw, n, enc, need) == need, "pack7 exact cap len %d", n);
        if (n > 0) {
            CHECK(sw_pack7(raw, n, enc, need - 1) == -1, "pack7 small cap len %d", n);
            CHECK(sw_unpack7(enc, k, dec, n - 1) == -1, "unpack7 small cap len %d", n);
        }
    }
    /* exact bytes */
    {
        uint8_t raw[8] = {0x80, 0x01, 0xFF, 0x00, 0x7F, 0x81, 0x40, 0xC3};
        uint8_t enc[16];
        static const uint8_t want[10] = {0x25, 0x00, 0x01, 0x7F, 0x00, 0x7F, 0x01, 0x40, 0x01, 0x43};
        int k = sw_pack7(raw, 8, enc, sizeof enc);
        CHECK(k == 10 && memcmp(enc, want, 10) == 0, "pack7 exact bytes");
    }
    /* bad input */
    {
        uint8_t out[32];
        static const uint8_t topbit[3] = {0x00, 0x80, 0x01};
        static const uint8_t lone_hdr[1] = {0x00};
        static const uint8_t bad_hdr_bit[3] = {0x04, 0x01, 0x02}; /* bit 2 for a byte not there */
        static const uint8_t hdr_top[2] = {0x80, 0x01};
        static const uint8_t trailing[9] = {0, 1, 2, 3, 4, 5, 6, 7, 0}; /* 8th byte = header, no data */
        CHECK(sw_unpack7(topbit, 3, out, 32) == -1, "unpack7 data top bit");
        CHECK(sw_unpack7(lone_hdr, 1, out, 32) == -1, "unpack7 lone header");
        CHECK(sw_unpack7(bad_hdr_bit, 3, out, 32) == -1, "unpack7 header bit past group");
        CHECK(sw_unpack7(hdr_top, 2, out, 32) == -1, "unpack7 header top bit");
        CHECK(sw_unpack7(trailing, 9, out, 32) == -1, "unpack7 trailing header");
        CHECK(sw_unpack7(trailing, 8, out, 32) == 7, "unpack7 a full group");
        CHECK(sw_unpack7(out, 0, out, 0) == 0, "unpack7 empty");
        CHECK(sw_pack7(out, -1, out, 32) == -1 && sw_unpack7(out, -1, out, 32) == -1, "negative n");
    }
}

/* ---- framing ---- */

static void test_foreign(void)
{
    reset();
    uint8_t out[SW_PROTO_MAX];
    static const uint8_t a[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};             /* identity request */
    static const uint8_t b[] = {0xF0, 0x7D, 0x45, 0x20, 0x00, 0xF7};             /* the mirror */
    static const uint8_t c[] = {0xF0, 0x7D, 0x53, 0x58, 0x01, 0x00, 0xF7};       /* 'S' 'X' */
    static const uint8_t d[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0xF7};
    static const uint8_t e[] = {0x90, 0x7D, 0x53, 0x57, 0x01, 0x00, 0xF7};       /* not SysEx */
    static const uint8_t f[] = {0xF0, 0x7D, 0x53};
    CHECK(sw_proto_handle(&P, a, sizeof a, out, sizeof out, 0) == 0, "foreign: universal");
    CHECK(sw_proto_handle(&P, b, sizeof b, out, sizeof out, 0) == 0, "foreign: mirror");
    CHECK(sw_proto_handle(&P, c, sizeof c, out, sizeof out, 0) == 0, "foreign: SX");
    CHECK(sw_proto_handle(&P, d, sizeof d, out, sizeof out, 0) == 0, "foreign: Yamaha");
    CHECK(sw_proto_handle(&P, e, sizeof e, out, sizeof out, 0) == 0, "foreign: not F0");
    CHECK(sw_proto_handle(&P, f, sizeof f, out, sizeof out, 0) == 0, "foreign: short");

    /* ours but malformed: BAD_MSG */
    static const uint8_t g[] = {0xF0, 0x7D, 0x53, 0x57, 0xF7};
    rep_len = sw_proto_handle(&P, g, sizeof g, rep, SW_PROTO_MAX, 0);
    parse(rep, rep_len);
    CHECK(is_nak(0, 0, SW_ERR_BAD_MSG), "no cmd: BAD_MSG");
    static const uint8_t h[] = {0xF0, 0x7D, 0x53, 0x57, 0x01, 0x05, 0x00};      /* no F7 */
    rep_len = sw_proto_handle(&P, h, sizeof h, rep, SW_PROTO_MAX, 0);
    parse(rep, rep_len);
    CHECK(is_nak(1, 5, SW_ERR_BAD_MSG), "no F7: BAD_MSG");
    static const uint8_t i[] = {0xF0, 0x7D, 0x53, 0x57, 0x02, 0x06, 0x00, 0xF7}; /* lone header */
    rep_len = sw_proto_handle(&P, i, sizeof i, rep, SW_PROTO_MAX, 0);
    parse(rep, rep_len);
    CHECK(is_nak(2, 6, SW_ERR_BAD_MSG), "bad packing: BAD_MSG");
    static const uint8_t j[] = {0xF0, 0x7D, 0x53, 0x57, 0x01, 0xF7};             /* no seq */
    rep_len = sw_proto_handle(&P, j, sizeof j, rep, SW_PROTO_MAX, 0);
    parse(rep, rep_len);
    CHECK(is_nak(1, 0, SW_ERR_BAD_MSG), "no seq: BAD_MSG");
    /* too long a payload */
    uint8_t big[300];
    memset(big, 0, sizeof big);
    CHECK(req(SW_CMD_WRITE, 3, big, 200) > 0 && is_nak(SW_CMD_WRITE, 3, SW_ERR_BAD_MSG), "huge WRITE: BAD_MSG");

    /* unknown commands */
    static const int bad[] = {0x00, 0x08, 0x0F, SW_CMD_EVENT, 0x11, 0x40, SW_CMD_NAK};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        req(bad[k], 77, NULL, 0);
        CHECK(is_nak(bad[k], 77, SW_ERR_BAD_CMD), "cmd 0x%02X: BAD_CMD", bad[k]);
    }
    /* NAK exact bytes */
    req(0x08, 0x12, NULL, 0);
    static const uint8_t nakw[] = {0xF0, 0x7D, 0x53, 0x57, 0x7F, 0x12, 0x00, 0x08, 0x02, 0xF7};
    CHECK(rep_len == (int)sizeof nakw && memcmp(rep, nakw, sizeof nakw) == 0, "NAK bytes");
}

/* ---- HELLO ---- */

static void test_hello(void)
{
    reset();
    cur_bank = 4;
    cur_layer = 1;
    req(SW_CMD_HELLO, 0x2A, NULL, 0);
    static const uint8_t raw[17] = {1, 16, 15, 32, 16, SW_BLOB_SIZE & 0xFF, SW_BLOB_SIZE >> 8, 4, 1,
                                    'S', 'W', 'E', 'L', 'L', 0, 0, 0};
    uint8_t want[40] = {0xF0, 0x7D, 0x53, 0x57, 0x01, 0x2A};
    int k = sw_pack7(raw, 17, want + 6, 30);
    want[6 + k] = 0xF7;
    CHECK(rep_len == 7 + k && memcmp(rep, want, (size_t)rep_len) == 0, "HELLO bytes (%d)", rep_len);
    CHECK(rep_len == 6 + 20 + 1, "HELLO length %d", rep_len);
    uint8_t one = 0;
    req(SW_CMD_HELLO, 1, &one, 1);
    CHECK(is_nak(SW_CMD_HELLO, 1, SW_ERR_BAD_MSG), "HELLO with payload");
}

/* ---- READ ---- */

static void test_read(void)
{
    reset();
    banks[2].name[0] = 'Q';
    banks[2].ctl[7].number = 99;
    uint8_t blob[SW_BLOB_SIZE], got[SW_BLOB_SIZE];
    CHECK(sw_store_encode(&banks[2], blob, sizeof blob) == SW_BLOB_SIZE, "encode");
    memset(got, 0, sizeof got);
    int ok = 1;
    for (int ofs = 0; ofs < SW_BLOB_SIZE; ofs += SW_PROTO_CHUNK) {
        int n = SW_BLOB_SIZE - ofs < SW_PROTO_CHUNK ? SW_BLOB_SIZE - ofs : SW_PROTO_CHUNK;
        uint8_t r[4] = {2, (uint8_t)ofs, (uint8_t)(ofs >> 8), (uint8_t)n};
        req(SW_CMD_READ, ofs / 128 + 1, r, 4);
        if (!is_ok(SW_CMD_READ, ofs / 128 + 1) || rraw_len != 3 + n || rraw[0] != 2 || g16(rraw + 1) != ofs) {
            ok = 0;
            break;
        }
        memcpy(got + ofs, rraw + 3, (size_t)n);
    }
    CHECK(ok, "READ chunks");
    CHECK(memcmp(got, blob, sizeof blob) == 0, "READ reassembles the blob");
    /* a full chunk reply fits */
    uint8_t r[4] = {0, 0, 0, 128};
    req(SW_CMD_READ, 5, r, 4);
    CHECK(is_ok(SW_CMD_READ, 5) && rep_len == 6 + 131 + 19 + 1, "READ 128 wire len %d", rep_len);
    /* the last byte */
    uint8_t last[4] = {0, (SW_BLOB_SIZE - 1) & 0xFF, (SW_BLOB_SIZE - 1) >> 8, 1};
    req(SW_CMD_READ, 6, last, 4);
    CHECK(is_ok(SW_CMD_READ, 6) && rraw_len == 4, "READ last byte");
    /* bad arguments */
    static const uint8_t bad[][4] = {
        {16, 0, 0, 1},                                                       /* bank */
        {0, 0, 0, 0},                                                        /* len 0 */
        {0, 0, 0, 129},                                                      /* len > chunk */
        {0, (SW_BLOB_SIZE - 10) & 0xFF, (SW_BLOB_SIZE - 10) >> 8, 11},       /* past the end */
        {0, SW_BLOB_SIZE & 0xFF, SW_BLOB_SIZE >> 8, 1},
        {0, 0xFF, 0xFF, 1},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        req(SW_CMD_READ, 7, bad[i], 4);
        CHECK(is_nak(SW_CMD_READ, 7, SW_ERR_BAD_ARG), "READ bad arg %u", i);
    }
    req(SW_CMD_READ, 8, r, 3);
    CHECK(is_nak(SW_CMD_READ, 8, SW_ERR_BAD_MSG), "READ short");
    req(SW_CMD_READ, 8, NULL, 0);
    CHECK(is_nak(SW_CMD_READ, 8, SW_ERR_BAD_MSG), "READ empty");
}

/* ---- WRITE / COMMIT ---- */

static int write_blob(int bank, const uint8_t *blob, int skip_chunk)
{
    int ok = 1;
    for (int ofs = 0, i = 0; ofs < SW_BLOB_SIZE; ofs += SW_PROTO_CHUNK, i++) {
        if (i == skip_chunk)
            continue;
        int n = SW_BLOB_SIZE - ofs < SW_PROTO_CHUNK ? SW_BLOB_SIZE - ofs : SW_PROTO_CHUNK;
        uint8_t r[3 + SW_PROTO_CHUNK] = {(uint8_t)bank, (uint8_t)ofs, (uint8_t)(ofs >> 8)};
        memcpy(r + 3, blob + ofs, (size_t)n);
        req(SW_CMD_WRITE, 40 + i, r, 3 + n);
        if (!is_ok(SW_CMD_WRITE, 40 + i) || rraw_len != 3 || rraw[0] != bank || g16(rraw + 1) != ofs)
            ok = 0;
    }
    return ok;
}

static int commit(int bank)
{
    uint8_t b = (uint8_t)bank;
    return req(SW_CMD_COMMIT, 60, &b, 1);
}

static void make_bank(sw_bank_t *b)
{
    sw_bank_default(b, 9);
    memcpy(b->name, "EDITED!!", 8);
    b->cycle = 2;
    b->jump_quant = SW_JQ_BAR;
    b->bpm_x10 = 1375;
    b->ctl[3].type = SW_T_NRPN14;
    b->ctl[3].number = 1000;
    b->ctl[3].min = 16000;
    b->ctl[3].max = 20;
    b->tog[5].cc = 7;
    b->snap[4].stored = 1;
    b->snap[4].fade = 3;
    b->snap[4].curve = SW_CURVE_S;
    b->snap[4].toggles = 0xA5A5;
    for (int c = 0; c < SW_CONTROLS; c++)
        b->snap[4].value[c] = (uint16_t)(c * 500);
}

static void test_write_commit(void)
{
    reset();
    sw_bank_t nb;
    make_bank(&nb);
    uint8_t blob[SW_BLOB_SIZE], before[SW_BLOB_SIZE], after[SW_BLOB_SIZE];
    sw_store_encode(&nb, blob, sizeof blob);

    /* COMMIT with nothing staged */
    commit(3);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "COMMIT unstaged");

    /* partial write */
    CHECK(write_blob(3, blob, 7), "WRITE partial");
    commit(3);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "COMMIT partial");
    CHECK(dirty_calls == 0, "no dirty after NAK");
    /* filling the gap later counts (the staging survived NOT_STAGED) */
    {
        uint8_t r[3 + SW_PROTO_CHUNK] = {3, (7 * 128) & 0xFF, (7 * 128) >> 8};
        memcpy(r + 3, blob + 7 * 128, 128);
        req(SW_CMD_WRITE, 50, r, 3 + 128);
        CHECK(is_ok(SW_CMD_WRITE, 50), "WRITE gap");
    }
    /* COMMIT for another bank than the staged one */
    commit(4);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "COMMIT other bank");
    commit(3);
    CHECK(is_ok(SW_CMD_COMMIT, 60) && rraw_len == 1 && rraw[0] == 3, "COMMIT ok");
    sw_store_encode(&banks[3], after, sizeof after);
    CHECK(memcmp(after, blob, sizeof blob) == 0, "COMMIT replaced bank 3");
    CHECK(dirty_calls == 1 && dirty_last == 3, "COMMIT marks bank 3 dirty");
    /* the staging is gone after a COMMIT */
    commit(3);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "COMMIT twice");

    /* WRITE to another bank restarts the staging */
    CHECK(write_blob(5, blob, 3), "WRITE bank 5 partial");
    CHECK(write_blob(6, blob, -1), "WRITE bank 6 full");
    {
        uint8_t r[3 + SW_PROTO_CHUNK] = {5, (3 * 128) & 0xFF, (3 * 128) >> 8};
        memcpy(r + 3, blob + 3 * 128, 128);
        req(SW_CMD_WRITE, 51, r, 3 + 128);
    }
    commit(5);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "restarted staging for bank 5");

    /* corrupt blob: BAD_BLOB, bank unchanged */
    sw_store_encode(&banks[8], before, sizeof before);
    uint8_t bad[SW_BLOB_SIZE];
    memcpy(bad, blob, sizeof bad);
    bad[100] ^= 0x01;
    CHECK(write_blob(8, bad, -1), "WRITE corrupt");
    dirty_calls = 0;
    commit(8);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_BAD_BLOB), "COMMIT corrupt: BAD_BLOB");
    sw_store_encode(&banks[8], after, sizeof after);
    CHECK(memcmp(before, after, sizeof before) == 0, "corrupt COMMIT left bank 8 alone");
    CHECK(dirty_calls == 0, "corrupt COMMIT not dirty");
    commit(8);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_NOT_STAGED), "staging cleared after BAD_BLOB");
    /* bad magic too */
    memcpy(bad, blob, sizeof bad);
    bad[0] = 'X';
    write_blob(8, bad, -1);
    commit(8);
    CHECK(is_nak(SW_CMD_COMMIT, 60, SW_ERR_BAD_BLOB), "COMMIT bad magic");

    /* COMMIT to the current bank: sw_bank_changed marks everything dirty */
    eng.ctl_dirty = 0;
    eng.tog_dirty = 0;
    CHECK(write_blob(0, blob, -1), "WRITE bank 0");
    commit(0);
    CHECK(is_ok(SW_CMD_COMMIT, 60), "COMMIT current");
    CHECK(eng.ctl_dirty == 0xFFFFFFFFu && eng.tog_dirty == 0xFFFF, "COMMIT current -> sw_bank_changed (%08X %04X)",
          (unsigned)eng.ctl_dirty, (unsigned)eng.tog_dirty);
    CHECK(dirty_last == 0, "COMMIT current dirty");
    /* a commit to a bank that isn't current doesn't touch the engine */
    eng.ctl_dirty = 0;
    write_blob(1, blob, -1);
    commit(1);
    CHECK(is_ok(SW_CMD_COMMIT, 60) && eng.ctl_dirty == 0, "COMMIT other bank leaves the engine");

    /* bad arguments */
    {
        uint8_t r[3 + 130];
        memset(r, 0, sizeof r);
        r[0] = 16;
        req(SW_CMD_WRITE, 61, r, 4);
        CHECK(is_nak(SW_CMD_WRITE, 61, SW_ERR_BAD_ARG), "WRITE bank 16");
        r[0] = 0;
        req(SW_CMD_WRITE, 61, r, 3 + 129);
        CHECK(is_nak(SW_CMD_WRITE, 61, SW_ERR_BAD_ARG), "WRITE 129 bytes");
        r[1] = (SW_BLOB_SIZE - 4) & 0xFF;
        r[2] = (SW_BLOB_SIZE - 4) >> 8;
        req(SW_CMD_WRITE, 61, r, 3 + 5);
        CHECK(is_nak(SW_CMD_WRITE, 61, SW_ERR_BAD_ARG), "WRITE past the end");
        req(SW_CMD_WRITE, 61, r, 3 + 4);
        CHECK(is_ok(SW_CMD_WRITE, 61), "WRITE up to the end");
        req(SW_CMD_WRITE, 61, r, 3);
        CHECK(is_nak(SW_CMD_WRITE, 61, SW_ERR_BAD_MSG), "WRITE no data");
        uint8_t b = 16;
        req(SW_CMD_COMMIT, 62, &b, 1);
        CHECK(is_nak(SW_CMD_COMMIT, 62, SW_ERR_BAD_ARG), "COMMIT bank 16");
        req(SW_CMD_COMMIT, 62, NULL, 0);
        CHECK(is_nak(SW_CMD_COMMIT, 62, SW_ERR_BAD_MSG), "COMMIT empty");
    }
}

/* ---- ACTION ---- */

static void test_action(void)
{
    reset();
    g_now_us = 1000;
    const sw_status_t *st;

    /* STORE */
    action(SW_ACT_SET_VALUE, 3, 4321);
    CHECK(is_ok(SW_CMD_ACTION, 9) && rraw_len == 1 && rraw[0] == SW_ACT_SET_VALUE, "SET_VALUE reply");
    CHECK(eng.live[3] == 4321, "SET_VALUE live %u", eng.live[3]);
    action(SW_ACT_STORE, 2, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && rraw[0] == SW_ACT_STORE, "STORE reply");
    CHECK(banks[0].snap[2].stored && banks[0].snap[2].value[3] == 4321, "STORE stored the slot");
    CHECK(dirty_calls == 1 && dirty_last == 0, "STORE marks the bank dirty");
    st = sw_status(&eng);
    CHECK(st->current_slot == 2 && !st->modified, "STORE current slot");

    /* PRESS arms, CANCEL_ARM clears */
    action(SW_ACT_STORE, 5, 0);
    action(SW_ACT_PRESS, 2, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && rraw[0] == SW_ACT_PRESS, "PRESS reply");
    st = sw_status(&eng);
    CHECK(st->armed_slot == 2, "PRESS arms (%d)", st->armed_slot);
    CHECK(sw_targets(&eng) != NULL, "armed: targets");
    action(SW_ACT_CANCEL_ARM, 0, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && rraw[0] == SW_ACT_CANCEL_ARM, "CANCEL_ARM reply");
    st = sw_status(&eng);
    CHECK(st->armed_slot == -1, "CANCEL_ARM");
    /* an empty slot does nothing */
    action(SW_ACT_PRESS, 9, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && sw_status(&eng)->armed_slot == -1, "PRESS empty slot");

    /* JUMP */
    action(SW_ACT_SET_VALUE, 3, 100);
    action(SW_ACT_JUMP, 5, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && rraw[0] == SW_ACT_JUMP, "JUMP reply");
    st = sw_status(&eng);
    CHECK(st->fading || st->current_slot == 5 || st->pending_jump == 5, "JUMP started");

    /* SET_TOGGLE */
    action(SW_ACT_SET_TOGGLE, 6, 1);
    CHECK(is_ok(SW_CMD_ACTION, 9) && (eng.toggles & (1u << 6)), "SET_TOGGLE on");
    action(SW_ACT_SET_TOGGLE, 6, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && !(eng.toggles & (1u << 6)), "SET_TOGGLE off");

    /* LAYER */
    action(SW_ACT_LAYER, 1, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && cur_layer == 1, "LAYER 1");
    action(SW_ACT_LAYER, 0, 0);
    CHECK(cur_layer == 0, "LAYER 0");

    /* SELECT_BANK */
    action(SW_ACT_SELECT_BANK, 5, 0);
    CHECK(is_ok(SW_CMD_ACTION, 9) && cur_bank == 5 && select_calls == 1 && eng.bank == &banks[5], "SELECT_BANK");

    /* bad arguments: nothing happens */
    int calls = dirty_calls;
    static const int bad[][3] = {
        {SW_ACT_SELECT_BANK, 16, 0}, {SW_ACT_PRESS, 15, 0}, {SW_ACT_JUMP, 15, 0}, {SW_ACT_STORE, 15, 0},
        {SW_ACT_STORE, 127, 0}, {SW_ACT_SET_VALUE, 32, 0}, {SW_ACT_SET_VALUE, 0, 16384},
        {SW_ACT_SET_VALUE, 0, 0xFFFF}, {SW_ACT_SET_TOGGLE, 16, 0}, {SW_ACT_SET_TOGGLE, 0, 2},
        {SW_ACT_LAYER, 2, 0}, {0, 0, 0}, {9, 0, 0}, {0x7F, 0, 0},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        action(bad[i][0], bad[i][1], bad[i][2]);
        CHECK(is_nak(SW_CMD_ACTION, 9, SW_ERR_BAD_ARG), "ACTION bad %u", i);
    }
    CHECK(cur_bank == 5 && cur_layer == 0 && dirty_calls == calls && select_calls == 1, "bad ACTIONs did nothing");
    action(SW_ACT_SET_VALUE, 0, 16383);
    CHECK(is_ok(SW_CMD_ACTION, 9) && eng.live[0] == 16383, "SET_VALUE max");
    uint8_t three[3] = {SW_ACT_PRESS, 0, 0};
    req(SW_CMD_ACTION, 10, three, 3);
    CHECK(is_nak(SW_CMD_ACTION, 10, SW_ERR_BAD_MSG), "ACTION short");
}

/* ---- STATE / EVENT ---- */

static void check_event_payload(const char *what)
{
    CHECK(rraw_len == 156, "%s: payload %d", what, rraw_len);
    if (rraw_len != 156)
        return;
    const sw_status_t *st = sw_status(&eng);
    const uint16_t *tg = sw_targets(&eng);
    CHECK(rraw[0] == cur_bank && rraw[1] == cur_layer, "%s: bank/layer", what);
    CHECK((int8_t)rraw[2] == st->current_slot && rraw[3] == (st->modified != 0), "%s: slot/modified", what);
    CHECK((int8_t)rraw[4] == st->armed_slot && rraw[5] == (st->fading != 0) && (int8_t)rraw[6] == st->fade_slot,
          "%s: arm/fade", what);
    CHECK(g16(rraw + 7) == st->fade_progress && (int8_t)rraw[9] == st->pending_jump, "%s: progress/pending", what);
    CHECK(rraw[10] == st->clock_source && rraw[11] == (st->running != 0) && g16(rraw + 12) == st->bpm_x10,
          "%s: clock", what);
    CHECK(g32(rraw + 14) == st->cycle_clocks && g32(rraw + 18) == st->cycle_pos, "%s: cycle", what);
    uint16_t mask = 0;
    for (int s = 0; s < SW_SLOTS; s++)
        if (banks[cur_bank].snap[s].stored)
            mask |= (uint16_t)(1u << s);
    CHECK(g16(rraw + 22) == mask, "%s: stored mask %04X want %04X", what, g16(rraw + 22), mask);
    CHECK(rraw[24] == dirty_flag[cur_bank], "%s: dirty", what);
    CHECK(g16(rraw + 25) == eng.toggles, "%s: toggles", what);
    int ok = 1;
    for (int c = 0; c < SW_CONTROLS; c++)
        if (g16(rraw + 27 + 2 * c) != eng.live[c] || g16(rraw + 92 + 2 * c) != (tg ? tg[c] : 0))
            ok = 0;
    CHECK(ok, "%s: live/targets", what);
    CHECK(rraw[91] == (tg != NULL), "%s: has_targets", what);
}

static int ev(uint32_t ms)
{
    memset(rep, 0xEE, sizeof rep);
    rep_len = sw_proto_event(&P, ms, rep, SW_PROTO_MAX);
    if (rep_len > 0)
        parse(rep, rep_len);
    return rep_len;
}

static void test_state_event(void)
{
    reset();
    g_now_us = 5000;
    sw_tick(&eng, g_now_us);
    req(SW_CMD_STATE, 33, NULL, 0);
    CHECK(is_ok(SW_CMD_STATE, 33) && rep_len == 186, "STATE reply len %d", rep_len);
    check_event_payload("STATE default");
    CHECK(g32(rraw + 14) == 4u * 4u * SW_PPQN, "STATE cycle_clocks %u", (unsigned)g32(rraw + 14));
    CHECK(g16(rraw + 12) == 1200, "STATE bpm");
    CHECK((int8_t)rraw[2] == -1 && rraw[2] == 0xFF, "STATE no current slot = -1");

    /* a richer state: stored slots, armed (targets), toggles, layer, dirty */
    action(SW_ACT_SET_VALUE, 0, 9000);
    action(SW_ACT_SET_VALUE, 31, 77);
    action(SW_ACT_STORE, 0, 0);
    action(SW_ACT_SET_VALUE, 0, 10);
    action(SW_ACT_STORE, 14, 0);
    action(SW_ACT_SET_TOGGLE, 15, 1);
    action(SW_ACT_PRESS, 0, 0);
    action(SW_ACT_LAYER, 1, 0);
    req(SW_CMD_STATE, 34, NULL, 0);
    check_event_payload("STATE armed");
    CHECK(g16(rraw + 22) == 0x4001 && rraw[24] == 1 && rraw[91] == 1 && g16(rraw + 92) == 9000, "STATE armed fields");
    req(SW_CMD_STATE, 35, &(uint8_t){0}, 1);
    CHECK(is_nak(SW_CMD_STATE, 35, SW_ERR_BAD_MSG), "STATE with payload");

    /* not subscribed: no events */
    CHECK(ev(0) == 0 && ev(5000) == 0, "no events unsubscribed");

    /* SUBSCRIBE */
    req(SW_CMD_SUBSCRIBE, 3, &(uint8_t){2}, 1);
    CHECK(is_nak(SW_CMD_SUBSCRIBE, 3, SW_ERR_BAD_ARG), "SUBSCRIBE 2");
    req(SW_CMD_SUBSCRIBE, 3, NULL, 0);
    CHECK(is_nak(SW_CMD_SUBSCRIBE, 3, SW_ERR_BAD_MSG), "SUBSCRIBE empty");
    req(SW_CMD_SUBSCRIBE, 4, &(uint8_t){1}, 1);
    CHECK(is_ok(SW_CMD_SUBSCRIBE, 4) && rraw_len == 1 && rraw[0] == 1, "SUBSCRIBE 1");

    uint32_t t = 100000;
    CHECK(ev(t) == 186 && rcmd == SW_CMD_EVENT && rseq == 0, "first EVENT at once (%d)", rep_len);
    check_event_payload("EVENT");
    CHECK(ev(t) == 0, "no repeat at the same ms");
    CHECK(ev(t + 65) == 0, "nothing at 65 ms");
    CHECK(ev(t + 66) == 0, "unchanged at 66 ms: nothing");
    CHECK(ev(t + 500) == 0, "unchanged at 500 ms: nothing");
    /* a change */
    action(SW_ACT_SET_VALUE, 4, 1234);
    CHECK(ev(t + 501) == 186, "a change goes out");
    check_event_payload("EVENT changed");
    CHECK(g16(rraw + 27 + 8) == 1234, "EVENT live[4]");
    uint32_t t1 = t + 501;
    action(SW_ACT_SET_VALUE, 4, 1235);
    CHECK(ev(t1 + 30) == 0, "change within 66 ms waits");
    CHECK(ev(t1 + 65) == 0, "change within 65 ms waits");
    CHECK(ev(t1 + 66) == 186 && g16(rraw + 35) == 1235, "change at 66 ms goes out");
    t1 += 66;
    /* once a second anyway */
    CHECK(ev(t1 + 999) == 0, "unchanged before a second");
    CHECK(ev(t1 + 1000) == 186, "unchanged after a second: sent");
    t1 += 1000;
    CHECK(ev(t1 + 100) == 0, "and then quiet again");
    /* the full timer restarts on any send */
    action(SW_ACT_SET_VALUE, 4, 1236);
    CHECK(ev(t1 + 500) == 186, "change sent");
    CHECK(ev(t1 + 1000) == 0, "not a second since the last send");
    CHECK(ev(t1 + 1500) == 186, "a second since the last send");
    t1 += 1500;

    /* unsent: retried at the next call */
    action(SW_ACT_SET_VALUE, 4, 2000);
    CHECK(ev(t1 + 70) == 186, "event due");
    sw_proto_event_unsent(&P);
    CHECK(ev(t1 + 71) == 186 && g16(rraw + 35) == 2000, "retried at the next pass");
    CHECK(ev(t1 + 72) == 0, "not again once sent");
    /* a change on another field: toggles, layer, bank */
    action(SW_ACT_SET_TOGGLE, 0, 1);
    CHECK(ev(t1 + 200) == 186 && (g16(rraw + 25) & 1), "toggle change");
    action(SW_ACT_LAYER, 0, 0);
    CHECK(ev(t1 + 300) == 186 && rraw[1] == 0, "layer change");
    dirty_flag[0] = 0;
    CHECK(ev(t1 + 400) == 186 && rraw[24] == 0, "dirty change");

    /* wrap-safe timing */
    uint32_t w = 0xFFFFFFF0u;
    action(SW_ACT_SET_VALUE, 4, 3000);
    CHECK(ev(w) == 186, "sent before wrap");
    action(SW_ACT_SET_VALUE, 4, 3001);
    CHECK(ev(w + 40) == 0, "not yet across the wrap");
    CHECK(ev(w + 66) == 186, "66 ms across the wrap");

    /* re-SUBSCRIBE sends at once; unsubscribe stops */
    req(SW_CMD_SUBSCRIBE, 5, &(uint8_t){1}, 1);
    CHECK(ev(w + 67) == 186, "re-subscribe: at once");
    req(SW_CMD_SUBSCRIBE, 6, &(uint8_t){0}, 1);
    CHECK(is_ok(SW_CMD_SUBSCRIBE, 6) && rraw[0] == 0, "SUBSCRIBE 0");
    action(SW_ACT_SET_VALUE, 4, 9);
    CHECK(ev(w + 2000) == 0 && ev(w + 5000) == 0, "unsubscribed: quiet");

    /* while the clock runs, cycle_pos changes: an EVENT every 66 ms */
    reset();
    req(SW_CMD_SUBSCRIBE, 7, &(uint8_t){1}, 1);
    int sent = 0;
    for (uint32_t ms = 0; ms <= 1000; ms++) {
        sw_tick(&eng, ms * 1000u);
        if (ev(ms) > 0)
            sent++;
    }
    CHECK(sent >= 14 && sent <= 16, "running: %d events in a second", sent);
    /* a too-small out buffer: nothing goes out, nothing is lost */
    reset();
    req(SW_CMD_SUBSCRIBE, 8, &(uint8_t){1}, 1);
    uint8_t tiny[50];
    CHECK(sw_proto_event(&P, 10, tiny, sizeof tiny) == 0, "event: small cap");
    CHECK(ev(11) == 186, "event after a small cap");
}

int main(void)
{
    test_pack();
    test_foreign();
    test_hello();
    test_read();
    test_write_commit();
    test_action();
    test_state_event();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
