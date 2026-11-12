/*
 * Host tests for the settings pages' fields (src/core/fields.c). Plain
 * C; prints "ALL PASSED" and exits 0 on success.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "model.h"
#include "fields.h"

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

static sw_bank_t bank;

static const char *txt(const sw_ftarget_t *t, int f, int lf)
{
    static char s[24];
    sw_field_text(t, f, lf, s);
    return s;
}

static void test_grid(void)
{
    int seen[SW_F_COUNT] = {0};
    for (int e = 0; e < 16; e++) {
        int k = sw_field_at(SW_PAGE_KNOB, e), b = sw_field_at(SW_PAGE_BANK, e);
        if (k >= 0)
            seen[k]++;
        if (b >= 0)
            seen[b]++;
        CHECK(strlen(sw_field_label(k)) <= 4, "label %d", k);
    }
    for (int f = 0; f < SW_F_COUNT; f++)
        CHECK(seen[f] == 1, "field %d appears %d times", f, seen[f]);
    CHECK(sw_field_at(SW_PAGE_KNOB, 0) == SW_F_TYPE, "TYPE on encoder 1");
    CHECK(sw_field_at(SW_PAGE_BANK, 0) == SW_F_CYCLE, "CYCL on encoder 1");
    CHECK(sw_field_at(SW_PAGE_NONE, 0) == -1, "no page");
    CHECK(sw_field_is_name(SW_F_TNAME) && !sw_field_is_name(SW_F_TCC), "names");
}

static void test_control_fields(void)
{
    sw_bank_default(&bank, 0);
    sw_ftarget_t t = {&bank, 3, 3};
    sw_control_t *k = &bank.ctl[3];

    CHECK(!strcmp(txt(&t, SW_F_TYPE, 0), "CC7"), "type %s", txt(&t, SW_F_TYPE, 0));
    CHECK(!strcmp(txt(&t, SW_F_CH, 0), "1"), "ch");
    CHECK(!strcmp(txt(&t, SW_F_PORT, 0), "1-U"), "ports %s", txt(&t, SW_F_PORT, 0));
    CHECK(!strcmp(txt(&t, SW_F_PORT, 1), "TRS1+USB"), "ports long %s", txt(&t, SW_F_PORT, 1));

    /* channel clamps at 16 */
    for (int i = 0; i < 20; i++)
        sw_field_turn(&t, SW_F_CH, 1, 0);
    CHECK(k->channel == 15, "ch %d", k->channel);
    CHECK(!sw_field_turn(&t, SW_F_CH, 1, 0), "no change past 16");

    /* number: coarse is 10 on a 7-bit CC, clamps at 127 */
    k->number = 20;
    sw_field_turn(&t, SW_F_NUM, 2, 1);
    CHECK(k->number == 40, "num %d", k->number);
    sw_field_turn(&t, SW_F_NUM, 50, 1);
    CHECK(k->number == 127, "num %d", k->number);

    /* CC7 -> CC14: min/max scale up, number clamps to 31 */
    k->min = 10;
    k->max = 127;
    CHECK(sw_field_turn(&t, SW_F_TYPE, 1, 0), "type changed");
    CHECK(k->type == SW_T_CC14, "type %d", k->type);
    CHECK(k->min == 1290 && k->max == 16383, "min %d max %d", k->min, k->max);
    CHECK(k->number == 31, "num %d", k->number);
    CHECK(!strcmp(txt(&t, SW_F_NUM, 1), "31 (LSB 63)"), "num long %s", txt(&t, SW_F_NUM, 1));
    /* 14-bit coarse is 128 */
    sw_field_turn(&t, SW_F_MIN, 1, 1);
    CHECK(k->min == 1418, "min %d", k->min);
    /* CC14 -> NRPN7: back to 7 bits */
    sw_field_turn(&t, SW_F_TYPE, 1, 0);
    CHECK(k->type == SW_T_NRPN7 && k->min == 1418 >> 7 && k->max == 127, "nrpn7 %d %d %d", k->type, k->min, k->max);
    /* NRPN numbers go to 16383 */
    sw_field_turn(&t, SW_F_NUM, 200, 1);
    CHECK(k->number == 16383, "nrpn num %d", k->number);
    /* down to OFF: the number keeps the 31 it clamped to at CC14 */
    sw_field_turn(&t, SW_F_TYPE, -1, 0);
    sw_field_turn(&t, SW_F_TYPE, -1, 0);
    sw_field_turn(&t, SW_F_TYPE, -1, 0);
    CHECK(k->type == SW_T_OFF && k->number == 31, "off %d %d", k->type, k->number);
    CHECK(!sw_field_turn(&t, SW_F_TYPE, -1, 0), "OFF is the bottom");

    /* ports step through the masks */
    k->ports = 0;
    CHECK(!strcmp(txt(&t, SW_F_PORT, 0), "off"), "no ports");
    sw_field_turn(&t, SW_F_PORT, 1, 0);
    CHECK(k->ports == SW_PORT_TRS1, "ports %d", k->ports);
    for (int i = 0; i < 10; i++)
        sw_field_turn(&t, SW_F_PORT, 1, 0);
    CHECK(k->ports == SW_PORT_ALL, "ports %d", k->ports);
    CHECK(!strcmp(txt(&t, SW_F_PORT, 0), "12U"), "all ports %s", txt(&t, SW_F_PORT, 0));

    int pos, hi;
    CHECK(sw_field_pos(&t, SW_F_CH, &pos, &hi) && pos == 15 && hi == 15, "pos");
    CHECK(!sw_field_pos(&t, SW_F_NAME, &pos, &hi), "a name has no position");
}

