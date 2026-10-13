#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|sum += flo * L + slope * ( dS - lo * L );|sum += flo * L + slope * ( dS + lo * L );|--abel|GLSL: the segment's r-term with the wrong sign, - -> +"
	"source/Shaders.cpp|: sqrt( shell.x - rho ) / shell.y * float( LutSize - 1 );|: sqrt( shell.x + rho ) / shell.y * float( LutSize - 1 );|--field|GLSL: the table read at sqrt( R + rho ), - -> +"
	"source/Shaders.cpp|vec2 p = ( gl_FragCoord.xy - 1.0 ) * Pitch;|vec2 p = ( gl_FragCoord.xy - 2.0 ) * Pitch;|--field|GLSL: the field a pixel off the frame, 1 -> 2"
	"source/Shaders.cpp|filt = vec3( clamp( 1.0 + uncover, 0.0, 2.0 ) );|filt = vec3( clamp( 1.0 - uncover, 0.0, 2.0 ) );|--knife|GLSL: the knife darkening what it should uncover, + -> -"
	"source/Shaders.cpp|result    = clipAt( here + way * Throw * deflection( here ) );|result    = clipAt( here - way * Throw * deflection( here ) );|--background|GLSL: the backdrop seen the wrong way, + -> -"
	"source/Shaders.cpp|float jxx = 1.0 + Bend * ( e - 2.0 * c + w );|float jxx = 1.0 + Bend * ( e - 3.0 * c + w );|--shadow|GLSL: the Jacobian's second difference with 3 c, 2 -> 3"
	"source/Shaders.cpp|dot( turned, vec2( -0.5, 0.8660254 ) )|dot( turned, vec2(  0.5, 0.8660254 ) )|--colour|GLSL: the colour wheel's green axis at 60 degrees, not 120, - -> space"
	"source/Schlieren.cpp|kIndexPerPascal / ( 2.0 * pitch )|kIndexPerPascal / ( 3.0 * pitch )|--knife|C++: the central difference over three pixels' width, 2 -> 3"
	"source/Acoustics.cpp|H[ j ] = s * W + 0.5 * Y[ j ] * Y[ j ];|H[ j ] = s * W + 0.6 * Y[ j ] * Y[ j ];|--steepen|C++: the Lax-Oleinik parabola 0.6 y^2, 5 -> 6"
	"source/Acoustics.cpp|const double u = 2.0 * x - kPopSpan;|const double u = 3.0 * x - kPopSpan;|--steepen|C++: a Pop's sigma a third of Length, not a half, 2 -> 3"
	"source/Acoustics.cpp|return PressureAtMetre( db ) * ( 1.0 / kSpeakerRadius );|return PressureAtMetre( db ) * ( 2.0 / kSpeakerRadius );|--air|C++: the cone's pressure twice what 1/r spreading gives, 1 -> 2"
	"source/Acoustics.cpp|shell.gain  = 2.0 * kSpeakerRadius * surfacePressure;|shell.gain  = 3.0 * kSpeakerRadius * surfacePressure;|--abel|C++: the shell's gain 3 a P_a, 2 -> 3"
	"source/Acoustics.cpp|radial.push_back( { kSpeakerRadius + soundSpeed * ( elapsed - emitted[ i ].x ), emitted[ i ].f } );|radial.push_back( { kSpeakerRadius + soundSpeed * ( elapsed + emitted[ i ].x ), emitted[ i ].f } );|--speed|C++: a shell laid out backwards in radius, - -> +"
	"source/Acoustics.cpp|return ( i % 2 == 0 ) ? i * side + p : ( i + 1 ) * side - p;|return ( i % 2 == 0 ) ? i * side + p : ( i + 1 ) * side + p;|--walls|C++: an odd image not mirrored, - -> +"
	"source/Acoustics.h|constexpr double kSoundSpeed = 343.2;|constexpr double kSoundSpeed = 343.9;|--air|C++: the speed of sound at 20 C off by 0.2%, 2 -> 9"
	"source/Air.cpp|( boundary - p0 ) / ( p1 - p0 );|( boundary + p0 ) / ( p1 - p0 );|--beat|C++: the beat's place in its frame from the sum of phases, - -> +"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target sztest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	[[ "$check" == "--air" || "$check" == "--steepen" || "$check" == "--beat" ]] || extra=( --size 320x180 )
	limit 300 "$tree/build/sztest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
