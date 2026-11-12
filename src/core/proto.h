/*
 * Swell core: the SysEx protocol between the E16 and the web editor
 * (editor/index.html). USB MIDI only.
 *
 * Every message:  F0 7D 53 57 <cmd> <seq> <payload...> F7
 *   7D       the non-commercial manufacturer ID
 *   53 57    'S' 'W'
 *   cmd      0x00-0x7F (below)
 *   seq      0-127, chosen by the editor; a reply echoes it. Events from
 *            the device use seq 0.
 *   payload  raw bytes packed 7-in-8: each group of up to 7 raw bytes is
 *            preceded by one byte whose bit i (0..6) is the top bit of the
 *            group's byte i; the group's bytes follow with their top bits
 *            cleared. (n raw bytes -> n + ceil(n / 7) encoded.)
 *            Multi-byte numbers inside the raw payload are little-endian.
 *
 * The editor sends one request and waits for its reply (or 500 ms) before
 * the next: the device takes one SysEx at a time. Replies are at most
 * SW_PROTO_MAX bytes on the wire.
 *
 * Requests (editor -> device) and their replies (same cmd, same seq):
 *
 *   0x01 HELLO        req: -
 *                     rep: proto_ver u8 (1), banks u8, slots u8, controls u8,
 *                          toggles u8, blob_size u16, current_bank u8,
 *                          layer u8, name[8] ("SWELL")
 *   0x02 READ         req: bank u8, offset u16, len u8 (1..SW_PROTO_CHUNK)
 *                     rep: bank u8, offset u16, data[len] (the bank's blob,
 *                          store.h, as it is in RAM now)
 *   0x03 WRITE        req: bank u8, offset u16, data[1..SW_PROTO_CHUNK]
 *                          (into a staging blob; offset + len <= SW_BLOB_SIZE)
 *                     rep: bank u8, offset u16
 *   0x04 COMMIT       req: bank u8
 *                     (the staging blob, which must have been fully written
 *                     for this bank since the last COMMIT, is decoded with
 *                     sw_store_decode and replaces the bank; the app saves
 *                     it; if it is the current bank, sw_bank_changed())
 *                     rep: bank u8
 *   0x05 ACTION       req: action u8, a u8, b u16
 *                       1 SELECT_BANK  a = bank
 *                       2 PRESS        a = slot  (sw_slot_press)
 *                       3 JUMP         a = slot
 *                       4 STORE        a = slot
 *                       5 CANCEL_ARM
 *                       6 SET_VALUE    a = control, b = 0..16383
 *                       7 SET_TOGGLE   a = toggle, b = 0 off / 1 on
 *                       8 LAYER        a = 0 / 1
 *                     rep: action u8
 *   0x06 SUBSCRIBE    req: on u8 (1 = send EVENTs, 0 = stop)
 *                     rep: on u8
 *   0x07 STATE        req: -
 *                     rep: the EVENT payload, below
 *
 *   0x10 EVENT        device -> editor, unsolicited, seq 0, about every
 *                     SW_PROTO_EVENT_MS while subscribed (and only when
 *                     something in it changed, plus once a second anyway):
 *                       bank u8, layer u8, current_slot i8, modified u8,
 *                       armed_slot i8, fading u8, fade_slot i8,
 *                       fade_progress u16, pending_jump i8,
 *                       clock_source u8, running u8, bpm_x10 u16,
 *                       cycle_clocks u32, cycle_pos u32,
 *                       stored_mask u16 (bit s = slot s stored),
 *                       dirty u8 (0: saved; 1-255: unsaved, a count
 *                         that changes with every change to the bank,
 *                         on the E16 or from here),
 *                       toggles u16, live[32] u16,
 *                       has_targets u8, targets[32] u16 (zeros if none)
 *                     = 156 raw bytes (179 packed, 186 on the wire)
 *
 *   0x7F NAK          rep instead of the expected reply: cmd u8, err u8
 *                       1 BAD_MSG (too short, bad packing), 2 BAD_CMD,
 *                       3 BAD_ARG, 4 NOT_STAGED (COMMIT without a full
 *                       WRITE), 5 BAD_BLOB (decode failed), 6 BUSY
 */
