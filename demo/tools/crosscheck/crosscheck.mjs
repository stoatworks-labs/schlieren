// The browser demo against the plugin's own harness, picture for picture.
//
//     cmake -B build-strict -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-ffp-contract=off
//     cmake --build build-strict --target sztest
//     node demo/tools/crosscheck/crosscheck.mjs --sztest build-strict/sztest \
//          [--native build/sztest] [--port 8722] [--gpu] [--keep DIR]
//
// The page does not port the plugin, it runs it: schlieren-core.wasm is the
// plugin's C++ compiled unmodified, drawing with its own GLSL. So there is no
// port to hold to the C++ line by line (slipstream's crosscheck does that for
// a JavaScript port). What can still differ is everything underneath: the
// WebAssembly (no fused multiply-add, so the hull, the shells and the beat's
// placement can round differently from an arm64 build) and WebGL2 (GLSL ES
// 3.00 through ANGLE, on SwiftShader or the GPU, against sztest's GL 4.1
// core). This measures what those come to.
//
// For each scenario below it renders the same schedule twice -- `sztest --out`
// (the real plugin class in a GL 4.1 context) and page.mjs in headless Chrome
// (the page's .wasm and shader copy) -- and compares:
//
//   - the air: pulses in flight and terms sent to the GPU, from sztest's
//     summary line and the harness accessors (sz_state). These must agree
//     exactly: they are the same code on the same inputs, and a difference
//     means the schedule or the build is not the same.
//   - the picture, 8 bits a channel: the largest difference, the mean, and the
//     share of pixels more than 2 and more than 8 levels apart, against
//     LIMITS below.
//
// **--sztest must be built with -ffp-contract=off.** WebAssembly has no fused
// multiply-add; Apple clang fuses a*b+c on arm64 by default, so the plugin's
// Apple Silicon slice rounds differently from its Intel slice (whose baseline
// has no FMA) and from this page. Mostly that is invisible, but a shock is a
// jump in pressure, and the knife sees a jump as a deflection a pixel wide: a
// last-bit difference that moves a shock across a pixel boundary changes that
// pixel by up to 175 levels. Measured on 2026-10-28 (the Blast scenario): the
// arm64 sztest against an -ffp-contract=off one differs on 2.9% of pixels by
// more than 8 levels -- exactly as much as the page differs from the arm64
// one -- while the page against the -ffp-contract=off sztest is within a
// level everywhere, on SwiftShader and on the GPU alike. So the strict build
// is what the page must match; --native (the default arm64 build) is compared
// too and reported, never failed on.
//
// Every scenario has Grey Field at 1 or View on Pressure, so the clip never
// reaches the output and the two sides need not share a clip.
//
// Needs node 22 or later (its WebSocket), Google Chrome, and a built sztest
// (cmake --build build). Not run by tools/verify.sh: it needs Chrome.

import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, existsSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, extname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';

const HERE = dirname(fileURLToPath(import.meta.url));
const DEMO = resolve(HERE, '..', '..');
const CHROME = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

const SIZE = [960, 540];
const FRAMES = 150;// sztest --out's default: 2.5 s at 60 fps
const SCENARIOS = [
  { name: 'defaults, Grey Field 1 (knife edge, focused 150 m beyond)', set: ['Grey Field=1'] },
  { name: 'View Pressure', set: ['View=Pressure'] },
  { name: 'Colour, Grey Field 1', set: ['Optics=Colour', 'Grey Field=1'] },
  { name: 'Shadowgraph, a screen 1 km off', set: ['Optics=Shadowgraph', 'Distance=0.5', 'Grey Field=1'] },
  {
    name: 'Blast at 180 dB in a box, on the half beat, from near a corner, focused on the air',
    set: ['Pulse=Blast', `Loudness=${(180 - 120) / 70}`, 'Walls=Box', 'Fire On=Half Beat', 'Speaker X=0.2', 'Speaker Y=0.25', 'Distance=0', 'Grey Field=1'],
  },
];
/**
 * Past these the picture fails, against the strict build. Measured 2026-10-28:
 * worst 1 level on the Apple GPU and 2 on SwiftShader, mean under 0.01; the
 * limits leave room for another GPU's rounding and none for a real difference.
 */
const LIMITS = { worst: 4, mean: 0.05 };

