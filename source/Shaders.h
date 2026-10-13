#pragma once

#include <string>

/**
	The passes.

	1. **abel** -- one row per pulse, kLutSize texels: the line-of-sight
	   integral P( rho ) = integral p dz of the pulse's shell, in closed form
	   per segment of its polyline (Acoustics.h). Texel i is at
	   rho = R - q^2, q = sqrt( R ) i / ( kLutSize - 1 ): uniform in
	   sqrt( R - rho ), so the square-root edge of a shock front is a straight
	   line in the table and linear interpolation is exact there.
	2. **field** -- the frame plus a one-pixel margin, R32F: P summed over
	   every term (an image of the speaker and the pulse it points at). The
	   margin is there so the composite's central differences are central at
	   the frame's edge too.
	3. **composite** -- to the host. The deflection is the central difference
	   of P over two pixels -- the mean gradient over them, exactly -- times
	   K / c^2; then the optics.

	The fragment shaders are assembled from pieces at InitGL -- kVersion +
	kCommon + the pass -- by `Assemble()`, the one place the order is written.
	tools/glslc.sh's ASSEMBLED table mirrors it, so the text it compiles is
	the text the plugin runs.

	No sin, cos, tan or atan anywhere (GLSL promises nothing about their
	accuracy): the knife's direction and the colour wheel's axes come from
	the CPU, and the colour wheel is three dot products.
*/
namespace schlieren::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kAbelFragment;
extern const char* const kFieldFragment;
extern const char* const kCompositeFragment;

enum class Pass
{
	Abel,
	Field,
	Composite
};
std::string Assemble( Pass pass );

/// Texels per pulse in the projection table.
constexpr int kLutSize = 2048;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model.
enum Hook : int
{
	kHookNoSpreading  = 1 << 0,///< the shell's pressure without its 1/r
	kHookLinearTable  = 1 << 1,///< the table read as if uniform in rho, not in sqrt( R - rho )
	kHookOneSided     = 1 << 2,///< the deflection a one-sided difference: half a pixel off
	kHookKnifeFlip    = 1 << 3,///< the knife on the other side
	kHookNoJacobian   = 1 << 4,///< the shadowgraph's light moved but not concentrated
	kHookBackwards    = 1 << 5,///< the backdrop seen at x - L eps
	kHookColourSwap   = 1 << 6 ///< the colour wheel turned the wrong way
};

} // namespace schlieren::shaders