static void test_toggle_fields(void)
{
    sw_bank_default(&bank, 0);
    sw_ftarget_t t = {&bank, 20, 4};
    sw_toggle_t *g = &bank.tog[4];
    sw_field_turn(&t, SW_F_TCC, -3, 1);
    CHECK(g->cc == 54, "tcc %d", g->cc);
    sw_field_turn(&t, SW_F_TON, -1, 0);
    CHECK(g->on == 126, "ton %d", g->on);
    sw_field_turn(&t, SW_F_TOFF, 50, 1);
    CHECK(g->off == 127, "toff %d", g->off);
    sw_field_turn(&t, SW_F_TCH, 1, 0);
    CHECK(g->channel == 1 && bank.ctl[20].channel == 0, "tch");
}

static void test_bank_fields(void)
{
    sw_bank_default(&bank, 0);
    sw_ftarget_t t = {&bank, 0, 0};
    CHECK(!strcmp(txt(&t, SW_F_CYCLE, 0), "4bar"), "cycle %s", txt(&t, SW_F_CYCLE, 0));
    sw_field_turn(&t, SW_F_CYCLE, 5, 0);
    CHECK(bank.cycle == 3 && !strcmp(txt(&t, SW_F_CYCLE, 1), "8 bars"), "cycle steps one at a time");
    sw_field_turn(&t, SW_F_CYCLE, -1, 0);
    sw_field_turn(&t, SW_F_CYCLE, -1, 0);
    sw_field_turn(&t, SW_F_CYCLE, -1, 0);
    CHECK(!strcmp(txt(&t, SW_F_CYCLE, 1), "1 bar"), "cycle %s", txt(&t, SW_F_CYCLE, 1));

    CHECK(!strcmp(txt(&t, SW_F_JUMPQ, 0), "now"), "jq");
    sw_field_turn(&t, SW_F_JUMPQ, 1, 0);
    CHECK(bank.jump_quant == SW_JQ_BEAT, "jq beat");

    sw_field_turn(&t, SW_F_BPM, 3, 0);
    CHECK(bank.bpm_x10 == 1230, "bpm %d", bank.bpm_x10);
    sw_field_turn(&t, SW_F_BPM, -4, 1);
    CHECK(bank.bpm_x10 == 1226 && !strcmp(txt(&t, SW_F_BPM, 0), "122.6"), "bpm fine %s", txt(&t, SW_F_BPM, 0));
    sw_field_turn(&t, SW_F_BPM, -500, 0);
    CHECK(bank.bpm_x10 == 400, "bpm floor %d", bank.bpm_x10);

    sw_field_turn(&t, SW_F_CLOCK, 4, 0);
    CHECK(bank.clock_out == SW_PORT_TRS1, "clock out steps one mask at a time: %d", bank.clock_out);
}

