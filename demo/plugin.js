/**
 * Schlieren — browser demo.
 *
 * Pressure waves on the beat, seen through a schlieren camera (SW Schlieren,
 * SZ01, an effect). The one idea, from AGENTS.md: a loudspeaker fires a pulse
 * into the air on every beat, and you watch the clip through that air.
 * Nothing is drawn. The speaker's pulse, its 1/r spreading, the steepening of
 * a loud one into shocks, the beam's line-of-sight integral and the knife
 * edge, colour wheel, shadowgraph screen or distant backdrop that turn a few
 * arcseconds of bent light into a picture are the plugin's physics, in real
 * units.
 *
 * ---------------------------------------------------------------------------
 * What runs here, and what does not
 * ---------------------------------------------------------------------------
 *
 * **This page does not port the plugin. It runs it.** `schlieren-core.wasm` is
 * the plugin's own C++ compiled UNMODIFIED with emscripten
 * (demo/tools/build-wasm.sh): `source/Schlieren.cpp` — the plugin class, with
 * its constructor and declarations, its clock and seconds-or-milliseconds
 * vote, its beat, its audio, its presses, every pass and every uniform — and
 * everything it calls: Air (the pulses in flight, each beat placed inside its
 * frame, what the GPU is sent), Acoustics (the three waveforms, the
 * steepening's convex hull, the shells, the walls' images), Audio, Controls,
 * Shaders, PassBuffer and Diag, with the FFGL SDK's CFFGLPlugin, its parameter
 * bookkeeping, FFGLShader, FFGLScreenQuad, FFGLFBO and the scoped bindings.
 * Its GL calls go to this page's WebGL2 context through emscripten's GL
 * library. The air keeps its state the way the plugin does — a list of pulses
 * on the CPU, each stamped with the air time it left the cone; the GPU passes
 * hold none from frame to frame — because it is the plugin's code doing it.
 *
 * What is the page's and not the plugin's:
 *
 *   - **The host.** demo/wasm/glue.cpp constructs the plugin, reads its
 *     parameter declarations back through the SDK's host getters — the panel
 *     below is built from them, not from a list typed here — and forwards
 *     SetFloatParameter and, every frame, SetTime, SetBeatInfo and
 *     ProcessOpenGL, in the order tools/sztest's Rig makes them. (The one text
 *     parameter, the About line, is display only and never sent.)
 *   - **The transport.** A browser has no Resolume transport, so the page
 *     keeps a BPM clock: the Host BPM field under the picture (120 at first,
 *     the SDK's default and sztest's), and a bar phase advanced from the
 *     page's own seconds, handed to SetBeatInfo every frame as Resolume hands
 *     over its own. Simulated, and said so on the page.
 *   - **No audio.** Audio is an FFT buffer Resolume fills every frame; nothing
 *     here writes it, so the plugin's onset analyser hears silence: Fire On
 *     Onset never fires, and Accent leaves every pulse at Loudness, which is
 *     what the plugin does in Resolume with nothing routed. Fire is a button.
 *   - **Five GL entry points** (demo/wasm/gl_shim.cpp). glShaderSource: the
 *     plugin hands GL desktop GLSL 4.10; `shaderSource()` below refuses any
 *     text that is not byte for byte one of this page's assemblies of its copy
 *     (shaders.js, held to source/Shaders.cpp, Assemble() and InitGL by
 *     demo/tools/check_shaders.py), then applies the kit's `port()` — the
 *     version line and the ES precision defaults, nothing else.
 *     glEnable / glDisable / glIsEnabled: GL_PROGRAM_POINT_SIZE, which the
 *     plugin saves and restores every frame, does not exist in WebGL2.
 *     glTexImage2D: the FFGL SDK allocates the R32F pass buffers with a client
 *     format of GL_RGBA and no data, which desktop GL takes and WebGL2 refuses;
 *     with no data the shim passes ES's own pair, GL_RED.
 *   - **The panel's read-outs.** The number beside a slider is Controls.cpp's
 *     conversion, compiled in (glue.cpp's sz_convert pairs each control with
 *     the function ProcessOpenGL applies to it); the units printed after it,
 *     and every tooltip, are this page's words.
 *   - **The presets** are this page's, taken from the user guide's "Start
 *     here" walk-through and converted to the host's 0..1 by the plugin's own
 *     ParamFrom functions where it has one.
 *   - **The About block's links.** The plugin opens them with system(); the
 *     page opens the same addresses itself, read from the plugin's own table.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Kit types for FFGL types.** The kit has no event, position or buffer
 * control. Fire, Clear and the About buttons (FF_TYPE_EVENT) are buttons: Fire
 * and Clear send the press and hold it through one frame, releasing it on the
 * next, as Resolume sends an event; an About button sends the press and the
 * release at once, as zoetrope's page does. Speaker X and Speaker Y
 * (FF_TYPE_XPOS / FF_TYPE_YPOS) are 0..1 sliders, which is what they are to
 * the plugin. Audio (FF_TYPE_BUFFER, FF_USAGE_FFT) is shown where the plugin
 * declares it, read-only, saying there is no audio here — rather than left
 * out, because Fire On's Onset, Threshold and Accent refer to it. The About
 * line (FF_TYPE_TEXT) is shown read-only.
 *
 * **The transport is a bar phase, not only a BPM** (zoetrope's reasoning).
 * Air.cpp has a fallback clock of its own at the host's BPM for a host that
 * never moves the bar phase, but Resolume sends one every frame and the
 * plugin's beat path reads it, so the page sends one, as sztest's Rig does:
 * the plugin then takes the path it takes in Resolume, placing each beat
 * inside the frame where the phase crossed it. The phase is accumulated, so
 * changing the BPM changes how fast it runs from then on and never jumps it;
 * Restart puts it back to the top of the bar.
 */

