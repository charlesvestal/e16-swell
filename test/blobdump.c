/*
 * Host tool for the editor codec test (test/editor_codec_test.mjs):
 * blobs made by the C store, for the editor's JS port to check against.
 *
 * stdout, one item per line:
 *   1  hex of sw_store_encode(sw_bank_default(.., 3))
 *   2  hex of a "varied" bank: every field filled with distinct non-default
 *      values from an LCG, then sw_bank_sanitize, then encoded
 *   3  JSON of the varied bank as decoded back from line 2
 *   4  hex of sw_pack7 over the raw bytes 0..255
 *   5  JSON of the default bank 3 (as decoded from line 1)
 *
 * The JSON field names are the struct's (model.h); names are strings with
 * the trailing NUL padding removed.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "model.h"
#include "proto.h"
#include "store.h"

static uint32_t lcg = 0x5EED1234u;
static uint32_t rnd(uint32_t n)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (lcg >> 8) % n;
}

static void fill_name(char *d)
{
    static const char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_+.#";
    int len = (int)rnd(SW_NAME_LEN + 1);
    for (int i = 0; i < SW_NAME_LEN; i++)
        d[i] = i < len ? cs[rnd(sizeof cs - 1)] : 0;
}

static void varied(sw_bank_t *b)
{
    sw_bank_default(b, 11);
    fill_name(b->name);
    b->cycle = (uint8_t)rnd(SW_CYCLES);
    b->jump_quant = (uint8_t)rnd(SW_JQ_COUNT);
    b->clock_out = (uint8_t)rnd(8);
    b->bpm_x10 = (uint16_t)(400 + rnd(2601));
    for (int c = 0; c < SW_CONTROLS; c++) {
        sw_control_t *k = &b->ctl[c];
        fill_name(k->name);
        k->type = (uint8_t)rnd(SW_T_COUNT);
        k->ports = (uint8_t)rnd(8);
        k->channel = (uint8_t)rnd(16);
        int wide = k->type == SW_T_CC14 || k->type == SW_T_NRPN14;
        int nrpn = k->type == SW_T_NRPN7 || k->type == SW_T_NRPN14;
        k->number = (uint16_t)(nrpn ? rnd(16384) : k->type == SW_T_CC14 ? rnd(32) : rnd(128));
        k->min = (uint16_t)rnd(wide ? 16384 : 128);
        k->max = (uint16_t)rnd(wide ? 16384 : 128);
    }
    for (int t = 0; t < SW_TOGGLES; t++) {
        sw_toggle_t *g = &b->tog[t];
        fill_name(g->name);
        g->ports = (uint8_t)rnd(8);
        g->channel = (uint8_t)rnd(16);
        g->cc = (uint8_t)rnd(128);
        g->off = (uint8_t)rnd(128);
        g->on = (uint8_t)rnd(128);
    }
    for (int s = 0; s < SW_SLOTS; s++) {
        sw_snapshot_t *n = &b->snap[s];
        n->stored = (uint8_t)(s % 3 != 1);
        n->fade = (uint8_t)rnd(SW_FADES);
        n->curve = (uint8_t)rnd(SW_CURVE_COUNT);
        n->toggles = (uint16_t)rnd(65536);
        for (int c = 0; c < SW_CONTROLS; c++)
            n->value[c] = (uint16_t)rnd(SW_VMAX + 1);
    }
    sw_bank_sanitize(b);
}

static void hex(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static void jname(const char *n)
{
    int len = SW_NAME_LEN;
    while (len > 0 && n[len - 1] == 0)
        len--;
    putchar('"');
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)n[i];
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 0x20 || ch > 0x7E)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}

static void json(const sw_bank_t *b)
{
    printf("{\"name\":");
    jname(b->name);
    printf(",\"cycle\":%u,\"jump_quant\":%u,\"clock_out\":%u,\"bpm_x10\":%u,\"ctl\":[", b->cycle, b->jump_quant,
           b->clock_out, b->bpm_x10);
    for (int c = 0; c < SW_CONTROLS; c++) {
        const sw_control_t *k = &b->ctl[c];
        printf("%s{\"name\":", c ? "," : "");
        jname(k->name);
        printf(",\"type\":%u,\"ports\":%u,\"channel\":%u,\"number\":%u,\"min\":%u,\"max\":%u}", k->type, k->ports,
               k->channel, k->number, k->min, k->max);
    }
    printf("],\"tog\":[");
    for (int t = 0; t < SW_TOGGLES; t++) {
        const sw_toggle_t *g = &b->tog[t];
        printf("%s{\"name\":", t ? "," : "");
        jname(g->name);
        printf(",\"ports\":%u,\"channel\":%u,\"cc\":%u,\"off\":%u,\"on\":%u}", g->ports, g->channel, g->cc, g->off,
               g->on);
    }
    printf("],\"snap\":[");
    for (int s = 0; s < SW_SLOTS; s++) {
        const sw_snapshot_t *n = &b->snap[s];
        printf("%s{\"stored\":%u,\"fade\":%u,\"curve\":%u,\"toggles\":%u,\"value\":[", s ? "," : "", n->stored,
               n->fade, n->curve, n->toggles);
        for (int c = 0; c < SW_CONTROLS; c++)
            printf("%s%u", c ? "," : "", n->value[c]);
        printf("]}");
    }
    printf("]}\n");
}

int main(void)
{
    static sw_bank_t def, var, back;
    static uint8_t b1[SW_BLOB_SIZE], b2[SW_BLOB_SIZE];
    sw_bank_default(&def, 3);
    varied(&var);
    if (sw_store_encode(&def, b1, sizeof b1) != SW_BLOB_SIZE || sw_store_encode(&var, b2, sizeof b2) != SW_BLOB_SIZE) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    hex(b1, SW_BLOB_SIZE);
    hex(b2, SW_BLOB_SIZE);
    if (sw_store_decode(&back, b2, SW_BLOB_SIZE) != 0) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    json(&back);
    uint8_t raw[256], enc[300];
    for (int i = 0; i < 256; i++)
        raw[i] = (uint8_t)i;
    int k = sw_pack7(raw, 256, enc, sizeof enc);
    if (k < 0) {
        fprintf(stderr, "pack failed\n");
        return 1;
    }
    hex(enc, k);
    if (sw_store_decode(&back, b1, SW_BLOB_SIZE) != 0) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    json(&back);
    return 0;
}