static void test_all(void)
{
    sw_bank_default(&bank, 0);
    sw_ftarget_t t = {&bank, 0, 0};
    CHECK(!strcmp(txt(&t, SW_F_ALLCH, 0), "1"), "allch");
    sw_field_turn(&t, SW_F_ALLCH, 1, 0);
    int ok = 1;
    for (int c = 0; c < SW_CONTROLS; c++)
        ok &= bank.ctl[c].channel == 1;
    for (int g = 0; g < SW_TOGGLES; g++)
        ok &= bank.tog[g].channel == 1;
    CHECK(ok, "all on channel 2");

    /* mixed: the first turn unifies on the first control's channel */
    bank.ctl[7].channel = 9;
    CHECK(!strcmp(txt(&t, SW_F_ALLCH, 0), "mix"), "mixed %s", txt(&t, SW_F_ALLCH, 0));
    int pos, hi;
    CHECK(!sw_field_pos(&t, SW_F_ALLCH, &pos, &hi), "mixed has no position");
    sw_field_turn(&t, SW_F_ALLCH, -1, 0);
    CHECK(bank.ctl[7].channel == 1 && bank.tog[15].channel == 1, "unified");

    /* all ports: only what sends; never "none" */
    bank.ctl[5].type = SW_T_OFF;
    bank.ctl[5].ports = 0;
    bank.tog[2].ports = 0;
    CHECK(!strcmp(txt(&t, SW_F_ALLPORT, 0), "1-U"), "allport %s", txt(&t, SW_F_ALLPORT, 0));
    sw_field_turn(&t, SW_F_ALLPORT, 1, 0);
    CHECK(bank.ctl[0].ports == 6 && bank.ctl[5].ports == 0 && bank.tog[2].ports == 0 && bank.tog[3].ports == 6,
          "ports set where something sends");
    for (int i = 0; i < 10; i++)
        sw_field_turn(&t, SW_F_ALLPORT, -1, 0);
    CHECK(bank.ctl[0].ports == 1, "floor at TRS1: %d", bank.ctl[0].ports);

    /* nothing sends */
    for (int c = 0; c < SW_CONTROLS; c++)
        bank.ctl[c].type = SW_T_OFF;
    for (int g = 0; g < SW_TOGGLES; g++)
        bank.tog[g].ports = 0;
    CHECK(!strcmp(txt(&t, SW_F_ALLPORT, 0), "-"), "nothing sends");
    CHECK(!sw_field_turn(&t, SW_F_ALLPORT, 1, 0), "nothing to set");
}

static void test_names(void)
{
    sw_bank_default(&bank, 0);
    sw_ftarget_t t = {&bank, 0, 0};
    char *n = sw_field_name(&t, SW_F_NAME);
    CHECK(n == bank.ctl[0].name, "control name");
    CHECK(sw_field_name(&t, SW_F_BNAME) == bank.name, "bank name");
    CHECK(sw_field_name(&t, SW_F_TNAME) == bank.tog[0].name, "toggle name");

    /* "A1": turn character 0 from A to C, add a character at 3 */
    sw_name_turn(n, 0, 2);
    CHECK(!memcmp(n, "C1\0\0\0\0\0\0", 8), "C1");
    sw_name_turn(n, 3, 1); /* space -> A */
    CHECK(!memcmp(n, "C1 A\0\0\0\0", 8), "%.8s", n);
    /* back to space at the end: trimmed to NUL */
    sw_name_turn(n, 3, -1);
    CHECK(!memcmp(n, "C1\0\0\0\0\0\0", 8), "trimmed");
    /* wraps: space - 1 is the last character, z */
    sw_name_turn(n, 7, -1);
    CHECK(n[7] == 'z' && n[2] == ' ', "wrap %.8s", n);
    /* a character outside the set starts from space */
    n[0] = '~';
    sw_name_turn(n, 0, 1);
    CHECK(n[0] == 'A', "outside the set");
    CHECK(!sw_name_turn(n, 8, 1) && !sw_name_turn(n, 0, 0), "out of range");
    CHECK(!strcmp(txt(&t, SW_F_NAME, 0), "edit"), "name cell");
}

int main(void)
{
    test_grid();
    test_control_fields();
    test_toggle_fields();
    test_bank_fields();
    test_all();
    test_names();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