import { mountDemo } from './vendor/demo.js';
import { port, GLError } from './vendor/gl.js';
import * as S from './shaders.js';
import createSchlieren from './schlieren-core.js';

//===========================================================================
// FFGL.h's parameter types, by number: what GetParamType returns.
//===========================================================================

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_XPOS = 5;
const FF_TYPE_YPOS = 6;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_BUFFER = 12;
const FF_TYPE_TEXT = 100;

//===========================================================================
// The GLSL: the page's checked copy, assembled stage by stage in the order
// the plugin joins the pieces (shaders.js's ASSEMBLY, read out of
// Schlieren.cpp's InitGL and Shaders.cpp's Assemble() by check_shaders.py).
//===========================================================================

const ASSEMBLED = Object.fromEntries(
  Object.entries(S.ASSEMBLY).map(([stage, pieces]) => [stage, pieces.map((piece) => S[piece]).join('')]),
);
const FRAGMENT_STAGES = Object.keys(ASSEMBLED).filter((stage) => stage !== 'vertex');
const PAGE_SHADERS = {
  vertex: [ASSEMBLED.vertex],
  fragment: FRAGMENT_STAGES.map((stage) => ASSEMBLED[stage]),
};

/** Which stages the compiled plugin handed over, and whether each was ours. */
const shaderCheck = { matched: [], refused: null };

/**
 * Called by gl_shim.cpp's glShaderSource with the text the compiled plugin
 * handed to GL. Anything but one of this page's own assemblies is refused (an
 * empty source, so the plugin's InitGL fails and logs which pass) rather than
 * compiled.
 */
function shaderSource(stage, text) {
  const ours = PAGE_SHADERS[stage].indexOf(text);
  if (ours < 0) {
    shaderCheck.refused = { stage, length: text.length };
    return '';
  }
  shaderCheck.matched.push(stage === 'vertex' ? 'vertex' : FRAGMENT_STAGES[ours]);
  return port(text, { stage });
}

//===========================================================================
// The module.
//===========================================================================

const printed = [];
let core;
try {
  core = await createSchlieren({
    schlierenShaderSource: shaderSource,
    print: (line) => { printed.push(line); console.log(line); },
    printErr: (line) => { printed.push(line); console.warn(line); },
  });
} catch (error) {
  const root = document.querySelector('#demo');
  root.textContent = `The demo could not start: this browser would not run its WebAssembly (${error.message}). The plugin, its source and its downloads are at https://github.com/stoatworks-labs/schlieren.`;
  throw error;
}

const text = (pointer) => core.UTF8ToString(pointer);

/** A C string for one call: allocated, handed over, freed. */
function withCString(value, fn) {
  const pointer = core.stringToNewUTF8(value);
  try {
    return fn(pointer);
  } finally {
    core._free(pointer);
  }
}

// Diag.cpp opens its log in InitGL, so its directory must exist in the page's
// memory before the first instance is initialised.
core._sz_prepare_log();

//===========================================================================
// The declarations, read from the plugin's constructor through the SDK.
//===========================================================================

function declarations() {
  const h = core._sz_new();
  const list = [];
  const n = core._sz_param_count(h);
  for (let i = 0; i < n; i += 1) {
    const type = core._sz_param_type(h, i);
    const d = { index: i, name: text(core._sz_param_name(h, i)), type, group: text(core._sz_param_group(h, i)) };
    if (type === FF_TYPE_TEXT) d.default = text(core._sz_param_default_text(h, i));
    else d.default = core._sz_param_default(h, i);
    if (type === FF_TYPE_OPTION) {
      d.elements = [];
      d.values = [];
      for (let e = 0; e < core._sz_param_element_count(h, i); e += 1) {
        d.elements.push(text(core._sz_param_element_name(h, i, e)));
        d.values.push(core._sz_param_element_value(h, i, e));
      }
    }
    if (type === FF_TYPE_BUFFER) {
      d.bins = core._sz_param_element_count(h, i);
      d.usage = core._sz_param_usage(h, i);
    }
    list.push(d);
  }
  const maxInputs = core._sz_max_inputs(h);
  core._sz_delete(h);
  return { list, maxInputs };
}

const DECLARED = declarations();

