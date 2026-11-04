// The Swell editor's JS codec against the C store (src/core/store.c).
//
//   node test/editor_codec_test.mjs [path/to/swell_blobdump]
//
// Extracts the <script id="swell-codec"> block from
// editor/index.html, runs it in a fresh context and checks it
// against what blobdump (test/blobdump.c) prints: decode of the C
// blobs field by field, encode byte for byte, CRC/magic/size errors,
// 7-in-8 packing against C's, and the request queue against the editor's
// pretend device. Prints "ALL PASSED" last; exits 1 on any failure.

import { readFileSync, existsSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import vm from "node:vm";
import os from "node:os";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
let fails = 0, checks = 0;
function check(cond, msg) {
  checks++;
  if (!cond) { fails++; console.error("FAIL: " + msg); }
}

// ---- load the codec block ----
const html = readFileSync(path.join(root, "editor/index.html"), "utf8");
const m = html.match(/^<script id="swell-codec">\n([\s\S]*?)^<\/script>/m);
if (!m) { console.error("FAIL: no <script id=\"swell-codec\"> block"); process.exit(1); }
const sandbox = { module: { exports: {} }, setTimeout, clearTimeout, console };
vm.createContext(sandbox);
vm.runInContext(m[1], sandbox, { filename: "index.html#swell-codec" });
const X = sandbox.module.exports;
check(X && typeof X.decodeBank === "function" && sandbox.Swell === X, "codec exports");

// ---- run blobdump (build it if needed) ----
let dump = process.argv[2] || path.join(root, "build-host/swell_blobdump");
if (!existsSync(dump)) {
  const out = path.join(os.tmpdir(), "swell-test");
  mkdirSync(out, { recursive: true });
  dump = path.join(out, "swell_blobdump");
  const core = ["model.c", "store.c", "engine.c", "proto.c"].map((f) => path.join(root, "src/core", f));
  execFileSync("cc", ["-std=c99", "-O2", "-Wall", "-Wextra", "-I" + path.join(root, "src/core"),
    path.join(root, "test/blobdump.c"), ...core, "-o", dump], { stdio: "inherit" });
}
const lines = execFileSync(dump, { encoding: "utf8" }).trim().split("\n");
check(lines.length >= 4, `blobdump printed ${lines.length} lines`);
const unhex = (s) => Uint8Array.from(s.match(/../g).map((x) => parseInt(x, 16)));
const hex = (a) => Array.from(a, (b) => b.toString(16).padStart(2, "0")).join("");
const defBlob = unhex(lines[0]), varBlob = unhex(lines[1]);
const varJson = JSON.parse(lines[2]);
const packC = unhex(lines[3]);
const defJson = lines[4] ? JSON.parse(lines[4]) : null;

// deep compare, reporting the first difference
function diff(a, b, where) {
  if (typeof a !== typeof b) return `${where}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
  if (a && typeof a === "object") {
    const ka = Object.keys(a), kb = Object.keys(b);
    if (Array.isArray(a) !== Array.isArray(b) || ka.length !== kb.length) return `${where}: shape ${ka} vs ${kb}`;
    for (const k of ka) {
      if (!(k in b)) return `${where}.${k} missing`;
      const d = diff(a[k], b[k], `${where}.${k}`);
      if (d) return d;
    }
    return null;
  }
  return a === b ? null : `${where}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
}
const plain = (o) => JSON.parse(JSON.stringify(o)); // out of the vm context

// ---- decode ----
check(defBlob.length === 1870 && varBlob.length === 1870, "blob sizes");
const dv = X.decodeBank(varBlob), dd = X.decodeBank(defBlob);
check(dv.ok && dd.ok, "decode ok");
let d = diff(plain(dv.bank), varJson, "varied");
check(!d, "JS decode of the varied blob matches C: " + d);
if (defJson) {
  d = diff(plain(dd.bank), defJson, "default");
  check(!d, "JS decode of the default blob matches C: " + d);
}
// JS's own defaultBank is the C one
d = diff(plain(X.defaultBank(3)), plain(dd.bank), "defaultBank(3)");
check(!d, "JS defaultBank(3) == C sw_bank_default(3): " + d);
check(dd.bank.name === "BANK 04" && dd.bank.ctl[17].name === "B2" && dd.bank.tog[15].name === "T16", "default names");
check(varJson.ctl.some((k) => k.type === 4) && varJson.snap.some((s) => s.stored) && varJson.name.length > 0, "varied bank is varied");

// ---- encode ----
check(hex(X.encodeBank(dv.bank)) === lines[1], "JS encode of the decoded varied bank == C blob");
check(hex(X.encodeBank(dd.bank)) === lines[0], "JS encode of the default bank == C blob");
check(hex(X.encodeBank(X.defaultBank(3))) === lines[0], "JS encode of defaultBank(3) == C blob");
check(hex(X.encodeBank(JSON.parse(lines[2]))) === lines[1], "JS encode of C's JSON == C blob");

// ---- CRC and other errors ----
check(X.crc16(new TextEncoder().encode("123456789")) === 0x29B1, "CRC-16/CCITT-FALSE check value");
for (const at of [0 + 4, 100, 1000, 1867, 1868, 1869]) {
  const b = varBlob.slice();
  b[at] ^= 0x10;
  const r = X.decodeBank(b);
  check(!r.ok && r.err === (at < 4 ? "magic" : "crc"), `flip at ${at}: ${r.err}`);
}
{
  const b = varBlob.slice(); b[0] = 0x58;
  check(X.decodeBank(b).err === "magic", "bad magic");
  check(X.decodeBank(varBlob.slice(0, 1869)).err === "size", "short");
  check(X.decodeBank(new Uint8Array(1871)).err === "size", "long");
  check(X.decodeBank(null).err === "size", "null");
}
// sanitize: out-of-range values are clamped the C way
{
  const b = X.defaultBank(0);
  b.name = "A\x01BCDEFGHIJ"; b.cycle = 9; b.bpm_x10 = 10; b.clock_out = 0xFF;
  b.ctl[0].type = 7; b.ctl[1].type = X.T.CC7; b.ctl[1].number = 300; b.ctl[1].max = 1000;
  b.ctl[2].type = X.T.CC14; b.ctl[2].number = 40; b.ctl[2].max = 20000;
  b.snap[0].value[0] = 20000; b.snap[0].fade = 99;
  const n = X.sanitizeBank(b);
  check(n === 11 && b.name === "A BCDEFG" && b.cycle === 4 && b.bpm_x10 === 400 && b.clock_out === 7 && b.ctl[0].type === 0 &&
    b.ctl[1].number === 127 && b.ctl[1].max === 127 && b.ctl[2].number === 31 && b.ctl[2].max === 16383 &&
    b.snap[0].value[0] === 16383 && b.snap[0].fade === 6, `sanitize (${n} changes)`);
  const e = X.encodeBank(b);
  check(X.decodeBank(e).ok, "a sanitized bank round-trips");
}
// mergeConfig keeps the device's snapshot contents, takes the editor's setup
{
  const fresh = X.cloneBank(dv.bank), mine = X.defaultBank(5);
  mine.snap[2].fade = 0; mine.snap[2].curve = 3;
  const mg = X.mergeConfig(fresh, mine);
  check(mg.name === mine.name && mg.ctl[5].number === mine.ctl[5].number && mg.snap[2].fade === 0 && mg.snap[2].curve === 3 &&
    mg.snap[2].stored === fresh.snap[2].stored && mg.snap[2].value[7] === fresh.snap[2].value[7] &&
    mg.snap[2].toggles === fresh.snap[2].toggles, "mergeConfig");
}

// ---- packing ----
const raw256 = Uint8Array.from({ length: 256 }, (_, i) => i);
check(hex(X.pack7(raw256)) === lines[3], "JS pack7(0..255) == C sw_pack7");
check(hex(X.unpack7(packC)) === hex(raw256), "JS unpack7 of C's packing");
for (const n of [0, 1, 6, 7, 8, 13, 14, 15, 128, 131, 156, 255, 1870]) {
  const r = Uint8Array.from({ length: n }, (_, i) => (i * 97 + 13) & 0xFF);
  const p = X.pack7(r);
  check(p.length === n + Math.ceil(n / 7) && p.every((b) => b < 0x80), `pack7 length/bits ${n}`);
  const u = X.unpack7(p);
  check(u && hex(u) === hex(r), `unpack7 round trip ${n}`);
}
check(X.unpack7([0x00, 0x80]) === null, "unpack7: top bit");
check(X.unpack7([0x00]) === null, "unpack7: lone header");
check(X.unpack7([0x04, 1, 2]) === null, "unpack7: header bit past the group");
check(X.unpack7([0, 1, 2, 3, 4, 5, 6, 7, 0]) === null, "unpack7: trailing header");
check(X.unpack7([]).length === 0, "unpack7: empty");

// ---- messages ----
{
  const msg = X.buildMsg(X.CMD.READ, 5, [1, 0x80, 0x07, 128]);
  check(hex(msg) === "f07d53570205" + hex(X.pack7([1, 0x80, 0x07, 128])) + "f7", "buildMsg bytes");
  const p = X.parseMsg(msg);
  check(p && p.cmd === 2 && p.seq === 5 && hex(p.raw) === "01800780", "parseMsg");
  check(X.parseMsg(Uint8Array.from([0xF0, 0x7D, 0x45, 0x20, 0, 0, 0xF7])) === null, "parseMsg: foreign");
  const ev = { bank: 3, layer: 1, current_slot: -1, modified: 1, armed_slot: 14, fading: 1, fade_slot: 2, fade_progress: 40000,
    pending_jump: -1, clock_source: 1, running: 1, bpm_x10: 1234, cycle_clocks: 7680, cycle_pos: 0x01020304 % 7680,
    stored_mask: 0x4321, dirty: 1, toggles: 0xBEEF, live: Array.from({ length: 32 }, (_, i) => i * 500),
    has_targets: 1, targets: Array.from({ length: 32 }, (_, i) => 16383 - i) };
  const r = X.buildEvent(ev);
  check(r.length === 156, "EVENT raw 156");
  check(X.buildMsg(X.CMD.EVENT, 0, r).length === 186, "EVENT on the wire 186");
  const back = X.parseEvent(r);
  ev.has_targets = 1;
  d = diff(plain(back), plain(ev), "event");
  check(!d, "parseEvent(buildEvent(x)) == x: " + d);
  check(X.parseEvent(r.slice(0, 155)) === null, "parseEvent: short");
  const k = { min: 0, max: 127 }, inv = { min: 127, max: 0 }, w = { min: 100, max: 16000 };
  check(X.outValue(k, 0) === 0 && X.outValue(k, 16383) === 127 && X.outValue(k, 8192) === 64 && X.outValue(inv, 16383) === 0 &&
    X.outValue(inv, 0) === 127 && X.outValue(w, 16383) === 16000, "outValue");
}

// ---- the request queue against the pretend device ----
async function queueTests() {
  const dev = new X.DemoDevice();
  let sent = 0, drop = 0, mute = false;
  const c = new X.Client((bytes) => {
    sent++;
    const copy = Uint8Array.from(bytes);
    if (mute) return;
    if (drop > 0) { drop--; return; }
    setTimeout(() => { const r = dev.handle(copy); if (r) c.handle(r); }, 1);
  }, { timeout: 60 });
  const h = await X.hello(c);
  check(h.ver === 1 && h.banks === 16 && h.blob_size === 1870 && h.name === "SWELL", "HELLO via the queue");
  // READ all of bank 0 in chunks
  const blob = await X.readBank(c, 0);
  check(hex(blob) === hex(X.encodeBank(dev.banks[0])), "readBank");
  // WRITE + COMMIT
  const nb = X.defaultBank(7); nb.name = "SENT";
  await X.writeBank(c, 7, X.encodeBank(nb));
  check(dev.banks[7].name === "SENT", "writeBank + COMMIT");
  // corrupt: BAD_BLOB
  const bad = X.encodeBank(nb); bad[50] ^= 1;
  let err = null;
  try { await X.writeBank(c, 6, bad); } catch (e) { err = e; }
  check(err && err.nak === X.ERR.BAD_BLOB && dev.banks[6].name === "BANK 07", "writeBank corrupt -> NAK BAD_BLOB");
  // NAK on a bad argument
  err = null;
  try { await X.action(c, X.ACT.SET_VALUE, 40, 0); } catch (e) { err = e; }
  check(err && err.nak === X.ERR.BAD_ARG && err.nakName === "BAD_ARG", "NAK BAD_ARG rejects");
  // one lost message: retried once
  drop = 1;
  const before = sent;
  await X.action(c, X.ACT.SET_VALUE, 3, 999);
  check(dev.live[3] === 999 && sent - before === 2, "a dropped request is retried once");
  // two lost: rejects with timeout
  drop = 2; err = null;
  try { await X.action(c, X.ACT.SET_VALUE, 3, 5); } catch (e) { err = e; }
  check(err && err.timeout, "two drops -> timeout");
  // one in flight at a time, in order; coalescing by key
  mute = true;
  const order = [];
  const p1 = c.request(X.CMD.ACTION, [X.ACT.SET_VALUE, 1, 1, 0], { retries: 0 }).catch(() => order.push("p1"));
  const p2 = c.request(X.CMD.ACTION, [X.ACT.SET_VALUE, 1, 2, 0], { key: "v1" }).then(() => order.push("p2"));
  const p3 = c.request(X.CMD.ACTION, [X.ACT.SET_VALUE, 1, 3, 0], { key: "v1" }).then(() => order.push("p3"));
  check(c.q.length === 1, "a queued request with the same key is replaced");
  const s0 = sent;
  await new Promise((r) => setTimeout(r, 20));
  check(sent === s0, "nothing else is sent while one is in flight");
  mute = false;
  await Promise.all([p1, p2, p3]);
  check(order.join() === "p1,p2,p3" && dev.live[1] === 3, `order ${order} live ${dev.live[1]}`);
  // EVENTs
  let evs = 0, last = null;
  c.onEvent = (e) => { evs++; last = e; };
  await X.subscribe(c, true);
  const e = dev.event(1000);
  check(e && e.length === 186, "demo EVENT");
  c.handle(e);
  check(evs === 1 && last.bank === 0 && last.live[1] === 3, "EVENT parsed");
  check(dev.event(1030) === null && dev.event(1066) === null, "demo EVENT only on change");
  check(dev.event(2000) !== null, "demo EVENT once a second");
  check(c.handle(Uint8Array.from([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])) === false, "handle ignores foreign SysEx");
  // reset rejects everything pending
  mute = true; err = null;
  const pr = c.request(X.CMD.HELLO).catch((x) => { err = x; });
  c.reset("bye");
  await pr;
  check(err && err.reset, "reset rejects");
}

await queueTests();

console.log(`${checks} checks, ${fails} failures`);
if (fails) { console.log("FAILED"); process.exit(1); }
console.log("ALL PASSED");
