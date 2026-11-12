/*
 * Swell SysEx editor protocol (proto.h has the wire format).
 *
 * One request in, one reply out. Every request is checked completely
 * before anything is touched: a request that gets a NAK changed nothing.
 *
 * WRITE goes into p->stage with a bit per byte in p->staged; a WRITE for
 * another bank than the one being staged starts the staging over. COMMIT
 * needs every byte of the blob written for that bank; it then decodes the
 * stage into the bank (sw_store_decode leaves the bank alone on any
 * error) and clears the staging either way, so a COMMIT that failed with
 * BAD_BLOB needs a full WRITE again. A COMMIT that fails with NOT_STAGED
 * keeps what was written, so the editor can fill the gaps.
 *
 * EVENT: the payload is built every SW_PROTO_EVENT_MS and compared with
 * the last one sent, byte for byte; it goes out when it differs, or when
 * a second has passed since the last one. last_event_len < 0 means "send
 * at the next call whatever the time" (after SUBSCRIBE and after
 * sw_proto_event_unsent()).
 */
#include "proto.h"

#define HDR_LEN 6           /* F0 7D 53 57 cmd seq */
#define RAW_MAX 160         /* the largest raw request payload we accept (WRITE: 131) */
#define EVENT_RAW 156
#define FULL_MS 1000u

/* Scratch for READ: the bank encoded as a blob. Static, not on the stack
 * (1870 bytes); the protocol is single-threaded. */
static uint8_t s_blob[SW_BLOB_SIZE];

static void zero(void *d, uint32_t n)
{
    uint8_t *b = d;
    while (n--)
        *b++ = 0;
}

static void copy(void *d, const void *s, uint32_t n)
{
    uint8_t *o = d;
    const uint8_t *i = s;
    while (n--)
        *o++ = *i++;
}

/* ---- 7-in-8 packing ---- */

int sw_pack7(const uint8_t *raw, int n, uint8_t *out, int cap)
{
    if (n < 0)
        return -1;
    int need = n + (n + 6) / 7;
    if (need > cap)
        return -1;
    int o = 0;
    for (int i = 0; i < n; i += 7) {
        int g = n - i < 7 ? n - i : 7;
        uint8_t hdr = 0;
        for (int k = 0; k < g; k++)
            hdr |= (uint8_t)(((raw[i + k] >> 7) & 1u) << k);
        out[o++] = hdr;
        for (int k = 0; k < g; k++)
            out[o++] = raw[i + k] & 0x7Fu;
    }
    return o;
}

int sw_unpack7(const uint8_t *enc, int n, uint8_t *out, int cap)
{
    if (n < 0)
        return -1;
    int o = 0, i = 0;
    while (i < n) {
        uint8_t hdr = enc[i++];
        int g = n - i < 7 ? n - i : 7;
        if ((hdr & 0x80u) || g == 0 || (hdr >> g) != 0)
            return -1; /* a stray top bit, a header with no bytes, or bits for bytes not there */
        if (o + g > cap)
            return -1;
        for (int k = 0; k < g; k++) {
            uint8_t b = enc[i + k];
            if (b & 0x80u)
                return -1;
            out[o++] = (uint8_t)(b | (((hdr >> k) & 1u) << 7));
        }
        i += g;
    }
    return o;
}

/* ---- replies ---- */

static int reply(uint8_t *out, int cap, int cmd, int seq, const uint8_t *raw, int n)
{
    if (cap < HDR_LEN + 1)
        return 0;
    out[0] = 0xF0;
    out[1] = 0x7D;
    out[2] = 0x53;
    out[3] = 0x57;
    out[4] = (uint8_t)cmd;
    out[5] = (uint8_t)seq;
    int k = sw_pack7(raw, n, out + HDR_LEN, cap - HDR_LEN - 1);
    if (k < 0)
        return 0;
    out[HDR_LEN + k] = 0xF7;
    return HDR_LEN + k + 1;
}