// The About block: its first index, and the address each button opens, from
// the plugin's own table (StoatworksAboutLinks.h, compiled in). A label that
// does not match the declaration is a page bug, not something to paper over.
const ABOUT_FIRST = core._sz_about_first();
const ABOUT_LINKS = new Map();
for (const d of DECLARED.list) {
  if (d.index <= ABOUT_FIRST || d.type !== FF_TYPE_EVENT) continue;
  const button = d.index - ABOUT_FIRST - 1;
  const label = text(core._sz_about_button_label(button));
  if (label !== d.name) throw new Error(`About button ${button} is "${label}" in the plugin's table and "${d.name}" in its declaration`);
  ABOUT_LINKS.set(d.index, text(core._sz_about_button_url(button)));
}

const indexOf = (name) => {
  const d = DECLARED.list.find((x) => x.name === name);
  if (!d) throw new Error(`the plugin declares no "${name}"`);
  return d.index;
};

//===========================================================================
// The read-outs: Controls.cpp's number, this page's unit.
//===========================================================================

const fixed = (n, d) => n.toFixed(d);
const UNITS = {
  Threshold: (x) => `sens. ${fixed(x, 2)}`,
  Accent: (x) => `${fixed(x, 1)} dB`,
  Loudness: (x) => `${fixed(x, 1)} dB`,
  Length: (x) => `${fixed(1e3 * x, x < 1e-3 ? 3 : 2)} ms`,
  'Field Width': (x) => `${fixed(x, x < 10 ? 2 : 1)} m`,
  'Slow Motion': (x) => `${fixed(x, 0)}×`,
  Reflection: (x) => fixed(x, 2),
  'Knife Angle': (x) => `${fixed(x, 0)}°`,
  Sensitivity: (x) => `${fixed(x, x < 1 ? 2 : 1)}″`,
  Distance: (x) => (x === 0 ? '0: on the air' : x < 1000 ? `${fixed(x, 0)} m` : `${fixed(x / 1000, x < 1e4 ? 2 : 1)} km`),
};

function readout(d) {
  return (v) => {
    const f = Math.fround(v);
    const x = core._sz_convert(d.index, f);
    if (Number.isNaN(x)) return fixed(v, 2);
    return (UNITS[d.name] ?? ((y) => fixed(y, 3)))(x);
  };
}

/** Tooltips: this page's words, from the README's, AGENTS.md's and the user guide's account of each control. */
const HINTS = {
  'Fire On': 'What fires a pulse. Beat, Half Beat, 2 Beats and Bar follow the transport — on this page the Host BPM clock under the picture, in Resolume the composition’s tempo — and each pulse leaves at the moment inside the frame where its beat fell. Onset fires on hits in Audio, which a browser does not have, so here it never fires. Off fires only on Fire.',
  Fire: 'A pulse now, at full Loudness, whatever Fire On says. FF_TYPE_EVENT: the button sends the press and releases it on the next frame, as Resolume sends an event.',
  Audio: 'Resolume’s audio input: an FFT buffer of 64 bins (FF_TYPE_BUFFER) the host fills every frame. A browser has no Resolume FFT, so this page never writes it: the plugin’s analyser hears silence, Onset never fires and every pulse is at Loudness.',
  Threshold: 'How hard a hit must be to count as an onset (Fire On Onset only). The number beside it is the analyser’s sensitivity, 1 − Threshold. With no audio on this page it changes nothing.',
  Accent: 'How far below Loudness the softest hit lands, 0 to 30 dB. It needs audio arriving; with none, as here, every pulse is at Loudness and this changes nothing.',
  Loudness: 'The pulse’s peak a metre from the speaker, 120 to 190 dB SPL in a straight line (165, a starting pistol at a metre, by default). Taken when a pulse fires: the rings already in the air keep theirs. Past about 170 dB the shocks are a good look rather than a prediction.',
  Pulse: 'The shape of the pressure the cone sends out. Pop: a clap, a compression then an equal rarefaction. Kick: a sine started smoothly that rings as it dies. Blast: a blast wave, arriving as a shock. Taken when a pulse fires.',
  Length: 'From the compression’s peak to the rarefaction’s trough (half the period of a Kick’s ringing), 0.05 to 20 ms geometrically; 1 ms is 34 cm of air. Shorter steepens into a shock sooner. Taken when a pulse fires.',
  'Speaker X': 'Where the speaker is, across the frame from the left (FF_TYPE_XPOS). Each pulse leaves from where the speaker was when it fired.',
  'Speaker Y': 'Where the speaker is, up the frame from the bottom edge, which is the Floor (FF_TYPE_YPOS).',
  'Field Width': 'How much air the frame shows, 0.5 to 50 m across, geometrically. The height follows the frame’s shape.',
  'Slow Motion': 'How much the camera slows the air down, 30× to 30 000× geometrically: a camera at 1 800 to 1 800 000 frames a second played back at 60. The beats are not slowed; each fires a pulse you then watch slowed.',
  Walls: 'Open: nothing reflects. Floor: the frame’s bottom edge. Box: all four edges, six reflections deep. Rigid walls, as mirror images of the speaker. Changes the rings already in the air too.',
  Reflection: 'How much of a ring’s pressure each wall sends back, 0 to 1. Nothing with Walls on Open.',
  Clear: 'Lets every pulse go at once; Fire On carries on firing new ones. FF_TYPE_EVENT: a press held through one frame.',
  Optics: 'Knife Edge: Toepler’s schlieren, light on one side of a ring and shade on the other. Colour: a colour wheel where the knife was, so the hue says which way the light bent. Shadowgraph: no filter, a screen Distance beyond the air. Background: the clip as a backdrop Distance behind the air, seen through it.',
  'Knife Angle': 'Which way the knife faces, 0 to 360°. At 0 the edge is upright and light bent to the right gets brighter. Turns the wheel on Colour; nothing on Shadowgraph or Background.',
  Sensitivity: 'The bend that doubles the light through the knife (and, bent the other way, cuts it to black), 50 arcseconds down to 0.05 geometrically. The default pulse bends light by a few arcseconds.',
  Distance: 'Exactly 0 at the bottom of the travel, then 10 m to 100 km geometrically: where the camera is focused beyond the air, the shadowgraph’s screen, or how far behind the air the backdrop stands. Kilometres are what a few arcseconds need to move light a pixel.',
  'Grey Field': 'Mixes the bare schlieren field, a flat mid-grey, into the clip before the optics. 1 is a classic grey schlieren photograph. A dark clip needs some to show rings: black doubled is still black.',
  View: 'Picture: the clip through the optics. Pressure: the pressure the beam sees, as grey, scaled to the strongest pulse in the air. Deflection: the bend, across as red and up as green, full scale at Sensitivity.',
  Mix: 'The result over the clip. At 0 the clip goes through untouched.',
  About: 'The plugin’s own About line (FF_TYPE_TEXT, display only).',
};

