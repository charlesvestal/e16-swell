/*
 * Swell core: the settings pages' fields (fields.h).
 */
#include "fields.h"

static const int8_t knob_grid[16] = {
    SW_F_TYPE, SW_F_CH,  SW_F_NUM,   SW_F_PORT,
    SW_F_MIN,  SW_F_MAX, SW_F_NAME,  -1,
    SW_F_TCC,  SW_F_TCH, SW_F_TPORT, -1,
    SW_F_TOFF, SW_F_TON, SW_F_TNAME, -1,
};

static const int8_t bank_grid[16] = {
    SW_F_CYCLE, SW_F_JUMPQ,   SW_F_BPM,   SW_F_CLOCK,
    SW_F_ALLCH, SW_F_ALLPORT, SW_F_BNAME, -1,
    -1, -1, -1, -1,
    -1, -1, -1, -1,
};

static const char *const labels[SW_F_COUNT] = {
    "TYPE", "CH", "NUM", "PORT", "MIN", "MAX", "NAME",
    "TCC", "TCH", "TPRT", "TOFF", "TON", "TNAM",
    "CYCL", "JMPQ", "BPM", "CLK", "ALCH", "ALPT", "NAME",
};

static const char *const longs[SW_F_COUNT] = {
    "type", "channel", "number", "ports", "min", "max", "name",
    "toggle CC", "toggle channel", "toggle ports", "toggle off", "toggle on", "toggle name",
    "cycle", "jump quantize", "tempo", "clock out", "all channels", "all ports", "bank name",
};

int sw_field_at(int page, int enc)
{
    if (enc < 0 || enc > 15)
        return -1;
    if (page == SW_PAGE_KNOB)
        return knob_grid[enc];
    if (page == SW_PAGE_BANK)
        return bank_grid[enc];
    return -1;
}

int sw_field_is_name(int field)
{
    return field == SW_F_NAME || field == SW_F_TNAME || field == SW_F_BNAME;
}

const char *sw_field_label(int field)
{
    return field >= 0 && field < SW_F_COUNT ? labels[field] : "";
}

const char *sw_field_long(int field)
{
    return field >= 0 && field < SW_F_COUNT ? longs[field] : "";
}

