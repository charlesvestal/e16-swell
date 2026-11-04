/*
 * Swell core: live values -> MIDI bytes.
 *
 * Each service call sends, for every dirty control and toggle, its
 * *current* value (intermediate values a fade passed through while a
 * port was busy are skipped, never queued). A value whose output-unit
 * result equals the last one sent for that control is not sent again.
 *
 * Mapping: out = min + (max - min) * pos / SW_VMAX, rounded to nearest
 * (works for min > max). Messages (channel ch, no running status):
 *   CC7     Bn number out
 *   CC14    Bn number (out >> 7)  Bn number+32 (out & 127)
 *   NRPN7   Bn 99 (num >> 7)  Bn 98 (num & 127)  Bn 06 out
 *   NRPN14  Bn 99 (num >> 7)  Bn 98 (num & 127)  Bn 06 (out >> 7)  Bn 38 (out & 127)
 *   toggle  Bn cc (on ? on : off)
 * A control or toggle with ports 0 (or type OFF) has its dirty flag
 * cleared and sends nothing.
 *
 * Rate limit: each port has a byte budget refilled continuously at
 * SW_OUT_RATE_TRS / SW_OUT_RATE_USB bytes per second (capped at
 * SW_OUT_BURST bytes). A control's message goes out on a port only when
 * that port's budget covers it and write() accepts it; a control is
 * clean once every port in its mask has its latest value. Dirty
 * controls are serviced round-robin from where the last call stopped, so
 * a 32-control fade can't starve the high-numbered ones.
 */
#ifndef SW_OUT_H
#define SW_OUT_H
#include <stdint.h>
#include "model.h"
#include "engine.h"

#define SW_OUT_PORTS 3               /* 0 TRS 1, 1 TRS 2, 2 USB (bit n of a mask) */
#define SW_OUT_RATE_TRS 2000u        /* of the 3125 bytes/s a TRS port can carry */
#define SW_OUT_RATE_USB 20000u
#define SW_OUT_BURST 96u

/* Queue len bytes on port (0..2): 1 if accepted (all of them), 0 if
 * there was no room (nothing queued). */
typedef int (*sw_write_fn)(void *ctx, int port, const uint8_t *bytes, int len);

typedef struct {
    uint32_t last_us;
    uint32_t budget[SW_OUT_PORTS];        /* bytes, x 1 */
    uint32_t frac[SW_OUT_PORTS];
    uint16_t sent[SW_CONTROLS];           /* last output-unit value sent; 0xFFFF = never */
    uint8_t sent_tog[SW_TOGGLES];         /* 0 off, 1 on, 0xFF never */
    uint8_t pend_ctl[SW_CONTROLS];        /* ports still owed the latest value */
    uint8_t pend_tog[SW_TOGGLES];
    uint8_t rr;                           /* round-robin start */
    uint32_t bytes_out[SW_OUT_PORTS];     /* stats */
} sw_out_t;

void sw_out_init(sw_out_t *o, uint32_t now);
/* Forget what was sent (after a bank change with a new mapping): the next
 * service resends everything dirty even if unchanged. */
void sw_out_forget(sw_out_t *o);
/* Send what's owed; consumes e->ctl_dirty / tog_dirty. */
void sw_out_service(sw_out_t *o, sw_engine_t *e, uint32_t now, sw_write_fn write, void *ctx);

/* Output-unit value of position pos for control c (exposed for the UI's
 * value display and for tests). */
uint16_t sw_out_value(const sw_control_t *c, uint16_t pos);

#endif