//===========================================================================
// Kit parameters from the declarations. The id is the name as Arena
// addresses it (lower case, spaces removed).
//===========================================================================

const KIT_TYPES = {
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_XPOS]: 'standard',
  [FF_TYPE_YPOS]: 'standard',
  [FF_TYPE_OPTION]: 'option',
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_EVENT]: 'boolean',
  [FF_TYPE_TEXT]: 'text',
  [FF_TYPE_BUFFER]: 'text',
};

/**
 * The kit reads `embed`, `size`, `clip` and `bg` from the query string itself,
 * and this page reads `bpm`; a parameter id equal to one would be set by it.
 */
const RESERVED_QUERY = new Set(['embed', 'size', 'clip', 'bg', 'bpm']);
const addressOf = (name) => {
  const address = name.toLowerCase().replace(/\s+/g, '');
  return RESERVED_QUERY.has(address) ? `param-${address}` : address;
};

function kitDefault(d) {
  switch (d.type) {
    case FF_TYPE_OPTION: {
      const i = d.values.findIndex((v) => v === d.default);
      return i >= 0 ? i : Math.round(d.default);
    }
    case FF_TYPE_BOOLEAN:
    case FF_TYPE_EVENT: return d.default > 0.5 ? 1 : 0;
    case FF_TYPE_BUFFER: return '';
    default: return d.default;
  }
}

const PARAMS = DECLARED.list.map((d) => {
  const p = { id: addressOf(d.name), name: d.name, type: KIT_TYPES[d.type], group: d.group, hint: HINTS[d.name], ff: d, default: kitDefault(d) };
  if (p.type === undefined) throw new Error(`no kit control for FFGL type ${d.type} (${d.name})`);
  if (d.type === FF_TYPE_OPTION) p.elements = d.elements;
  if (d.type === FF_TYPE_STANDARD) p.display = readout(d);
  if (d.type === FF_TYPE_BUFFER) p.placeholder = 'none: no audio in a browser';
  if (ABOUT_LINKS.has(d.index)) p.hint = `Opens ${ABOUT_LINKS.get(d.index)} — the address the plugin’s own button opens.`;
  return p;
});
const PARAM = Object.fromEntries(PARAMS.map((p) => [p.name, p]));

//===========================================================================
// The presets: the user guide's "Start here" steps, converted to the host's
// 0..1 by Controls.cpp's own inverses (sz_param_from). An option is its
// element's index. Grey Field, Speaker X and Y and Mix are the host's 0..1 in
// the plugin and have no inverse; they are given as the slider positions.
//===========================================================================

const from = (name, x) => {
  const v = core._sz_param_from(indexOf(name), x);
  if (Number.isNaN(v)) throw new Error(`no inverse for ${name}`);
  return Math.fround(v);
};
const element = (name, label) => {
  const i = PARAM[name].elements.indexOf(label);
  if (i < 0) throw new Error(`${name} has no element "${label}"`);
  return i;
};
const id = (name) => PARAM[name].id;

