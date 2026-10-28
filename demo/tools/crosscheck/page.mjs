// The browser half of the crosscheck: the page's own schlieren-core.wasm and
// its checked shader copy, driven through exactly the schedule sztest's Rig
// drives the plugin through for `sztest --out`, and the frame read back.
//
//   index.html?size=960x540&frames=150&fps=60&bpm=120&set=Grey%20Field=1&set=...
//
// The schedule, from tools/sztest/main.cpp (Rig::Init and Rig::Render):
//   - InitGL, then SetClockScaleForTest( 1.0 ): the clock is declared seconds
//     from frame 0, with no vote (the page itself never calls this; it lets the
//     plugin vote, as Resolume does);
//   - each --set, as applySetting does it: an option by its element's name,
//     anything else as the host's float;
//   - for frame f = 0 .. frames-1: SetTime( f / fps ), SetBeatInfo( bpm,
//     fmod( f / fps * bpm / 240, 1 ) ), a cleared target, ProcessOpenGL.
//
// The clip is opaque black, at the frame's size (the plugin takes its raster
// from the input's Width and Height, so the size matters and the content does
// not): every scenario crosscheck.mjs runs has Grey Field at 1 (the slide is
// the bare mid-grey field, whatever the clip) or View on Pressure, so the clip
// never reaches the output, and sztest's own look card does not have to be
// rebuilt here.
//
// Nothing here is the page. It is a second host for the same .wasm, built the
// same way (the same shader refusal, the same port()), so that what it measures
// is the WebAssembly and WebGL2 against sztest's GL 4.1, not a third copy.

import createSchlieren from '../../schlieren-core.js';
import { port } from '../../vendor/gl.js';
import * as S from '../../shaders.js';

const FF_TYPE_OPTION = 11;

const ASSEMBLED = Object.fromEntries(
  Object.entries(S.ASSEMBLY).map(([stage, pieces]) => [stage, pieces.map((piece) => S[piece]).join('')]),
);
const FRAGMENT_STAGES = Object.keys(ASSEMBLED).filter((stage) => stage !== 'vertex');
const PAGE_SHADERS = { vertex: [ASSEMBLED.vertex], fragment: FRAGMENT_STAGES.map((stage) => ASSEMBLED[stage]) };
const shaderCheck = { matched: [], refused: null };
function shaderSource(stage, text) {
  const ours = PAGE_SHADERS[stage].indexOf(text);
  if (ours < 0) {
    shaderCheck.refused = { stage, length: text.length };
    return '';
  }
  shaderCheck.matched.push(stage === 'vertex' ? 'vertex' : FRAGMENT_STAGES[ours]);
  return port(text, { stage });
}

function base64(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i += 0x8000) out += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(out);
}

async function run() {
  const q = new URLSearchParams(window.location.search);
  const [width, height] = (q.get('size') ?? '960x540').split('x').map(Number);
  const frames = Number(q.get('frames') ?? 150);
  const fps = Number(q.get('fps') ?? 60);
  const bpm = Number(q.get('bpm') ?? 120);

  const canvas = document.getElementById('out');
  canvas.width = width;
  canvas.height = height;
  const gl = canvas.getContext('webgl2', { alpha: true, antialias: false, depth: false, stencil: false, premultipliedAlpha: true, preserveDrawingBuffer: true });
  if (!gl) throw new Error('no WebGL2');
  if (!gl.getExtension('EXT_color_buffer_float')) throw new Error('no EXT_color_buffer_float');

  const core = await createSchlieren({ schlierenShaderSource: shaderSource, print: () => {}, printErr: () => {} });
  core._sz_prepare_log();
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);

  // The clip: opaque black at the frame's size, under a name the plugin can bind.
  const black = new Uint8Array(width * height * 4);
  for (let i = 3; i < black.length; i += 4) black[i] = 255;
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, black);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.bindTexture(gl.TEXTURE_2D, null);
  const clip = core.GL.getNewId(core.GL.textures);
  core.GL.textures[clip] = texture;
  texture.name = clip;

  const h = core._sz_new();
  if (!core._sz_init_gl(h, width, height)) throw new Error(`InitGL failed ${JSON.stringify(shaderCheck)}`);
  core._sz_set_clock_scale_for_test(h, 1.0);

  // applySetting, as sztest's: by display name; an option by its element's name.
  const names = {};
  for (let i = 0; i < core._sz_param_count(h); i += 1) names[core.UTF8ToString(core._sz_param_name(h, i))] = i;
  for (const assignment of q.getAll('set')) {
    const at = assignment.indexOf('=');
    const name = assignment.slice(0, at);
    const value = assignment.slice(at + 1);
    const index = names[name];
    if (index === undefined) throw new Error(`no parameter called '${name}'`);
    let done = false;
    if (core._sz_param_type(h, index) === FF_TYPE_OPTION) {
      for (let e = 0; e < core._sz_param_element_count(h, index); e += 1) {
        if (core.UTF8ToString(core._sz_param_element_name(h, index, e)) === value) {
          core._sz_set_float(h, index, e);
          done = true;
        }
      }
    }
    if (!done) {
      const v = Number(value);
      if (value === '' || !Number.isFinite(v)) throw new Error(`'${value}' is not a number or an option of ${name}`);
      core._sz_set_float(h, index, Math.fround(v));
    }
  }

  for (let f = 0; f < frames; f += 1) {
    const seconds = f / fps;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!core._sz_process(h, seconds, Math.fround(bpm), Math.fround(((seconds * bpm) / 240) % 1), clip, width, height)) {
      throw new Error(`ProcessOpenGL failed at frame ${f}`);
    }
  }

  // Read back bottom row first, as glReadPixels gives it.
  const pixels = new Uint8Array(width * height * 4);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
  const state = {};
  ['pulses', 'fired', 'terms', 'dropped', 'shocks', 'air'].forEach((k, i) => { state[k] = core._sz_state(h, i); });
  return {
    width, height, frames, state, shaderCheck,
    renderer: gl.getParameter(gl.RENDERER),
    rgba: base64(pixels),
  };
}

run().then(
  (result) => { window.crosscheck = result; },
  (error) => { window.crosscheck = { error: String(error && error.stack || error) }; },
);
