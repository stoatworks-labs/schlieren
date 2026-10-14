# Attributions

Schlieren is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the harness and the tooling — Stoatworks fiche

<https://github.com/stoatworks-labs/fiche>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, source/GLState.h (radar's, which is vectrix's, from resolume-scopes), the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline, and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py, the CI and release workflows and scripts/release-lib.sh are fiche's, adapted. The idea of firing a beat at the moment inside the frame where it fell (source/Air.h) is fiche's lesson.

### The audio analyser, the host-clock vote, the pass buffers and the log — Stoatworks millpond

<https://github.com/stoatworks-labs/millpond>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is millpond's onset analyser (rosette's, adapted from macroblock's), with its primed first frame; the host-clock unit vote in SchlierenPlugin::UpdateClock is rosette's by way of millpond; source/PassBuffer.{h,cpp} (FFGLFBO with the SDK's colour-texture leak fixed) and source/Diag.{h,cpp} are millpond's. All adapted.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Weak-shock theory for a spherical wave — G. B. Whitham, Linear and Nonlinear Waves (1974); M. F. Hamilton and D. T. Blackstock, Nonlinear Acoustics, ch. 4

Steepen() in source/Acoustics.cpp: along the age variable s = a ln( r / a ) a loud pulse obeys the inviscid Burgers equation, with kappa = beta / ( rho c^3 ) and beta = ( gamma + 1 ) / 2, until it folds and shocks form at the equal-area position. sztest --steepen holds it to the shock-formation age s* = 1 / ( kappa P_a max f0' ) and to the closed forms for a top hat's front (-L + sqrt( 2 k L s )) and an N-wave's growth (L sqrt( 1 + 2 k s / L )). A shock front's speed of c plus half its strength (Rankine-Hugoniot) falls out of the same solution. Implemented from the theory; no code is used.

### The Lax–Oleinik formula — Peter D. Lax; Olga A. Oleinik

Steepen() takes the entropy solution of the Burgers problem, shocks and all, as the Lax–Oleinik minimiser of s W + Y^2 / 2 - X Y, found as a vertex of the lower convex hull of the samples in O( N ); the hull's long edges are the shocks.

### Godunov's method — S. K. Godunov

tools/sztest/main.cpp's godunov(): an independent first-order solver of w_s + ( w^2 / 2 )_X = 0 that sztest --steepen compares Pop, Kick and Blast against (2400 cells, within the solver's own error).

### The Friedlander waveform — F. G. Friedlander

The Blast shape in source/Acoustics.cpp: ( 1 - u ) e^-u, u = theta / t+, with b = 1, the one decay whose impulse is zero, arriving as a shock.

### Edlén's standard air — Bengt Edlén

sztest --air checks n - 1 = K rho (the Gladstone–Dale constant and the density in source/Acoustics.h) against Edlén's standard air at 15 °C, n - 1 = 2.77e-4 at 589 nm, scaled by density to 20 °C.

### The Douglas–Peucker simplification — David H. Douglas and Thomas K. Peucker

Simplify() in source/Acoustics.cpp: each steepened profile and each shell is thinned by vertical distance before it goes to the GPU, keeping every jump and the edges of silence.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Schlieren photography, and the people who photograph shock waves with it — August Toepler and those after him

Toepler's knife edge at the focus of a lens (1864), colour schlieren's filter in its place, the shadowgraph's screen, and background-oriented schlieren's distant backdrop (real ones use distant terrain; NASA has photographed supersonic shocks against the desert floor from above). Built from how the optics work; nothing is copied from anyone's software, images or design.

## Standards and published specifications

What the implementation is measured against.

- **The far field of a pulsating sphere (linear acoustics)** — p( r, t ) = ( a / r ) P_a f( t - ( r - a ) / c ) for the 20 cm speaker in source/Acoustics.h and ShellAt(): every pulse keeps its shape and fades as 1/r, and every shape has zero net impulse. sztest --speed puts the front at a + c t and --spreading the projection's fall as R^-1/2.
- **The Gladstone–Dale relation** — n - 1 = K rho, K = 2.26e-4 m^3/kg for visible light (kGladstoneDale in source/Acoustics.h); for an adiabatic sound wave dn/dp = K / c^2, so the deflection is eps = ( K / c^2 ) grad P, taken in the composite pass in source/Shaders.cpp as a central difference over two pixels.
- **The Abel transform** — What the beam sees of a spherical shell, P( rho ) = integral p dz: worked out in closed form per segment of the shell's polyline in the abel pass in source/Shaders.cpp, into a table per pulse; sztest --abel checks it against ProjectedReference().
- **Gauss–Legendre quadrature** — ProjectedReference() in source/Acoustics.cpp, the harness's brute-force line of sight along z (16 points per piece), independent of the GPU's closed form.
- **The method of images** — Images() in source/Acoustics.cpp: the Floor and Box walls as mirror images of the speaker, to order 6, each weighted by Reflection to the power of its order, exact for rigid planes. sztest --walls checks a rigid floor mirrors the field bit for bit.
- **The schlieren, shadowgraph and background-oriented schlieren relations** — The composite pass in source/Shaders.cpp: the knife edge I / I0 = clamp( 1 + eps . k / eps_c, 0, 2 ); the colour wheel's hue from the deflection's direction; the shadowgraph's light landing at x + L eps with intensity 1 / |det J| (to first order I / I0 - 1 = -L div eps, which sztest --shadow checks); and the backdrop seen at x + L eps (--background).
- **The ideal gas law and CODATA's molar gas constant** — sztest --air derives c = sqrt( gamma R T / M ) and rho = p M / ( R T ) for dry air (M = 28.965 g/mol) at 20 °C and one atmosphere, and checks the speed of sound and density in source/Acoustics.h against them.
- **Sound pressure level, re 20 µPa** — Loudness is peak dB SPL at 1 m: PressureAtMetre() in source/Acoustics.cpp, which sztest --air checks at 140 dB = 200 Pa.
- **IEC 61966-2-1 (sRGB)** — toLinear() and toCode() in source/Shaders.cpp: the clip is linearised so the knife, the colour wheel and the shadowgraph multiply light, then encoded again.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