const PRESETS = {
  'A grey schlieren photograph (Grey Field 1)': {
    [id('Grey Field')]: 1,
  },
  'Rings and the clip together (Grey Field ⅓)': {
    [id('Grey Field')]: 1 / 3,
  },
  'Colour schlieren': {
    [id('Optics')]: element('Optics', 'Colour'),
    [id('Grey Field')]: 1 / 3,
  },
  'Shadowgraph, a screen 1 km off': {
    [id('Optics')]: element('Optics', 'Shadowgraph'),
    [id('Distance')]: from('Distance', 1000),
  },
  'Background, the clip 2 km behind the air': {
    [id('Optics')]: element('Optics', 'Background'),
    [id('Distance')]: from('Distance', 2000),
  },
  'Shocks: a Blast at 180 dB, focused on the air': {
    [id('Pulse')]: element('Pulse', 'Blast'),
    [id('Loudness')]: from('Loudness', 180),
    [id('Distance')]: 0,
    [id('Grey Field')]: 1 / 3,
  },
  'Echoes: a box, the speaker near a corner, on the half beat': {
    [id('Walls')]: element('Walls', 'Box'),
    [id('Speaker X')]: 0.2,
    [id('Speaker Y')]: 0.25,
    [id('Fire On')]: element('Fire On', 'Half Beat'),
    [id('Grey Field')]: 1 / 3,
  },
  'The pressure the beam sees': {
    [id('View')]: element('View', 'Pressure'),
  },
  'The deflection, red across and green up': {
    [id('View')]: element('View', 'Deflection'),
  },
};

const query = new URLSearchParams(window.location.search);

//===========================================================================
// The transport: the page's BPM clock, standing in for Resolume's.
//===========================================================================

const BPM_LOW = 20;
const BPM_HIGH = 300;
const queryBpm = Number(query.get('bpm'));
const transport = {
  bpm: Number.isFinite(queryBpm) && queryBpm >= BPM_LOW && queryBpm <= BPM_HIGH ? queryBpm : 120,
  bars: 0,///< bars since the top, accumulated from the page's seconds
  last: null,///< the page's seconds at the last frame
  /** The bar phase at the page's `seconds`: advanced at the BPM, four beats to the bar. */
  advance(seconds) {
    if (this.last === null || seconds < this.last) this.bars = 0;// a start, or Restart
    else this.bars += ((seconds - this.last) * this.bpm) / 240;
    this.last = seconds;
    return this.bars - Math.floor(this.bars);
  },
};

//===========================================================================
// The renderer: one plugin instance, driven as a host drives it.
//===========================================================================

/** The value the host would send for a kit control, or null for none. */
function hostValue(p, v) {
  const d = p.ff;
  if (d.index === ABOUT_FIRST) return null;// display only
  switch (d.type) {
    case FF_TYPE_OPTION: return Math.fround(d.values[Math.round(v)] ?? 0);
    case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
    case FF_TYPE_EVENT: return null;
    case FF_TYPE_BUFFER: return null;// the page has no audio to write into it
    case FF_TYPE_TEXT: return String(v);
    default: return Math.fround(v);
  }
}

