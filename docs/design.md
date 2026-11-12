# Swell: a snapshot-morphing performance controller

Date: 2026-11-03. Status: built (v1).

> Written when Swell lived in the launcher's tree, so paths here say
> `apps/swell/...` and `test/swell/...`; in this repository they are
> `src/...`, `editor/` and `test/`.

Swell turns the E16 into a performance controller: store the state of
every control in snapshots, then fade to one now (jump) or at the end of
the musical cycle (drop). A web
editor sets everything up and shows a live view.

## Decisions

| Topic | Decision |
|---|---|
| Form | Launcher native app (`apps/swell`, `swell.e16app`). The engine is portable C in `apps/swell/core`, so a standalone firmware front end can reuse it later (not in v1). |
| Editor | Setup plus a live view: WebMIDI page `apps/swell/editor/index.html`, SysEx over USB. |
| Controls | 16 encoders × 2 layers = 32 continuous controls, plus 16 toggles (the pushes). |
| Snapshots | 15 slots per bank, 16 banks. |
| Snapshot access | Hold Shift: encoders 1–15 become slots, and encoder 16 is the layer/bank control. |
| Clock | External MIDI clock (USB or TRS in), with a phase-continuous internal tempo as fallback; per-bank cycle length 1/2/4/8/16 bars. |
| v1 scope | Fade curves, hands-on override, CC14/NRPN outputs. Chain is v1.1. MIDI merge is out. |

## Architecture

```
apps/swell/
  core/model.h/.c    controls, snapshots, banks; defaults; sanitize
  core/store.h/.c    bank <-> 1870-byte blob (file and editor format)
  core/engine.h/.c   clock, cycle, arm/drop/jump, fades, override
  core/out.h/.c      live values -> MIDI, dedupe, per-port rate limit
  core/proto.h/.c    SysEx editor protocol
  app.c              E16 front end: input, rings, OLED, files, glue
  ui.c / ui.h        ring + OLED drawing (C, launcher font)
  editor/index.html  single-file web editor
test/swell/          host tests: core_test.c, proto_test.c, editor codec test
```

The headers in `core/` are the contract. Each one documents its exact
behaviour, and `store.h` and `proto.h` document the byte formats the
editor mirrors.

Each loop pass does the following:
1. Read the incoming MIDI. Realtime bytes and Song Position go to the engine, and our SysEx goes to the protocol (its reply goes out on USB).
2. Read the encoders and run the Shift surface, which calls into the engine.
3. Run `sw_tick`.
4. Run `sw_out_service`.
5. Send an EVENT if one is due.
6. Draw the rings and OLED at about 30 Hz.
7. Do any delayed saves.

## Surface

Play mode (Shift up):
- Encoder *i* edits control `layer*16+i`. Turning it overrides any running fade for that control.
- Holding the push while turning gives fine steps. In that case the push does not toggle.
- A push released without a turn flips toggle *i*.
- Rings show the value as an arc in the layer's colour (A teal, B amber), a ghost dot at the target when one exists, and the toggle as one brighter LED at the bottom.

Shift held:
- Encoders 1–15 are the slots.
- Ring states: an empty slot is dark, a stored one is a dim full ring, the current one is bright (pulsing when modified), an armed one shows a segment spinning in time with the beat, and a fading one fills clockwise.
- Push and release in under 600 ms calls `sw_slot_press`, which arms the slot, or jumps to it if it's already armed.
- Holding for 600 ms or more, then releasing, calls `sw_store`; a turn before the release makes it a curve edit instead. The OLED shows the store filling up while you hold.
- Turning a slot changes its fade length. Shift + push + turn changes its curve.
- Encoder 16: Shift + push flips the layer, and Shift + turn changes the bank, which loads it and jumps to its slot 1. Turns while 16 is pushed are ignored: a push often comes with a click of turn, which changed the bank instead of the layer (2026-11-12).
- Exit: the launcher's leave prompt, Shift held on its own for 5 s, then encoder 1 (it replaced the launcher's Shift-held-1-s exit on 2026-11-04); Swell saves, then exits, when `api->exit_pending()` says so.