#ifndef SW_PROTO_H
#define SW_PROTO_H
#include <stdint.h>
#include "model.h"
#include "engine.h"
#include "store.h"

#define SW_PROTO_VER 1
#define SW_PROTO_CHUNK 128   /* data bytes per READ / WRITE */
#define SW_PROTO_MAX 200     /* the longest message on the wire */
#define SW_PROTO_EVENT_MS 66

enum {
    SW_CMD_HELLO = 0x01, SW_CMD_READ, SW_CMD_WRITE, SW_CMD_COMMIT, SW_CMD_ACTION,
    SW_CMD_SUBSCRIBE, SW_CMD_STATE,
    SW_CMD_EVENT = 0x10, SW_CMD_NAK = 0x7F
};
enum { SW_ACT_SELECT_BANK = 1, SW_ACT_PRESS, SW_ACT_JUMP, SW_ACT_STORE, SW_ACT_CANCEL_ARM,
       SW_ACT_SET_VALUE, SW_ACT_SET_TOGGLE, SW_ACT_LAYER };
enum { SW_ERR_BAD_MSG = 1, SW_ERR_BAD_CMD, SW_ERR_BAD_ARG, SW_ERR_NOT_STAGED, SW_ERR_BAD_BLOB,
       SW_ERR_BUSY };

/* What the protocol acts on; the app fills it in. */
typedef struct {
    sw_bank_t *banks;       /* SW_BANKS of them, in RAM */
    sw_engine_t *engine;
    int *current_bank;
    int *layer;
    void *user;
    /* bank b's contents changed (commit or store): the app marks it
     * dirty and saves it later. */
    void (*bank_dirty)(void *user, int bank);
    /* 0 if bank b is saved, else 1-255, changing with every change (for
     * EVENT's dirty). */
    int (*is_dirty)(void *user, int bank);
    /* switch the current bank (the app does sw_set_bank, the OLED etc.) */
    void (*select_bank)(void *user, int bank, uint32_t now);
    /* switch the layer the encoders edit */
    void (*set_layer)(void *user, int layer);
} sw_proto_host_t;

typedef struct {
    sw_proto_host_t host;
    uint8_t stage[SW_BLOB_SIZE];
    uint8_t staged[(SW_BLOB_SIZE + 7) / 8]; /* bit per byte written */
    int stage_bank;                          /* -1 = none */
    int subscribed;
    uint32_t last_event_ms, last_full_ms;
    uint8_t last_event[200];                 /* raw payload of the last EVENT sent */
    int last_event_len;
} sw_proto_t;

void sw_proto_init(sw_proto_t *p, const sw_proto_host_t *host);

/* Handle one received SysEx (F0 ... F7, complete). Writes the reply to
 * out (cap >= SW_PROTO_MAX) and returns its length; 0 if msg isn't ours
 * (wrong ID / prefix: ignore it). */
int sw_proto_handle(sw_proto_t *p, const uint8_t *msg, int len, uint8_t *out, int cap,
                    uint32_t now_us);

/* Call every loop pass; returns an EVENT's length in out when one is due
 * (subscribed and SW_PROTO_EVENT_MS since the last, and something
 * changed or a second has passed), else 0. now_ms from millis(). If the
 * caller can't send it, call sw_proto_event_unsent() so it's retried. */
int sw_proto_event(sw_proto_t *p, uint32_t now_ms, uint8_t *out, int cap);
void sw_proto_event_unsent(sw_proto_t *p);

/* 7-in-8 packing (exposed for tests): return the encoded / decoded
 * length, or -1 if it doesn't fit cap (decode: or bad input). */
int sw_pack7(const uint8_t *raw, int n, uint8_t *out, int cap);
int sw_unpack7(const uint8_t *enc, int n, uint8_t *out, int cap);

#endif