function createRenderer(gl) {
  // emscripten's GL library, pointed at the kit's context. Nothing is turned
  // on behind the kit's back: no extensions are enabled on its behalf (the
  // kit has already asked for EXT_color_buffer_float, which the plugin's two
  // R32F passes render into, and stopped the page if it is missing).
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);

  let instance = 0;
  let pushed = new Map();
  let releases = [];
  let clipName = 0;
  let clipTexture = null;
  let frames = 0;
  let barPhase = 0;

  /** A kit texture under a GL name the plugin can bind, in emscripten's tables. */
  function glName(texture) {
    if (texture === clipTexture) return clipName;
    if (clipName) core.GL.textures[clipName] = null;
    clipName = core.GL.getNewId(core.GL.textures);
    core.GL.textures[clipName] = texture;
    texture.name = clipName;
    clipTexture = texture;
    return clipName;
  }

  function start(width, height) {
    instance = core._sz_new();
    pushed = new Map();
    releases = [];
    frames = 0;
    if (!core._sz_init_gl(instance, width, height)) {
      const why = shaderCheck.refused
        ? `the ${shaderCheck.refused.stage} shader text the compiled plugin handed to GL (${shaderCheck.refused.length} characters) is not this page’s checked copy of source/Shaders.cpp, so it was not compiled`
        : 'the plugin’s InitGL failed — its log line under the picture says which pass';
      throw new GLError(`SW Schlieren could not start: ${why}.`);
    }
  }

  function push(params) {
    for (const p of PARAMS) {
      const value = hostValue(p, params.get(p.id));
      if (value === null || pushed.get(p.id) === value) continue;
      pushed.set(p.id, value);
      if (typeof value === 'string') withCString(value, (s) => core._sz_set_text(instance, p.ff.index, s));
      else core._sz_set_float(instance, p.ff.index, value);
    }
  }

  return {
    render({ input, params, width, height, time }) {
      core.GL.makeContextCurrent(handle);
      if (!instance) start(width, height);
      push(params);
      barPhase = transport.advance(time);
      const clip = glName(input.texture);
      if (!core._sz_process(instance, time, Math.fround(transport.bpm), Math.fround(barPhase), clip, input.width, input.height)) {
        throw new GLError('The plugin’s ProcessOpenGL failed.');
      }
      // A press is held through exactly one frame: Resolume sends 1 on one
      // frame and 0 on the next.
      for (const index of releases) core._sz_set_float(instance, index, 0);
      releases = [];
      // For anything checking the page from outside: frames this instance drew.
      frames += 1;
      gl.canvas.dataset.frames = String(frames);
    },
    /** Fire or Clear: 1 now, 0 after the next frame. */
    press(index) {
      if (!instance) return false;
      core._sz_set_float(instance, index, 1);
      releases.push(index);
      return true;
    },
    /** An About button: the press and the release at once (the plugin acts on the press). */
    click(index) {
      if (!instance) return;
      core._sz_set_float(instance, index, 1);
      core._sz_set_float(instance, index, 0);
    },
    get barPhase() { return barPhase; },
    /** What the air is doing, through the harness's accessors. */
    state() {
      if (!instance) return null;
      const s = (k) => core._sz_state(instance, k);
      return {
        pulses: s(0), fired: s(1), terms: s(2), dropped: s(3), shocks: s(4),
        air: s(5), width: s(6), height: s(7), newest: s(8), frames: s(9),
      };
    },
    log() {
      try {
        return core.FS.readFile(text(core._sz_log_path()), { encoding: 'utf8' });
      } catch {
        return '';
      }
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

let renderer = null;

const mounted = mountDemo({
  name: 'Schlieren',
  pluginId: 'SZ01',
  kind: 'effect',
  tagline:
    'Pressure waves on the beat, seen through a schlieren camera, for Resolume. A loudspeaker fires a pulse of sound on every beat; the air carries it away as a spherical wave, fading as 1/r and steepening into shocks when it is loud; and a knife edge, a colour wheel, a shadowgraph screen or a backdrop kilometres away turns the few arcseconds it bends the light into light and shade on your clip. Nothing in the picture is drawn.',
  repo: 'https://github.com/stoatworks-labs/schlieren',
  page: 'https://stoatworks-labs.com/software/schlieren/',

  blurb:
    'It is the plugin’s own C++ — the plugin class, the air, the acoustics and the parameter table, with the FFGL SDK classes it uses — compiled unmodified to WebAssembly, drawing with its own GLSL in WebGL2. The host is this page: its tempo is a BPM clock the page keeps, there is no audio, the clock is the browser’s and the clip is generated in the page.',

  sources: ['bars', 'grid', 'scene', 'detail', 'ramp', 'spot', 'alpha'],

  params: PARAMS,
  presets: PRESETS,
  needFloat: true,
  showBackdrop: true,

  differences: [
    'This page runs the plugin rather than a port of it. schlieren-core.wasm is source/Schlieren.cpp — the plugin class: its constructor and declarations, its clock and unit vote, its beat and audio handling, its presses, every pass and every uniform — and everything it calls (Air, Acoustics, Audio, Controls, Shaders, PassBuffer, Diag), with the FFGL SDK’s CFFGLPlugin, parameter bookkeeping, FFGLShader, FFGLScreenQuad, FFGLFBO and scoped bindings, all compiled unmodified by emscripten (demo/tools/build-wasm.sh). So the pulses, the steepening’s convex hull, the shells, the walls’ images and which of them reach the GPU are the plugin’s own code, keeping its own state between frames. Of source/ only PluginEntry.cpp, the registration and build stamp, is left out, and with it the SDK’s plugMain (FFGL.cpp) and CFFGLPluginInfo: the page constructs the plugin itself, as the plugin’s harness does. The panel is read back from the plugin’s own declarations through the SDK’s host getters, and the numbers beside the sliders are Controls.cpp’s conversions, compiled in; their units, every tooltip and the presets are this page’s.',
    'The GLSL is the plugin’s: demo/shaders.js is spliced from source/Shaders.cpp, with the order Assemble() joins the pieces in, and the repository’s verify script fails if a character differs or if the WebAssembly was built from sources that have since changed. At start-up the page also requires each text the compiled plugin hands to glShaderSource to equal its own assembly of that copy, and refuses to compile anything else. The kit’s port() then changes the version line and adds the ES precision defaults (highp throughout). GL is emscripten’s WebGL2 library on this page’s context, not a GL 4.1 driver, and five entry points are the page’s (demo/wasm/gl_shim.cpp): glShaderSource, for that check and port; glEnable, glDisable and glIsEnabled, because GL_PROGRAM_POINT_SIZE — state the plugin saves and restores each frame, as a plugin in Resolume must — does not exist in WebGL2 (it reads as off here; the plugin draws no points); and glTexImage2D, because the FFGL SDK allocates the two R32F pass buffers with a client format of GL_RGBA and no data, which desktop GL accepts and WebGL2 refuses, so with no data the page passes the format WebGL2 pairs with R32F, GL_RED. Nothing about what is allocated changes.',
    'The pressure on the frame and the projection table are 32-bit float textures the plugin renders into, which WebGL2 allows only with EXT_color_buffer_float. Without it the page stops rather than run on an 8-bit stand-in, which would quantise the pressure and produce a plausible wrong picture.',
    'The tempo is simulated. Resolume hands the plugin its BPM and bar phase every frame (SetBeatInfo); a browser has no transport, so this page keeps a BPM clock of its own — the Host BPM field under the picture, 120 at first — and hands the plugin that BPM and a bar phase advanced from the page’s own seconds, as the plugin’s harness does. The plugin cannot tell it from Resolume’s: Beat, Half Beat, 2 Beats and Bar fire on its boundaries, each pulse placed inside the frame where its beat fell. It is not the composition’s tempo and not anything you can hear: there is no tap tempo, no nudge and no music it follows. Pause stops it with the clock, a changed BPM runs from then on without a jump, and Restart puts it back to the top of a bar.',
    'There is no audio. The plugin declares Audio, an FFT buffer of 64 bins that Resolume fills every frame; it is shown on the panel where the plugin declares it, but nothing on this page writes it, so the plugin’s onset analyser hears silence. Fire On Onset therefore never fires, Threshold changes nothing, and Accent changes nothing either: with no signal the plugin fires every pulse at Loudness, which is exactly what it does in Resolume with nothing routed. Fire and Clear are buttons that send the press and release it on the next frame, as Resolume sends an event.',
    'The clock is the browser’s: SetTime is handed seconds summed from the display’s frames (a frame capped at 0.1 s), and the plugin’s own vote settles on seconds within four frames, running on its wall clock (here performance.now()) until it does. The rings move at the speed of sound slowed by Slow Motion against that clock, so a slow or uneven frame rate makes them step rather than glide; their radii are still right for the moment each frame is drawn. Pause stops the host clock and the plugin sees no time pass: the rings hold still. Step advances 1/60 s. Restart is a backward jump, which the plugin takes as no time passing: the air carries on where it was, and Clear is how to empty it.',
    'The About block (a text line and four buttons) is the plugin’s, and is on the panel because Resolume shows it. Pressing a button sends the press to the plugin, as Resolume would, and the plugin’s handler opens the link by asking the operating system (std::system), which a browser does not have — so there nothing happens, and the page opens the same address itself, read from the plugin’s own About table (StoatworksAboutLinks.h, compiled in). The plugin’s log (Diag.cpp) is written to this page’s memory, and its last lines are shown under the picture.',
    'The clip is the kit’s generated one, premultiplied, or your own image or video, handed to the plugin unpadded (Width = HardwareWidth); a host may pad it. The output is the page’s 8-bit canvas. With Grey Field up, the output’s alpha goes to 1 as the plugin’s does, so the backdrop chosen under the picture shows only through a clip’s own alpha at low Grey Field.',
    'Nothing on this page is measured while you watch it. Once, on 2026-10-28, demo/tools/crosscheck in the repository rendered five fixed schedules (150 frames each at 960 × 540: the defaults, Pressure, Colour, a shadowgraph, and a Blast at 180 dB in a box) through this page’s WebAssembly and shaders in headless Chrome and through sztest, the plugin’s harness, in a GL 4.1 context. Against an sztest built without fused multiply-adds, as WebAssembly has none, every frame agreed to within one level in 255 on an Apple Silicon Mac’s GPU and two on SwiftShader, with the same pulses and the same images in flight. Against the plugin’s default Apple Silicon build, which fuses them, a Blast’s shock fronts can land a pixel apart and about 3% of that frame differs — exactly as much as the plugin’s Apple Silicon build differs from its own build without them. Your browser and GPU are not the ones measured. The render cost is not measured either: on that Mac’s GPU the page held its display’s 60 frames a second with a box of walls on the half beat at 960 × 540 (112 images per pixel), and on SwiftShader it manages a few frames a second; the projection table sums up to 256 segments per texel for each of 24 pulses and every pixel up to 128 images, so a slow GPU at 1920 × 1080 will step.',
    'The plugin itself has never been loaded into Resolume on macOS, and no host has played it any music (its README’s Status says so; on Windows it has been gated in Arena 7.27.1 with software rendering and no sound). Its harness, sztest, drives the same classes headlessly and measures each claim out of the picture — a pulse leaving on its beat, the front at a + c t to the pixel, the projection against a brute-force line of sight, the shocks against Godunov and closed forms, every optic’s law per pixel, a rigid wall mirroring the field — and that harness, not this page, is the reason to believe it.',
  ],

  createRenderer: (gl) => {
    renderer = createRenderer(gl);
    return renderer;
  },
});

//===========================================================================
// The controls the kit has no type for, the About block, the transport's
// BPM, and the plugin's state and log.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule.
//===========================================================================

if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';

  // For anything checking the page from outside (a headless browser reading
  // the plugin's own accessors): the kit's params and state, and the renderer.
  window.schlierenDemo = { params, state: mounted.state, redraw: mounted.redraw, transport, shaderCheck, get renderer() { return renderer; } };

  if (!embed) {
    const button = (label, title) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = label;
      if (title) b.title = title;
      return b;
    };
    const rowOf = (p) => document.getElementById(`p-${p.id}`)?.closest('.prow')
      ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name);

    for (const p of PARAMS) {
      const row = rowOf(p);
      if (!row) continue;

      // FF_TYPE_EVENT: Fire and Clear are a press held through one frame. An
      // About button sends its press and opens the address the plugin's own
      // press would have opened.
      if (p.ff.type === FF_TYPE_EVENT) {
        const toggle = row.querySelector('.prow__toggle');
        const url = ABOUT_LINKS.get(p.ff.index);
        let press;
        if (url) {
          press = document.createElement('a');
          press.href = url;
          press.target = '_blank';
          press.rel = 'noopener';
          press.textContent = 'Open';
          press.title = p.hint;
          press.addEventListener('click', () => renderer?.click(p.ff.index));
        } else {
          press = button(p.name, p.hint);
          press.addEventListener('click', () => {
            if (renderer?.press(p.ff.index)) mounted.redraw();
          });
        }
        press.className = 'prow__toggle';
        press.dataset.press = p.id;
        press.style.textAlign = 'center';
        press.style.textDecoration = 'none';
        toggle?.replaceWith(press);
        continue;
      }

      const field = row.querySelector('.prow__text');
      if (!field) continue;

      // The About line and Audio: the plugin's, display only.
      if (p.ff.index === ABOUT_FIRST || p.ff.type === FF_TYPE_BUFFER) {
        field.readOnly = true;
        field.tabIndex = -1;
        if (p.ff.type === FF_TYPE_BUFFER) field.title = p.hint;
      }
    }

    //-----------------------------------------------------------------------
    // The transport's BPM: what Resolume's would hand the plugin.
    //-----------------------------------------------------------------------
    const bar = document.querySelector('.transport');
    if (bar) {
      const label = document.createElement('label');
      label.className = 'transport__field';
      const span = document.createElement('span');
      span.className = 'transport__label';
      span.textContent = 'Host BPM';
      const bpm = document.createElement('input');
      bpm.type = 'number';
      bpm.className = 'prow__text';
      bpm.min = String(BPM_LOW);
      bpm.max = String(BPM_HIGH);
      bpm.step = '1';
      bpm.value = String(transport.bpm);
      bpm.inputMode = 'numeric';
      bpm.style.width = '6em';
      bpm.style.flex = '0 0 auto';
      bpm.id = 'schlieren-bpm';
      label.title = 'A simulated transport: the BPM, and a bar phase from this page’s clock, handed to the plugin every frame (SetBeatInfo) as Resolume hands over the composition’s. Fire On’s Beat, Half Beat, 2 Beats and Bar fire on it. Not anything you can hear.';
      bpm.addEventListener('change', () => {
        const v = Number(bpm.value);
        if (Number.isFinite(v) && v >= BPM_LOW && v <= BPM_HIGH) {
          transport.bpm = v;
          if (!mounted.state.playing) mounted.redraw();
        } else {
          bpm.value = String(transport.bpm);
        }
      });
      label.append(span, bpm);
      bar.append(label);
    }

    //-----------------------------------------------------------------------
    // What the air is doing (the harness's accessors), and the plugin's own
    // log (Diag.cpp). Both report; neither measures.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const status = document.createElement('p');
      status.className = 'stage__status';
      status.id = 'schlieren-state';
      status.setAttribute('aria-live', 'off');
      const log = document.createElement('p');
      log.className = 'stage__status';
      log.id = 'schlieren-log';
      log.setAttribute('aria-live', 'off');
      log.style.fontFamily = 'ui-monospace, SFMono-Regular, Menlo, monospace';
      log.style.fontSize = '12px';
      log.style.whiteSpace = 'pre-wrap';
      stage.append(status, log);
      let lastLog = '';
      setInterval(() => {
        const s = renderer?.state();
        if (s && Number.isFinite(s.pulses)) {
          const bars = transport.bars;
          const beat = Math.floor((bars - Math.floor(bars)) * 4) + 1;
          const dropped = s.dropped > 0 ? ` (${s.dropped} weaker ones dropped over 128)` : '';
          const newest = Number.isFinite(s.newest) ? `, the newest at ${fixed(s.newest, 1)} dB` : '';
          const shocks = s.shocks === 1 ? '1 shock' : `${s.shocks} shocks`;
          status.textContent =
            `The plugin reports: ${s.pulses} pulse${s.pulses === 1 ? '' : 's'} in the air${newest} · ${s.fired} fired · `
            + `${s.terms} image${s.terms === 1 ? '' : 's'} sent to the GPU${dropped} · ${shocks} in flight · `
            + `the air’s clock at ${fixed(1e3 * s.air, 2)} ms · a frame ${fixed(s.width, 2)} × ${fixed(s.height, 2)} m. `
            + `The page’s transport: ${transport.bpm} BPM, bar ${Math.floor(bars) + 1} beat ${beat}.`;
        }
        const all = renderer?.log() ?? '';
        if (all === lastLog) return;
        lastLog = all;
        const lines = all.trim().split('\n').filter(Boolean).map((l) => l.replace(/^\S+ (INFO |WARN |ERROR) schlieren: /, '$1 '));
        log.textContent = lines.length
          ? `The plugin’s own log (Diag.cpp, in this page’s memory):\n${lines.slice(-2).join('\n')}`
          : '';
      }, 250);
    }
  }
}
