/*
 * Swell core: a bank as bytes. The same blob is the file on the disk
 * (/apps/swell/bankNN.bin) and what the editor reads and writes over
 * SysEx (proto.h), so the web editor implements this exact layout too.
 *
 * Blob v1, SW_BLOB_SIZE = 1870 bytes, multi-byte fields little-endian:
 *
 *   off   size  field
 *   0     4     magic 'S' 'W' 'B' '1'
 *   4     8     bank name
 *   12    1     cycle (index into sw_cycle_bars)
 *   13    1     jump_quant
 *   14    1     clock_out (port mask)
 *   15    1     reserved (0)
 *   16    2     bpm_x10
 *   18    576   32 controls, 18 bytes each:
 *                 name[8], type, ports, channel, reserved(0),
 *                 number u16, min u16, max u16
 *   594   224   16 toggles, 14 bytes each:
 *                 name[8], ports, channel, cc, off, on, reserved(0)
 *   818   1050  15 snapshots, 70 bytes each:
 *                 stored, fade, curve, reserved(0), toggles u16,
 *                 value[32] u16
 *   1868  2     CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no
 *               reflection, no final xor) over bytes 0..1867
 */
#ifndef SW_STORE_H
#define SW_STORE_H
#include <stdint.h>
#include "model.h"

#define SW_BLOB_SIZE 1870
#define SW_BLOB_CRC_OFS 1868

/* Errors. */
#define SW_STORE_ESIZE (-1)  /* wrong length */
#define SW_STORE_EMAGIC (-2) /* not 'SWB1' */
#define SW_STORE_ECRC (-3)   /* CRC mismatch */

uint16_t sw_crc16(const uint8_t *p, uint32_t n);

/* Write b as a blob into buf (cap >= SW_BLOB_SIZE); returns SW_BLOB_SIZE,
 * or SW_STORE_ESIZE if cap is too small. */
int sw_store_encode(const sw_bank_t *b, uint8_t *buf, uint32_t cap);

/* Read a blob into *b. On any error *b is left untouched. On success the
 * result is passed through sw_bank_sanitize(). Returns 0 or SW_STORE_E*. */
int sw_store_decode(sw_bank_t *b, const uint8_t *buf, uint32_t len);

#endif