Settings pages (added 2026-11-12, so that everything the editor sets can be set on the E16 too):
- **Getting in:** hold a push for 1 s without turning. In play mode that opens the **knob page** for that encoder (control `layer*16+i` and toggle *i*); Shift + encoder 16 opens the **bank page**. The ring fills white while you hold, and a turn during the hold cancels it (it's fine steps then). A shorter push still toggles. The launcher's own Shift + hold-16 bootloader gesture only runs in its menu, not in apps (`app_mode.c` only watches for the leave prompt), so the two don't meet. There's no other shortcut: the cycle length, say, is on the bank page only.
- **Layout:** a 4×4 grid, one field per encoder, matching the encoders. The OLED shows half of it at a time, encoders 1–8 or 9–16 (whichever half was turned last; a bar at the right edge says which), as two rows of four cells: a 4-letter label, a rule, the value. All 16 cells at once were too cramped to read.
  - Knob page: `TYPE CH NUM PORT` / `MIN MAX NAME ·` / `TCC TCH TPRT ·` / `TOFF TON TNAM ·` (the top half the control, the bottom half the toggle).
  - Bank page: `CYCL JMPQ BPM CLK` / `ALCH ALPT NAME ·`, the rest empty.
- **Turning** a field changes it. With the push held it's coarse (×128 on a 14-bit range, ×10 on a 0–127 one), except the tempo, which turns in whole BPM and pushed in 0.1.
  - A type change between 7 and 14 bits rescales min/max (×129, or >>7) and clamps the number, as the editor does.
  - Ports step through the 8 masks, shown `1-U` (TRS 1, TRS 2, USB).
  - **ALCH** sets the channel of all 32 controls and 16 toggles; **ALPT** the ports of every control and toggle that sends (type not OFF, ports not none), never to none. Both show `mix` when the bank differs, and the first turn from `mix` makes them all the first one's.
- **Names:** pushing a NAME cell opens the name editor: encoders 1–8 are the 8 characters, each turning through space, A–Z, 0–9, `-+./:#*!?` and a–z (wrapping). Spaces at the end become the NUL padding.
- **The other half:** on the knob page a push on a blank cell (encoders 8, 12, 16, drawn `PUSH ↓ 9-16` / `PUSH ↑ 1-8`) shows the other half of the grid, so you can look without turning anything.
- **Getting out:** any other push, or a Shift tap, goes back (from the name editor to the page, from the page to play).
- **Meanwhile:** the engine runs on: clock, drops and fades. A changed mapping takes effect with the next value sent; nothing is resent on its own.
- **The rings:** each field's ring is an arc to its value, a name field a dim full ring, and the field last turned brighter.
- **The header** shows the page (`A3 CUTOFF  T3`, `BANK 02 JP8 UP`), or for 2 s after a turn the field in full (`channel 2`).
- **Code:** `src/core/fields.c` (portable, host-tested: each field's grid cell, label, text, range and turn) and the gestures in `app.c`, the drawing in `ui.c`. Every change is `bank_dirty()`, so it's saved like any other edit.

OLED, line by line:
1. The bank, the slot (`*` when modified), the layer, the BPM and the clock source.
2. The cycle progress bar (the drop countdown).
3. The armed or fading slot and its fade.
4. The last control touched, with its name and value in output units.
5. A save dot in the top-right corner while the bank is unsaved.

## Persistence

All 16 banks live in RAM (`sw_bank_t banks[16]`, about 30 KB).
- **Files:** each bank is saved as `/apps/swell/bankNN.bin` (NN = 00–15). `/apps/swell/state.bin` holds the current bank and layer.
- **When it saves:** a store, an editor commit or a config edit marks the bank dirty. A dirty bank is written once there's been no change for 2 s and at least 15 s have passed since that file's last write. `E16_EBUSY` means try again later.
- **Loading:** at start every bank file is read and decoded. A missing or bad file falls back to `sw_bank_default`.

## Editor

The editor is one HTML file with no external dependencies, using WebMIDI with SysEx. It connects to the port whose name contains "E16".
- **Connect:** HELLO first, then it reads all 16 banks with READ and decodes them with a JS port of `store.h`.
- **Editing:** it edits a bank's config, meaning per control the name, type, ports, channel, number and min/max; per toggle the name, ports, channel, CC and off/on values; per slot the fade and curve; and the bank's own settings. "Send" writes the blob with WRITE and then sends COMMIT.
- **Changes on the E16:** EVENT's `dirty` byte is 0 when the bank is saved and otherwise an edit count (1–255) that moves on with every change. The editor re-reads a bank whenever it changes, so edits on the E16's settings pages show up; if you have unsent edits for that bank, it keeps them and says so. (An older editor just sees "unsaved", so the protocol version stays 1.)
- **Live view:** this is the main panel. It subscribes to EVENTs and draws 2×16 rings with ghost targets, plus the countdown, armed and fading state, and the clock. Clicking a slot sends PRESS, a jump button sends JUMP, and a store button sends STORE. Dragging a ring sends SET_VALUE.
- **Files:** it can export and import a bank's blob as a `.bin`.

## Error handling

- **Bad files:** a corrupt bank file is never applied (CRC check) and falls back to the default. It's logged.
- **Protocol errors:** any bad request gets a NAK. The device never acts on a bad request.
- **Full outputs:** `out` skips the in-between values, so an output port being full can't queue up stale CCs.
- **Clock dropouts:** if the external clock stops, the internal clock takes over phase-continuously.

## Testing

- `test/swell/core_test.c` covers:
  - store round-trip, CRC and bad-input cases, and sanitize
  - engine timelines: drop at the cycle boundary under internal and external clock, Start/Stop/SPP, jump quantize, every fade curve, toggles at fade start, override during a fade, store/modified, bank change
  - out: mapping, dedupe, the rate limit and round-robin under a full port, and the CC14/NRPN byte sequences
- `test/swell/proto_test.c` covers: pack/unpack, every command, NAKs, staged commit, and EVENT rate and change detection.
- Editor codec: a Node script (`test/swell/editor_codec_test.mjs`) runs the editor's JS blob codec and packing against blobs produced by the C `sw_store_encode` (a small host tool).
- Hardware is checked by hand, by the user.