/* ---- text ---- */

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_u(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_ports(char *p, uint8_t m, int long_form)
{
    if (!(m & SW_PORT_ALL))
        return put_s(p, long_form ? "none" : "off");
    if (!long_form) {
        *p++ = m & SW_PORT_TRS1 ? '1' : '-';
        *p++ = m & SW_PORT_TRS2 ? '2' : '-';
        *p++ = m & SW_PORT_USB ? 'U' : '-';
        *p = 0;
        return p;
    }
    int first = 1;
    static const char *const names[3] = {"TRS1", "TRS2", "USB"};
    for (int i = 0; i < 3; i++)
        if (m >> i & 1) {
            if (!first)
                p = put_s(p, "+");
            p = put_s(p, names[i]);
            first = 0;
        }
    return p;
}

static char *put_name(char *p, const char *name)
{
    for (int i = 0; i < SW_NAME_LEN && name[i]; i++)
        *p++ = name[i];
    *p = 0;
    return p;
}

/* ---- ranges ---- */

static int wide(uint8_t type)
{
    return type == SW_T_CC14 || type == SW_T_NRPN14;
}

static int num_hi(uint8_t type)
{
    return type == SW_T_CC7 || type == SW_T_OFF ? 127 : type == SW_T_CC14 ? 31 : 16383;
}

static int val_hi(uint8_t type)
{
    return wide(type) ? 16383 : 127;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The controls and toggles that ALL PORTS sets: the ones that send. */
static int sends_ctl(const sw_control_t *k)
{
    return k->type != SW_T_OFF;
}

static int sends_tog(const sw_toggle_t *g)
{
    return g->ports != 0;
}

/* The common channel of every control and toggle, or -1 if mixed. */
static int all_channel(const sw_bank_t *b)
{
    int ch = b->ctl[0].channel;
    for (int c = 0; c < SW_CONTROLS; c++)
        if (b->ctl[c].channel != ch)
            return -1;
    for (int t = 0; t < SW_TOGGLES; t++)
        if (b->tog[t].channel != ch)
            return -1;
    return ch;
}

/* The common ports of the sending controls and toggles: -1 if mixed, -2
 * if nothing sends. *first gets the first one's ports. */
static int all_ports(const sw_bank_t *b, int *first)
{
    int m = -2;
    for (int c = 0; c < SW_CONTROLS; c++)
        if (sends_ctl(&b->ctl[c])) {
            if (m == -2)
                m = *first = b->ctl[c].ports;
            else if (b->ctl[c].ports != m)
                m = -1;
        }
    for (int t = 0; t < SW_TOGGLES; t++)
        if (sends_tog(&b->tog[t])) {
            if (m == -2)
                m = *first = b->tog[t].ports;
            else if (b->tog[t].ports != m)
                m = -1;
        }
    return m;
}

/* ---- text ---- */

void sw_field_text(const sw_ftarget_t *t, int field, int long_form, char *out)
{
    const sw_bank_t *b = t->bank;
    const sw_control_t *k = &b->ctl[t->ctl >= 0 && t->ctl < SW_CONTROLS ? t->ctl : 0];
    const sw_toggle_t *g = &b->tog[t->tog >= 0 && t->tog < SW_TOGGLES ? t->tog : 0];
    static const char *const type_s[SW_T_COUNT] = {"OFF", "CC7", "CC14", "NRP7", "NR14"};
    static const char *const type_l[SW_T_COUNT] = {"off", "CC 7-bit", "CC 14-bit", "NRPN 7-bit", "NRPN 14-bit"};
    static const char *const jq_s[SW_JQ_COUNT] = {"now", "beat", "bar"};
    static const char *const jq_l[SW_JQ_COUNT] = {"none (now)", "next beat", "next bar"};
    char *p = out;
    *p = 0;
    int first = 0, v;
    switch (field) {
    case SW_F_TYPE:
        put_s(p, (long_form ? type_l : type_s)[k->type < SW_T_COUNT ? k->type : 0]);
        break;
    case SW_F_CH:
        put_u(p, (uint32_t)k->channel + 1);
        break;
    case SW_F_NUM:
        p = put_u(p, k->number);
        if (long_form && k->type == SW_T_CC14) {
            p = put_s(p, " (LSB ");
            p = put_u(p, (uint32_t)k->number + 32);
            put_s(p, ")");
        }
        break;
    case SW_F_PORT:
        put_ports(p, k->ports, long_form);
        break;
    case SW_F_MIN:
        put_u(p, k->min);
        break;
    case SW_F_MAX:
        put_u(p, k->max);
        break;
    case SW_F_TCC:
        put_u(p, g->cc);
        break;
    case SW_F_TCH:
        put_u(p, (uint32_t)g->channel + 1);
        break;
    case SW_F_TPORT:
        put_ports(p, g->ports, long_form);
        break;
    case SW_F_TOFF:
        put_u(p, g->off);
        break;
    case SW_F_TON:
        put_u(p, g->on);
        break;
    case SW_F_CYCLE:
        p = put_u(p, sw_cycle_bars[b->cycle < SW_CYCLES ? b->cycle : 0]);
        put_s(p, long_form ? (b->cycle ? " bars" : " bar") : "bar");
        break;
    case SW_F_JUMPQ:
        put_s(p, (long_form ? jq_l : jq_s)[b->jump_quant < SW_JQ_COUNT ? b->jump_quant : 0]);
        break;
    case SW_F_BPM:
        p = put_u(p, b->bpm_x10 / 10u);
        *p++ = '.';
        put_u(p, b->bpm_x10 % 10u);
        break;
    case SW_F_CLOCK:
        put_ports(p, b->clock_out, long_form);
        break;
    case SW_F_ALLCH:
        v = all_channel(b);
        if (v < 0)
            put_s(p, long_form ? "mixed" : "mix");
        else
            put_u(p, (uint32_t)v + 1);
        break;
    case SW_F_ALLPORT:
        v = all_ports(b, &first);
        if (v == -2)
            put_s(p, long_form ? "nothing sends" : "-");
        else if (v == -1)
            put_s(p, long_form ? "mixed" : "mix");
        else
            put_ports(p, (uint8_t)v, long_form);
        break;
    case SW_F_NAME:
    case SW_F_TNAME:
    case SW_F_BNAME:
        if (long_form)
            put_name(p, sw_field_name(t, field));
        else
            put_s(p, "edit");
        break;
    default:
        break;
    }
}

int sw_field_pos(const sw_ftarget_t *t, int field, int *pos, int *hi)
{
    const sw_bank_t *b = t->bank;
    const sw_control_t *k = &b->ctl[t->ctl >= 0 && t->ctl < SW_CONTROLS ? t->ctl : 0];
    const sw_toggle_t *g = &b->tog[t->tog >= 0 && t->tog < SW_TOGGLES ? t->tog : 0];
    int first = 0, v;
    switch (field) {
    case SW_F_TYPE: *pos = k->type; *hi = SW_T_COUNT - 1; return 1;
    case SW_F_CH: *pos = k->channel; *hi = 15; return 1;
    case SW_F_NUM: *pos = k->number; *hi = num_hi(k->type); return 1;
    case SW_F_PORT: *pos = k->ports; *hi = SW_PORT_ALL; return 1;
    case SW_F_MIN: *pos = k->min; *hi = val_hi(k->type); return 1;
    case SW_F_MAX: *pos = k->max; *hi = val_hi(k->type); return 1;
    case SW_F_TCC: *pos = g->cc; *hi = 127; return 1;
    case SW_F_TCH: *pos = g->channel; *hi = 15; return 1;
    case SW_F_TPORT: *pos = g->ports; *hi = SW_PORT_ALL; return 1;
    case SW_F_TOFF: *pos = g->off; *hi = 127; return 1;
    case SW_F_TON: *pos = g->on; *hi = 127; return 1;
    case SW_F_CYCLE: *pos = b->cycle; *hi = SW_CYCLES - 1; return 1;
    case SW_F_JUMPQ: *pos = b->jump_quant; *hi = SW_JQ_COUNT - 1; return 1;
    case SW_F_BPM: *pos = b->bpm_x10 - 400; *hi = 3000 - 400; return 1;
    case SW_F_CLOCK: *pos = b->clock_out; *hi = SW_PORT_ALL; return 1;
    case SW_F_ALLCH:
        v = all_channel(b);
        if (v < 0)
            return 0;
        *pos = v;
        *hi = 15;
        return 1;
    case SW_F_ALLPORT:
        v = all_ports(b, &first);
        if (v < 0)
            return 0;
        *pos = v;
        *hi = SW_PORT_ALL;
        return 1;
    default:
        return 0;
    }
}

/* ---- turning ---- */

/* Step an 8-bit field; returns 1 if it changed. */
static int step8(uint8_t *f, int d, int lo, int hi)
{
    int v = clampi(*f + d, lo, hi);
    if (v == *f)
        return 0;
    *f = (uint8_t)v;
    return 1;
}

static int step16(uint16_t *f, int d, int lo, int hi)
{
    int v = clampi(*f + d, lo, hi);
    if (v == *f)
        return 0;
    *f = (uint16_t)v;
    return 1;
}

/* A big range turns 128 at a time with alt, a 0-127 one 10 at a time. */
static int coarse(int d, int hi, int alt)
{
    if (!alt)
        return d;
    return d * (hi > 127 ? 128 : 10);
}

static int set_type(sw_control_t *k, int d)
{
    int ty = clampi(k->type + (d > 0 ? 1 : -1), 0, SW_T_COUNT - 1);
    if (ty == k->type)
        return 0;
    int was = wide(k->type), now = wide((uint8_t)ty);
    /* moving between 7 and 14 bits rescales min/max, as the editor does */
    if (now && !was) {
        k->min = (uint16_t)clampi(k->min * 129, 0, 16383);
        k->max = (uint16_t)clampi(k->max * 129, 0, 16383);
    } else if (!now && was) {
        k->min >>= 7;
        k->max >>= 7;
    }
    k->type = (uint8_t)ty;
    if (k->number > num_hi(k->type))
        k->number = (uint16_t)num_hi(k->type);
    return 1;
}

static int set_all_channels(sw_bank_t *b, int d)
{
    int ch = all_channel(b);
    /* mixed: the first turn makes them all the first control's channel */
    int to = ch < 0 ? b->ctl[0].channel : clampi(ch + d, 0, 15);
    if (to == ch)
        return 0;
    for (int c = 0; c < SW_CONTROLS; c++)
        b->ctl[c].channel = (uint8_t)to;
    for (int t = 0; t < SW_TOGGLES; t++)
        b->tog[t].channel = (uint8_t)to;
    return 1;
}

static int set_all_ports(sw_bank_t *b, int d)
{
    int first = 0;
    int m = all_ports(b, &first);
    if (m == -2)
        return 0;
    /* 1..7 only: "none" would leave nothing for the next turn to set */
    int to = m < 0 ? first : clampi(m + (d > 0 ? 1 : -1), 1, SW_PORT_ALL);
    if (to == m)
        return 0;
    for (int c = 0; c < SW_CONTROLS; c++)
        if (sends_ctl(&b->ctl[c]))
            b->ctl[c].ports = (uint8_t)to;
    for (int t = 0; t < SW_TOGGLES; t++)
        if (sends_tog(&b->tog[t]))
            b->tog[t].ports = (uint8_t)to;
    return 1;
}

int sw_field_turn(const sw_ftarget_t *t, int field, int delta, int alt)
{
    sw_bank_t *b = t->bank;
    if (!delta || t->ctl < 0 || t->ctl >= SW_CONTROLS || t->tog < 0 || t->tog >= SW_TOGGLES)
        return 0;
    sw_control_t *k = &b->ctl[t->ctl];
    sw_toggle_t *g = &b->tog[t->tog];
    int s = delta > 0 ? 1 : -1, ch = 0;
    switch (field) {
    case SW_F_TYPE: ch = set_type(k, delta); break;
    case SW_F_CH: ch = step8(&k->channel, s, 0, 15); break;
    case SW_F_NUM: ch = step16(&k->number, coarse(delta, num_hi(k->type), alt), 0, num_hi(k->type)); break;
    case SW_F_PORT: ch = step8(&k->ports, s, 0, SW_PORT_ALL); break;
    case SW_F_MIN: ch = step16(&k->min, coarse(delta, val_hi(k->type), alt), 0, val_hi(k->type)); break;
    case SW_F_MAX: ch = step16(&k->max, coarse(delta, val_hi(k->type), alt), 0, val_hi(k->type)); break;
    case SW_F_TCC: ch = step8(&g->cc, coarse(delta, 127, alt), 0, 127); break;
    case SW_F_TCH: ch = step8(&g->channel, s, 0, 15); break;
    case SW_F_TPORT: ch = step8(&g->ports, s, 0, SW_PORT_ALL); break;
    case SW_F_TOFF: ch = step8(&g->off, coarse(delta, 127, alt), 0, 127); break;
    case SW_F_TON: ch = step8(&g->on, coarse(delta, 127, alt), 0, 127); break;
    case SW_F_CYCLE: ch = step8(&b->cycle, s, 0, SW_CYCLES - 1); break;
    case SW_F_JUMPQ: ch = step8(&b->jump_quant, s, 0, SW_JQ_COUNT - 1); break;
    /* whole BPM; alt is the fine one, 0.1 */
    case SW_F_BPM: ch = step16(&b->bpm_x10, alt ? delta : delta * 10, 400, 3000); break;
    case SW_F_CLOCK: ch = step8(&b->clock_out, s, 0, SW_PORT_ALL); break;
    case SW_F_ALLCH: ch = set_all_channels(b, s); break;
    case SW_F_ALLPORT: ch = set_all_ports(b, s); break;
    default: break;
    }
    if (ch)
        sw_bank_sanitize(b);
    return ch;
}

/* ---- names ---- */

char *sw_field_name(const sw_ftarget_t *t, int field)
{
    sw_bank_t *b = t->bank;
    if (field == SW_F_NAME && t->ctl >= 0 && t->ctl < SW_CONTROLS)
        return b->ctl[t->ctl].name;
    if (field == SW_F_TNAME && t->tog >= 0 && t->tog < SW_TOGGLES)
        return b->tog[t->tog].name;
    return b->name;
}

static const char charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+./:#*!?abcdefghijklmnopqrstuvwxyz";
#define CHARSET_N ((int)sizeof charset - 1)

int sw_name_turn(char *name, int pos, int delta)
{
    if (pos < 0 || pos >= SW_NAME_LEN || !delta)
        return 0;
    char buf[SW_NAME_LEN];
    for (int i = 0; i < SW_NAME_LEN; i++)
        buf[i] = name[i] ? name[i] : ' ';
    /* a character outside the set (from the editor) starts from the space */
    int at = 0;
    for (int i = 0; i < CHARSET_N; i++)
        if (charset[i] == buf[pos]) {
            at = i;
            break;
        }
    at = ((at + delta) % CHARSET_N + CHARSET_N) % CHARSET_N;
    buf[pos] = charset[at];
    int end = SW_NAME_LEN;
    while (end > 0 && buf[end - 1] == ' ')
        end--;
    int changed = 0;
    for (int i = 0; i < SW_NAME_LEN; i++) {
        char c = i < end ? buf[i] : 0;
        if (name[i] != c) {
            name[i] = c;
            changed = 1;
        }
    }
    return changed;
}
