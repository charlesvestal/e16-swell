/*
 * Swell core: a bank as bytes (store.h has the exact layout).
 */
#include "store.h"

#define CTL_OFS 18
#define CTL_SIZE 18
#define TOG_OFS 594
#define TOG_SIZE 14
#define SNAP_OFS 818
#define SNAP_SIZE 70

/* The layout must add up (C99 has no static_assert: a negative array size
 * fails the build). */
typedef char sw_layout_check[(CTL_OFS + SW_CONTROLS * CTL_SIZE == TOG_OFS &&
                              TOG_OFS + SW_TOGGLES * TOG_SIZE == SNAP_OFS &&
                              SNAP_OFS + SW_SLOTS * SNAP_SIZE == SW_BLOB_CRC_OFS &&
                              SW_BLOB_CRC_OFS + 2 == SW_BLOB_SIZE) ? 1 : -1];

uint16_t sw_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_name(uint8_t *p, const char *name)
{
    for (int i = 0; i < SW_NAME_LEN; i++)
        p[i] = (uint8_t)name[i];
}

static void get_name(char *name, const uint8_t *p)
{
    for (int i = 0; i < SW_NAME_LEN; i++)
        name[i] = (char)p[i];
}

int sw_store_encode(const sw_bank_t *b, uint8_t *buf, uint32_t cap)
{
    if (cap < SW_BLOB_SIZE)
        return SW_STORE_ESIZE;
    for (int i = 0; i < SW_BLOB_SIZE; i++) /* every reserved byte is 0 */
        buf[i] = 0;
    buf[0] = 'S';
    buf[1] = 'W';
    buf[2] = 'B';
    buf[3] = '1';
    put_name(buf + 4, b->name);
    buf[12] = b->cycle;
    buf[13] = b->jump_quant;
    buf[14] = b->clock_out;
    put16(buf + 16, b->bpm_x10);
    for (int c = 0; c < SW_CONTROLS; c++) {
        const sw_control_t *k = &b->ctl[c];
        uint8_t *p = buf + CTL_OFS + c * CTL_SIZE;
        put_name(p, k->name);
        p[8] = k->type;
        p[9] = k->ports;
        p[10] = k->channel;
        put16(p + 12, k->number);
        put16(p + 14, k->min);
        put16(p + 16, k->max);
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        const sw_toggle_t *g = &b->tog[t];
        uint8_t *p = buf + TOG_OFS + t * TOG_SIZE;
        put_name(p, g->name);
        p[8] = g->ports;
        p[9] = g->channel;
        p[10] = g->cc;
        p[11] = g->off;
        p[12] = g->on;
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        const sw_snapshot_t *n = &b->snap[s];
        uint8_t *p = buf + SNAP_OFS + s * SNAP_SIZE;
        p[0] = n->stored;
        p[1] = n->fade;
        p[2] = n->curve;
        put16(p + 4, n->toggles);
        for (int c = 0; c < SW_CONTROLS; c++)
            put16(p + 6 + 2 * c, n->value[c]);
    }
    put16(buf + SW_BLOB_CRC_OFS, sw_crc16(buf, SW_BLOB_CRC_OFS));
    return SW_BLOB_SIZE;
}

int sw_store_decode(sw_bank_t *b, const uint8_t *buf, uint32_t len)
{
    /* Every check comes before the first write to *b, so a bad blob leaves
     * it untouched without needing a ~1.9 KB copy on the stack. */
    if (len != SW_BLOB_SIZE)
        return SW_STORE_ESIZE;
    if (buf[0] != 'S' || buf[1] != 'W' || buf[2] != 'B' || buf[3] != '1')
        return SW_STORE_EMAGIC;
    if (get16(buf + SW_BLOB_CRC_OFS) != sw_crc16(buf, SW_BLOB_CRC_OFS))
        return SW_STORE_ECRC;

    get_name(b->name, buf + 4);
    b->cycle = buf[12];
    b->jump_quant = buf[13];
    b->clock_out = buf[14];
    b->bpm_x10 = get16(buf + 16);
    for (int c = 0; c < SW_CONTROLS; c++) {
        sw_control_t *k = &b->ctl[c];
        const uint8_t *p = buf + CTL_OFS + c * CTL_SIZE;
        get_name(k->name, p);
        k->type = p[8];
        k->ports = p[9];
        k->channel = p[10];
        k->number = get16(p + 12);
        k->min = get16(p + 14);
        k->max = get16(p + 16);
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        sw_toggle_t *g = &b->tog[t];
        const uint8_t *p = buf + TOG_OFS + t * TOG_SIZE;
        get_name(g->name, p);
        g->ports = p[8];
        g->channel = p[9];
        g->cc = p[10];
        g->off = p[11];
        g->on = p[12];
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        sw_snapshot_t *n = &b->snap[s];
        const uint8_t *p = buf + SNAP_OFS + s * SNAP_SIZE;
        n->stored = p[0];
        n->fade = p[1];
        n->curve = p[2];
        n->toggles = get16(p + 4);
        for (int c = 0; c < SW_CONTROLS; c++)
            n->value[c] = get16(p + 6 + 2 * c);
    }
    sw_bank_sanitize(b);
    return 0;
}