static int nak(uint8_t *out, int cap, int cmd, int seq, int err)
{
    uint8_t r[2];
    r[0] = (uint8_t)(cmd & 0x7F);
    r[1] = (uint8_t)err;
    return reply(out, cap, SW_CMD_NAK, seq, r, 2);
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- state ---- */

static int event_payload(sw_proto_t *p, uint8_t *r)
{
    const sw_proto_host_t *h = &p->host;
    const sw_status_t *st = sw_status(h->engine);
    int bank = *h->current_bank;
    const sw_bank_t *b = &h->banks[bank];
    const uint16_t *tg = sw_targets(h->engine);
    int o = 0;
    r[o++] = (uint8_t)bank;
    r[o++] = (uint8_t)*h->layer;
    r[o++] = (uint8_t)(int8_t)st->current_slot;
    r[o++] = (uint8_t)(st->modified != 0);
    r[o++] = (uint8_t)(int8_t)st->armed_slot;
    r[o++] = (uint8_t)(st->fading != 0);
    r[o++] = (uint8_t)(int8_t)st->fade_slot;
    put16(r + o, st->fade_progress);
    o += 2;
    r[o++] = (uint8_t)(int8_t)st->pending_jump;
    r[o++] = (uint8_t)st->clock_source;
    r[o++] = (uint8_t)(st->running != 0);
    put16(r + o, st->bpm_x10);
    o += 2;
    put32(r + o, st->cycle_clocks);
    o += 4;
    put32(r + o, st->cycle_pos);
    o += 4;
    uint32_t mask = 0;
    for (int s = 0; s < SW_SLOTS; s++)
        if (b->snap[s].stored)
            mask |= 1u << s;
    put16(r + o, mask);
    o += 2;
    {   /* 0 saved, else a count the host moves on with every change */
        int d = h->is_dirty ? h->is_dirty(h->user, bank) : 0;
        r[o++] = (uint8_t)(d < 0 ? 1 : d > 255 ? 255 : d);
    }
    put16(r + o, h->engine->toggles);
    o += 2;
    for (int c = 0; c < SW_CONTROLS; c++, o += 2)
        put16(r + o, h->engine->live[c]);
    r[o++] = (uint8_t)(tg != 0);
    for (int c = 0; c < SW_CONTROLS; c++, o += 2)
        put16(r + o, tg ? tg[c] : 0);
    return o; /* EVENT_RAW */
}

void sw_proto_init(sw_proto_t *p, const sw_proto_host_t *host)
{
    zero(p, sizeof *p);
    p->host.banks = host->banks;
    p->host.engine = host->engine;
    p->host.current_bank = host->current_bank;
    p->host.layer = host->layer;
    p->host.user = host->user;
    p->host.bank_dirty = host->bank_dirty;
    p->host.is_dirty = host->is_dirty;
    p->host.select_bank = host->select_bank;
    p->host.set_layer = host->set_layer;
    p->stage_bank = -1;
    p->subscribed = 0;
    p->last_event_len = -1;
}

static void stage_clear(sw_proto_t *p)
{
    zero(p->staged, sizeof p->staged);
    p->stage_bank = -1;
}

static int stage_full(const sw_proto_t *p)
{
    for (int i = 0; i < SW_BLOB_SIZE; i++)
        if (!(p->staged[i >> 3] & (1u << (i & 7))))
            return 0;
    return 1;
}

/* ---- requests ---- */

int sw_proto_handle(sw_proto_t *p, const uint8_t *msg, int len, uint8_t *out, int cap,
                    uint32_t now_us)
{
    const sw_proto_host_t *h = &p->host;
    if (!msg || len < 4 || msg[0] != 0xF0 || msg[1] != 0x7D || msg[2] != 0x53 || msg[3] != 0x57)
        return 0;
    int cmd = len > 4 && !(msg[4] & 0x80) ? msg[4] : 0;
    int seq = len > 5 && !(msg[5] & 0x80) ? msg[5] : 0;
    if (len < HDR_LEN + 1 || msg[len - 1] != 0xF7 || (msg[4] & 0x80) || (msg[5] & 0x80))
        return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);

    uint8_t raw[RAW_MAX];
    int n = sw_unpack7(msg + HDR_LEN, len - HDR_LEN - 1, raw, RAW_MAX);
    if (n < 0)
        return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);

    uint8_t r[EVENT_RAW + 4];
    switch (cmd) {
    case SW_CMD_HELLO: {
        if (n != 0)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        static const char name[SW_NAME_LEN] = {'S', 'W', 'E', 'L', 'L', 0, 0, 0};
        r[0] = SW_PROTO_VER;
        r[1] = SW_BANKS;
        r[2] = SW_SLOTS;
        r[3] = SW_CONTROLS;
        r[4] = SW_TOGGLES;
        put16(r + 5, SW_BLOB_SIZE);
        r[7] = (uint8_t)*h->current_bank;
        r[8] = (uint8_t)*h->layer;
        copy(r + 9, name, SW_NAME_LEN);
        return reply(out, cap, cmd, seq, r, 9 + SW_NAME_LEN);
    }
    case SW_CMD_READ: {
        if (n != 4)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        int bank = raw[0], ofs = u16(raw + 1), cnt = raw[3];
        if (bank >= SW_BANKS || cnt < 1 || cnt > SW_PROTO_CHUNK || ofs + cnt > SW_BLOB_SIZE)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
        if (sw_store_encode(&h->banks[bank], s_blob, SW_BLOB_SIZE) != SW_BLOB_SIZE)
            return nak(out, cap, cmd, seq, SW_ERR_BUSY);
        r[0] = (uint8_t)bank;
        put16(r + 1, (uint32_t)ofs);
        copy(r + 3, s_blob + ofs, (uint32_t)cnt);
        return reply(out, cap, cmd, seq, r, 3 + cnt);
    }
    case SW_CMD_WRITE: {
        if (n < 4)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        int bank = raw[0], ofs = u16(raw + 1), cnt = n - 3;
        if (bank >= SW_BANKS || cnt > SW_PROTO_CHUNK || ofs + cnt > SW_BLOB_SIZE)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
        if (p->stage_bank != bank) {
            stage_clear(p);
            p->stage_bank = bank;
        }
        copy(p->stage + ofs, raw + 3, (uint32_t)cnt);
        for (int i = ofs; i < ofs + cnt; i++)
            p->staged[i >> 3] |= (uint8_t)(1u << (i & 7));
        r[0] = (uint8_t)bank;
        put16(r + 1, (uint32_t)ofs);
        return reply(out, cap, cmd, seq, r, 3);
    }
    case SW_CMD_COMMIT: {
        if (n != 1)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        int bank = raw[0];
        if (bank >= SW_BANKS)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
        if (p->stage_bank != bank || !stage_full(p))
            return nak(out, cap, cmd, seq, SW_ERR_NOT_STAGED);
        int rc = sw_store_decode(&h->banks[bank], p->stage, SW_BLOB_SIZE);
        stage_clear(p);
        if (rc != 0)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_BLOB);
        if (h->bank_dirty)
            h->bank_dirty(h->user, bank);
        if (bank == *h->current_bank)
            sw_bank_changed(h->engine, now_us);
        r[0] = (uint8_t)bank;
        return reply(out, cap, cmd, seq, r, 1);
    }
    case SW_CMD_ACTION: {
        if (n != 4)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        int act = raw[0], a = raw[1];
        uint16_t b = u16(raw + 2);
        switch (act) {
        case SW_ACT_SELECT_BANK:
            if (a >= SW_BANKS || !h->select_bank)
                return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
            h->select_bank(h->user, a, now_us);
            break;
        case SW_ACT_PRESS:
        case SW_ACT_JUMP:
        case SW_ACT_STORE:
            if (a >= SW_SLOTS)
                return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
            if (act == SW_ACT_PRESS)
                sw_slot_press(h->engine, a, now_us);
            else if (act == SW_ACT_JUMP)
                sw_jump(h->engine, a, now_us);
            else if (sw_store(h->engine, a) && h->bank_dirty)
                h->bank_dirty(h->user, *h->current_bank);
            break;
        case SW_ACT_CANCEL_ARM:
            sw_cancel_arm(h->engine);
            break;
        case SW_ACT_SET_VALUE:
            if (a >= SW_CONTROLS || b > SW_VMAX)
                return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
            sw_set_value(h->engine, a, b, now_us);
            break;
        case SW_ACT_SET_TOGGLE:
            if (a >= SW_TOGGLES || b > 1)
                return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
            sw_toggle(h->engine, a, b);
            break;
        case SW_ACT_LAYER:
            if (a >= SW_LAYERS || !h->set_layer)
                return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
            h->set_layer(h->user, a);
            break;
        default:
            return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
        }
        r[0] = (uint8_t)act;
        return reply(out, cap, cmd, seq, r, 1);
    }
    case SW_CMD_SUBSCRIBE: {
        if (n != 1)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        if (raw[0] > 1)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_ARG);
        p->subscribed = raw[0];
        p->last_event_len = -1; /* the first EVENT goes out at once */
        r[0] = raw[0];
        return reply(out, cap, cmd, seq, r, 1);
    }
    case SW_CMD_STATE: {
        if (n != 0)
            return nak(out, cap, cmd, seq, SW_ERR_BAD_MSG);
        int k = event_payload(p, r);
        return reply(out, cap, cmd, seq, r, k);
    }
    default:
        return nak(out, cap, cmd, seq, SW_ERR_BAD_CMD);
    }
}

/* ---- events ---- */

int sw_proto_event(sw_proto_t *p, uint32_t now_ms, uint8_t *out, int cap)
{
    if (!p->subscribed)
        return 0;
    int force = p->last_event_len < 0;
    if (!force && now_ms - p->last_event_ms < SW_PROTO_EVENT_MS)
        return 0;
    uint8_t r[EVENT_RAW + 4];
    int n = event_payload(p, r);
    if (!force) {
        int same = n == p->last_event_len;
        for (int i = 0; same && i < n; i++)
            if (r[i] != p->last_event[i])
                same = 0;
        if (same && now_ms - p->last_full_ms < FULL_MS)
            return 0;
    }
    int k = reply(out, cap, SW_CMD_EVENT, 0, r, n);
    if (k <= 0)
        return 0;
    copy(p->last_event, r, (uint32_t)n);
    p->last_event_len = n;
    p->last_event_ms = now_ms;
    p->last_full_ms = now_ms;
    return k;
}

void sw_proto_event_unsent(sw_proto_t *p)
{
    p->last_event_len = -1;
}