//---------------------------------------------------------------------------
const args = process.argv.slice(2);
const option = (name, fallback = null) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const sztest = option('--sztest', process.env.SZTEST);
const native = option('--native');
const port = Number(option('--port', 0));
const gpu = args.includes('--gpu');
const keep = option('--keep');
if (!sztest || !existsSync(sztest)) {
  console.error('crosscheck: --sztest PATH (a built tools/sztest) is required');
  process.exit(2);
}
if (!existsSync(CHROME)) {
  console.error(`crosscheck: no Chrome at ${CHROME} (set CHROME=)`);
  process.exit(2);
}

//---------------------------------------------------------------------------
// A static server for demo/, so page.mjs loads the page's own files.
//---------------------------------------------------------------------------
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css' };
const server = createServer((req, res) => {
  const path = resolve(DEMO, `.${decodeURIComponent(new URL(req.url, 'http://x').pathname)}`);
  const file = path.endsWith('/') ? join(path, 'index.html') : path;
  if (!file.startsWith(DEMO) || !existsSync(file)) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream' }).end(readFileSync(file));
});
await new Promise((ok) => server.listen(port, '127.0.0.1', ok));
const origin = `http://127.0.0.1:${server.address().port}`;

//---------------------------------------------------------------------------
// Chrome over the DevTools protocol, on node's own WebSocket.
//---------------------------------------------------------------------------
const profile = mkdtempSync(join(tmpdir(), 'sz-crosscheck-'));
const gl = gpu ? ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist']
  : ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
const chrome = spawn(CHROME, ['--headless=new', '--no-first-run', '--no-default-browser-check', ...gl,
  `--user-data-dir=${profile}`, '--remote-debugging-port=0', 'about:blank'], { stdio: 'ignore' });

async function devtools() {
  const file = join(profile, 'DevToolsActivePort');
  for (let i = 0; i < 150; i += 1) {
    if (existsSync(file)) {
      const [debugPort] = readFileSync(file, 'utf8').split('\n');
      const list = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page.webSocketDebuggerUrl;
    }
    await new Promise((ok) => setTimeout(ok, 200));
  }
  throw new Error('Chrome never exposed a debuggable page');
}

const ws = new WebSocket(await devtools());
await new Promise((ok, fail) => { ws.onopen = ok; ws.onerror = fail; });
let nextId = 0;
const waiting = new Map();
ws.onmessage = (event) => {
  const msg = JSON.parse(event.data);
  if (msg.id && waiting.has(msg.id)) {
    waiting.get(msg.id)(msg);
    waiting.delete(msg.id);
  }
};
const call = (method, params = {}) => new Promise((ok, fail) => {
  nextId += 1;
  waiting.set(nextId, (msg) => (msg.error ? fail(new Error(`${method}: ${msg.error.message}`)) : ok(msg.result)));
  ws.send(JSON.stringify({ id: nextId, method, params }));
});
const evaluate = async (expression) => (await call('Runtime.evaluate', { expression, returnByValue: true })).result?.value;

async function page(scenario) {
  const q = new URLSearchParams({ size: `${SIZE[0]}x${SIZE[1]}`, frames: String(FRAMES), fps: '60', bpm: '120' });
  for (const s of scenario.set) q.append('set', s);
  await call('Page.navigate', { url: `${origin}/tools/crosscheck/index.html?${q}` });
  for (let i = 0; i < 600; i += 1) {
    await new Promise((ok) => setTimeout(ok, 200));
    let done = null;
    try {
      done = await evaluate('window.crosscheck ? JSON.stringify(window.crosscheck) : null');
    } catch {
      // navigation in flight
    }
    if (done) {
      const r = JSON.parse(done);
      if (r.error) throw new Error(`page: ${r.error}`);
      return r;
    }
  }
  throw new Error('page: no result after two minutes');
}

//---------------------------------------------------------------------------
// sztest's side, and its PNG (always filter 0, rows top first: writePng).
//---------------------------------------------------------------------------
function readSztestPng(path) {
  const data = readFileSync(path);
  let at = 8;
  let width = 0;
  let height = 0;
  const idat = [];
  while (at < data.length) {
    const length = data.readUInt32BE(at);
    const type = data.toString('latin1', at + 4, at + 8);
    const body = data.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') { width = body.readUInt32BE(0); height = body.readUInt32BE(4); }
    if (type === 'IDAT') idat.push(body);
    at += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const rgba = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    const row = y * (1 + width * 4);
    if (raw[row] !== 0) throw new Error('sztest PNG row with a filter: not writePng\'s');
    rgba.set(raw.subarray(row + 1, row + 1 + width * 4), y * width * 4);
  }
  return { width, height, rgba };
}

