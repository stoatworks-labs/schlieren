# schlieren

Pressure waves on the beat, seen through a schlieren camera, for Resolume
Arena/Avenue, as an FFGL effect. A loudspeaker fires a spherical sound pulse on
every beat; the clip is seen through that air. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. ID `SZ01`, display name
`SW Schlieren`.

Read `AGENTS.md` before touching the steepening (the hull), the shell's
simplification, the projection table's mapping or the composite's stencils.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/sztest --out /tmp/frame.png` (the defaults at 120 BPM, 2.5 s in)
- List parameters: `./build/sztest --list`
- Set anything by name: `./build/sztest --set "Optics=Shadowgraph" --set "Grey Field=1"`
- Press something later: `./build/sztest --at 100 "Fire=1"`; music in the Audio buffer: `--kicks`
- Film: `./build/sztest --film 600 --size 1280x720 --script cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/sztest --pipe --size WxH | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (a few minutes: fresh universal build, every
  check at 1280x720 and 320x180, again on Apple's software renderer, the
  negative controls, 16 mutants, the sweep, lipo, plist, ad-hoc signature,
  `oxbow probe` and `selftest`, the bench)
- **The air** (no GL): `--air` (the constants from the ideal gas and Edlén),
  `--steepen` (shocks: the closed-form top hat and N-wave, Godunov, s*),
  `--beat` (a pulse leaves on the beat, placed inside its frame).
- **The beam**: `--abel` (the table against a brute-force line of sight),
  `--field` (the pressure texture, every image summed), `--speed` (a + c t),
  `--spreading` (R^-1/2 in projection).
- **The optics**: `--knife`, `--colour`, `--shadow`, `--background`; `--walls`.
- **The rest**: `--audio`, `--resize`, `--state`, `--cues`, `--names`.
- The checks can fail: `--negative` (16 wrong models), `tools/mutate.sh` (16 mutants).
- No dead controls: `python3 tools/sweep.py` (21 swept, all live; the 22nd
  control, Audio, is driven through Threshold and Accent; `--bare` lists the four
  that need a context).
- Render cost: `./build/sztest --bench`.

## Notes
- **Two clocks.** Beats arrive on the host's, pulses live on the air's (host ÷
  Slow Motion). A pulse is stamped with the air time it left the cone; a beat is
  placed where it fell INSIDE its frame.
- **The shells are CPU, the light is GPU.** `Air::Build` steepens each pulse
  (cached until its age moves 0.2%), lays it out in radius, simplifies it to
  ≤ 256 vertices and picks the images worth sending (≤ 128 terms). The GPU
  does the Abel transform per pulse into a 2048-texel table, sums the terms per
  pixel, then the optics.
- **The table is uniform in q = sqrt( R − rho )**, not in rho: a shock front's
  projection goes as sqrt( R − rho ), which is then a straight line in the table.
  Read by `texelFetch` and a hand lerp, never the sampler.
- **The deflection is a central difference over two pixels** of the field
  pass's P, which carries a one-pixel margin so the frame's edge is central too.
  At Distance 0 that is the texels themselves, exactly.
- **No trig in the GLSL.** The knife's direction comes from the CPU; the colour
  wheel is three dot products; `log1p` is a series below 1 (GLSL promises `log`
  only 2^-21 absolute near 1).
- All host parameters are 0..1 and mapped in `Controls.cpp`; options hold the
  element index; Fire and Clear act on the rising edge.
- GLSL reserved words (`patch sample input output filter common active half
  layout flat packed`, `noise1`–`noise4`) must not be identifiers; `verify.sh`
  greps for them.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin.
- `schlieren_core` is an OBJECT library: the plugin registers itself from a
  file-scope constructor nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- Public at github.com/stoatworks-labs/schlieren and registered on the website
  (https://stoatworks-labs.com/software/schlieren/, the guide at `.../guide/`).
  v0.1.0 (2026-10-14) is the first release.
- Generated, do not edit: `source/StoatworksAbout*.h` (stoatworks-backend's
  `sync-about.py`: four buttons, User guide, Project page, Source on GitHub,
  Support the work), `ATTRIBUTIONS.md`, `.github/FUNDING.yml` and the issue
  forms (the backend's sync scripts), and the README's downloads block
  (`gen-downloads.py`, after the tag). Edit the sources in the backend.

## Not done yet
- Never loaded into Resolume on macOS. No host has played it any music.
- Windows: gated in Arena 7.27.1 on win-lab (llvmpipe, no sound): 9 passed, audio
  skipped; 8 controls live (9 in a second run), the other ring controls inconclusive (the air never
  stands still), none dead; plugin-bench `arena/expect/schlieren.json`.
- No browser demo, no OpenFX port, no video, no presets. The user guide is
  `docs/USER-GUIDE.md`.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/schlieren/schlieren.YYYY-MM-DD.log
    %LOCALAPPDATA%\schlieren\logs\schlieren.YYYY-MM-DD.log

`SCHLIEREN_LOG_DIR` overrides the folder.
