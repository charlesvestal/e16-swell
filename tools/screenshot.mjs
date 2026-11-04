// node shot.mjs <url> <out.png> [wait ms] [clip x,y,w,h] [w,h]
import { chromium } from "playwright-core";
const [url, out, waitMs = "3000", clip, vp = "1100,900"] = process.argv.slice(2);
const exe = process.env.CHROME;
const [vw, vh] = vp.split(",").map(Number);
const browser = await chromium.launch({ executablePath: exe, headless: true, args: ["--enable-features=WebMIDI"] });
const ctx = await browser.newContext({ viewport: { width: vw, height: vh }, deviceScaleFactor: 2, colorScheme: process.env.SCHEME || "light" });
await ctx.grantPermissions(["midi", "midi-sysex"]);
const page = await ctx.newPage();
page.on("console", (m) => { if (m.type() === "error") console.error("console:", m.text()); });
await page.goto(url);
await page.waitForTimeout(Number(waitMs));
const opts = { path: out };
if (clip) { const [x, y, width, height] = clip.split(",").map(Number); opts.clip = { x, y, width, height }; }
await page.screenshot(opts);
console.log("wrote", out);
await browser.close();