function sztestRun(binary, scenario, out) {
  const argv = ['--out', out, '--size', `${SIZE[0]}x${SIZE[1]}`, '--frames', String(FRAMES), '--fps', '60', '--bpm', '120'];
  for (const s of scenario.set) argv.push('--set', s);
  const said = execFileSync(binary, argv, { encoding: 'utf8' });
  const m = /(\d+) pulses in flight, (\d+) terms/.exec(said);
  if (!m) throw new Error(`sztest said: ${said}`);
  return { pulses: Number(m[1]), terms: Number(m[2]), ...readSztestPng(out) };
}

//---------------------------------------------------------------------------
const work = keep ?? mkdtempSync(join(tmpdir(), 'sz-crosscheck-out-'));
if (keep) mkdirSync(keep, { recursive: true });
let failed = 0;
let renderer = '';

/** The page's frame against one sztest's, and the line that says so. */
function compare(nat, web, pixels) {
  const { width, height } = nat;
  if (web.width !== width || web.height !== height) throw new Error('sizes differ');
  let worst = 0;
  let sum = 0;
  let over2 = 0;
  let over8 = 0;
  const diff = new Uint8Array(width * height * 4);
  // The page's read-back is bottom row first; sztest's PNG is top first.
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const a = (y * width + x) * 4;
      const b = ((height - 1 - y) * width + x) * 4;
      let most = 0;
      for (let c = 0; c < 3; c += 1) {
        const d = Math.abs(nat.rgba[a + c] - pixels[b + c]);
        sum += d;
        most = Math.max(most, d);
      }
      worst = Math.max(worst, most);
      if (most > 2) over2 += 1;
      if (most > 8) over8 += 1;
      diff[a] = diff[a + 1] = diff[a + 2] = Math.min(255, most * 16);
      diff[a + 3] = 255;
    }
  }
  const n = width * height;
  const mean = sum / (n * 3);
  return {
    worst, mean, diff,
    air: web.state.pulses === nat.pulses && web.state.terms === nat.terms,
    picture: worst <= LIMITS.worst && mean <= LIMITS.mean,
    line: `worst ${worst}/255, mean ${mean.toFixed(3)}, ${(100 * over2 / n).toFixed(3)}% of pixels over 2 levels, ${(100 * over8 / n).toFixed(3)}% over 8`,
  };
}

try {
  for (const scenario of SCENARIOS) {
    const slug = scenario.name.replace(/[^a-z0-9]+/gi, '-').toLowerCase().slice(0, 40);
    const strict = sztestRun(sztest, scenario, join(work, `${slug}.sztest.png`));
    const web = await page(scenario);
    renderer = web.renderer;
    const pixels = Buffer.from(web.rgba, 'base64');
    const r = compare(strict, web, pixels);
    if (!r.air || !r.picture) failed += 1;
    console.log(`${r.air && r.picture ? 'ok  ' : 'FAIL'} ${scenario.name}`);
    console.log(`       air: sztest ${strict.pulses} pulses, ${strict.terms} terms; page ${web.state.pulses} pulses, ${web.state.terms} terms (${web.state.shocks} shocks)`);
    console.log(`       picture against sztest -ffp-contract=off: ${r.line}`);
    if (native) {
      const n = compare(sztestRun(native, scenario, join(work, `${slug}.native.png`)), web, pixels);
      console.log(`       picture against the arm64 sztest (FMA; reported only): ${n.line}`);
    }
    if (keep) {
      writeFileSync(join(work, `${slug}.page.rgba`), Buffer.from(pixels));
      writeFileSync(join(work, `${slug}.diff.rgba`), Buffer.from(r.diff));
    }
  }
} finally {
  ws.close();
  chrome.kill();
  server.close();
  setTimeout(() => rmSync(profile, { recursive: true, force: true }), 500);
  if (!keep) rmSync(work, { recursive: true, force: true });
}
console.log(`\n${SCENARIOS.length} scenarios at ${SIZE[0]}x${SIZE[1]}, ${FRAMES} frames each, the page on ${renderer}`);
console.log(failed ? `${failed} FAILED` : 'the page\'s WebAssembly and WebGL2 agree with sztest within the limits above');
process.exit(failed ? 1 : 0);
