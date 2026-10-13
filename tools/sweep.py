"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Every render is 150 frames (2.5 s at
60 fps) at 120 BPM, so a few rings are in flight. The Audio controls act only
on music, so they are swept with `--kicks` (the harness's kick every half
second in the Audio buffer); Reflection acts only with walls; Clear is a
press, swept by pressing it late (`--at`) and comparing with not pressing it;
Grey Field acts on the projection optics; Knife Angle on a knife or a colour
wheel; Walls and Reflection are swept with the speaker near the floor, where
the rings reach a wall inside the 2.5 s. An option parameter reads back 0..1 whatever its count (the fleet's
trap), so options are set here by element index. A control whose ends differ
by less than `--floor` (mean 8-bit difference per channel) is reported as
barely alive: vectrix's lesson.

Every render is 320x180 (CI's raster).

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

# A setting is "Name=Value" (passed as --set) or a list of raw arguments.
MUSIC = [["--kicks"]]

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Fire On": (["Fire On=0"], ["Fire On=5"], []),
    "Fire": ([], [["--at", "100", "Fire=1"]], ["Fire On=5"]),
    "Threshold": (["Threshold=0"], ["Threshold=1"], MUSIC + ["Fire On=4"]),
    "Accent": (["Accent=0"], ["Accent=1"], MUSIC + ["Fire On=4"]),
    "Loudness": (["Loudness=0.4"], ["Loudness=0.8"], []),
    "Pulse": (["Pulse=0"], ["Pulse=2"], []),
    "Length": (["Length=0.2"], ["Length=0.6"], []),
    "Speaker X": (["Speaker X=0.3"], ["Speaker X=0.7"], []),
    "Speaker Y": (["Speaker Y=0.3"], ["Speaker Y=0.7"], []),
    "Field Width": (["Field Width=0.4"], ["Field Width=0.7"], []),
    "Slow Motion": (["Slow Motion=0.3"], ["Slow Motion=0.5"], []),
    "Walls": (["Walls=0"], ["Walls=2"], ["Speaker Y=0.15"]),
    "Reflection": (["Reflection=0"], ["Reflection=1"], ["Walls=2", "Speaker Y=0.15"]),
    "Clear": ([], [["--at", "140", "Clear=1"]], []),
    "Optics": (["Optics=0"], ["Optics=3"], []),
    "Knife Angle": (["Knife Angle=0"], ["Knife Angle=0.25"], []),
    "Sensitivity": (["Sensitivity=0.1"], ["Sensitivity=0.4"], []),
    "Distance": (["Distance=0"], ["Distance=0.6"], []),
    "Grey Field": (["Grey Field=0"], ["Grey Field=1"], []),
    "View": (["View=0"], ["View=1"], []),
    "Mix": (["Mix=0"], ["Mix=1"], []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
# Audio is the host's FFT buffer: --kicks writes it, and Threshold and Accent
# are swept through it.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide", "Audio"}


def read_png(path):
    """Enough of PNG for sztest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(sztest, out, settings):
    args = [str(sztest), "--out", str(out), "--size", "320x180", "--frames", "150"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"sztest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(sztest):
    result = subprocess.run([str(sztest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"sztest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(sztest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(sztest, scratch / f"{tag}-a.png", context + low)
    after = render(sztest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    sztest = build / "sztest"
    if not sztest.exists():
        print(f"{sztest} not found", file=sys.stderr)
        return 1

    declared = parameters(sztest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(sztest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
