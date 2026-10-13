#pragma once

#include <vector>

/**
    The air: what a pulse from the speaker is, how it travels, and what the
    beam sees of it. Plain C++, no GL, so `sztest --offline` checks it.

    ---------------------------------------------------------------- the pulse

    The speaker is a sphere of radius a (kSpeakerRadius) and its far field is
    a monopole's, which is exact in linear acoustics:

        p( r, t ) = ( a / r ) P_a f( tau ),    tau = t - ( r - a ) / c

    so a pulse keeps its shape and fades as 1/r. P_a is the peak pressure at
    the speaker's surface and f is the waveform, peak 1. Every shape has zero
    net impulse -- the far-field pressure is the cone's acceleration, and a
    cone that comes back to rest has accelerated by nothing overall -- which
    is also what keeps the air behind a ring still.

    ------------------------------------------------------------- steepening

    Loud sound is not linear: each part of the wave travels at c + beta u, so
    compressions catch up with what is in front of them. For a spherical wave
    the distortion accumulates with the AGE variable s = a ln( r / a ) (weak-
    shock theory: Whitham 1974; Hamilton & Blackstock, Nonlinear Acoustics,
    ch. 4), and along it the waveform obeys the inviscid Burgers equation

        f( s, tau ) = f0( theta ),    tau = theta - kappa P_a s f0( theta )

    until that folds, when shocks form where the equal-area rule puts them.
    Steepen() computes the entropy solution EXACTLY on the sampled waveform:
    the Lax-Oleinik minimiser, found from the lower convex hull of
    s U0( y ) + y^2 / 2 in O( N ). The shocks fall out as the hull's long
    edges.

    One approximation, said once: a pulse is steepened at ONE age, the one at
    its shell's centre (thin shell). The error in the shift is the shell's
    width over r ln( r / a ) of the shift itself -- a percent or two by the
    time there is any shift worth having.

    ------------------------------------------------------------ the beam

    The beam crosses the frame perpendicularly, so it sees the line integral
    P( rho ) = integral p dz, rho the distance from the speaker in the frame
    -- an Abel transform. The plugin computes it on the GPU from the shell's
    polyline, in closed form per segment. ProjectedReference() is the
    harness's independent version: brute-force Gauss-Legendre along z.
*/
namespace schlieren::acoustics
{
//---------------------------------------------------------------------------
// Air at 20 C and one atmosphere.
//---------------------------------------------------------------------------
constexpr double kSoundSpeed = 343.2;///< m/s
constexpr double kDensity    = 1.204;///< kg/m^3
/// The coefficient of nonlinearity, ( gamma + 1 ) / 2 for a diatomic gas.
constexpr double kBeta = 1.2;
/// Gladstone-Dale, m^3/kg, visible light: n - 1 = K rho, 2.72e-4 at 1.204 kg/m^3.
constexpr double kGladstoneDale = 2.26e-4;
/// The speaker: the sphere the pulse leaves. A 20 cm driver.
constexpr double kSpeakerRadius = 0.1;
/// 0 dB SPL.
constexpr double kReferencePressure = 20e-6;
/// dn/dp for an adiabatic sound wave, K / c^2, per pascal.
constexpr double kIndexPerPascal = kGladstoneDale / ( kSoundSpeed * kSoundSpeed );
/// kappa = beta / ( rho c^3 ): seconds a pascal gains on the wave per metre of age.
constexpr double kNonlinearity = kBeta / ( kDensity * kSoundSpeed * kSoundSpeed * kSoundSpeed );

constexpr double kPi = 3.14159265358979323846;

/// The waveforms. Box and NWave are the harness's: shapes with closed-form
/// answers, never shown to the host.
enum class Shape
{
	Pop = 0,///< a Gaussian velocity pulse: compression then rarefaction -- a clap
	Kick,   ///< a decaying sine, started smoothly
	Blast,  ///< Friedlander, b = 1: a shock front, then the negative phase
	Count,
	Box,    ///< 1 for Length: a top hat (the closed-form Abel transform)
	NWave   ///< 1 falling to -1 over Length: an N-wave (the closed-form growth law)
};

/// Peak pressure at 1 m, Pa, from dB SPL.
double PressureAtMetre( double db );
/// P_a: the same pulse at the speaker's surface, Pa.
double SurfacePressure( double db );

/// The waveform f0( theta ), theta the retarded time since the cone first
/// moved, peak magnitude 1. Length is the time from the compression's peak
/// to the rarefaction's trough (Pop: 2 sigma; Kick: half a period; Blast: 2
/// t+), so one Length means one size of ring whatever the shape.
double Waveform( Shape shape, double length, double theta );

/// f0 is zero outside [ 0, Support ].
double Support( Shape shape, double length );

/// The steepest rise of f0 in theta, per second. The shock forms at age
/// s* = 1 / ( kappa P_a MaxRise ).
double MaxRise( Shape shape, double length );

/// Where the waveform's weight is, in retarded time: the centroid of | f0 |.
/// A shell is steepened at the radius this has reached.
double Centroid( Shape shape, double length );

/// The age variable s = a ln( r / a ), metres. Zero at the speaker.
double AgeAt( double radius );

/// One vertex of a waveform polyline: x is retarded time (a Profile) or radius
/// (a Shell). A jump is two vertices at the same x.
struct Vertex
{
	double x;
	double f;
};

/// The waveform after steepening, in retarded time, ascending.
struct Profile
{
	std::vector< Vertex > vertices;
	int shocks = 0;///< jumps the hull made, bigger than 1e-6 of the peak
};

/// Hooks for the negative controls. A shipped build passes 0.
enum SteepenHook : int
{
	kHookShockAtVertex = 1 << 0,///< a shock where the hull vertex is, not where the equal-area rule puts it
	kHookNoEntropy     = 1 << 1 ///< the folded Poisson solution, kept multivalued and read in order
};

/// The waveform at age s, for a pulse whose surface pressure is P_a.
Profile Steepen( Shape shape, double length, double surfacePressure, double age, int samples = 4096, int hooks = 0 );

/// A pulse as the beam sees it: the waveform as a function of radius, at one
/// moment, simplified.
struct Shell
{
	std::vector< Vertex > vertices;///< ( r, f ), r ascending
	double outer  = 0.0;           ///< P( rho ) is exactly 0 beyond this
	double inner  = 0.0;           ///< and the air is still inside this
	double gain   = 0.0;           ///< 2 a P_a, Pa m: P = gain * integral f dr / sqrt( r^2 - rho^2 )
	double peak   = 0.0;           ///< max | f |
	double impulse = 0.0;          ///< integral | f | dr, m
};

/// Most vertices a shell may carry to the GPU.
constexpr int kMaxVertices = 256;

/// The shell of a profile `elapsed` seconds after the cone first moved:
/// r = a + c ( elapsed - tau ). What has not left the cone yet is cut at
/// r = a. Simplified to within `tolerance` of the peak (vertically), jumps
/// kept; the tolerance doubles until the shell fits kMaxVertices.
Shell ShellAt( const Profile& profile, double elapsed, double surfacePressure, double tolerance = 2e-4,
               double soundSpeed = kSoundSpeed );

/// Douglas-Peucker by VERTICAL distance, keeping every jump. Exposed for
/// the harness, which checks the bound.
std::vector< Vertex > Simplify( const std::vector< Vertex >& line, double tolerance );

/// The polyline's value at x (the later vertex of a jump wins), 0 outside.
double ValueAt( const std::vector< Vertex >& line, double x );

/// P( rho ), Pa m, by Gauss-Legendre along the line of sight -- NOT the
/// GPU's closed form. The harness's reference. Given `metrePressure` (the
/// pulse's peak at 1 m, from its dB), p is taken as metrePressure ( 1 m / r ) f
/// and the shell's own gain is not used -- so a wrong gain cannot hide by
/// being wrong on both sides.
double ProjectedReference( const Shell& shell, double rho, double metrePressure = -1.0 );

/// A bound on | dP/drho | for rho inside the shell's still interior.
double InteriorSlopeBound( const Shell& shell, double rho );

//---------------------------------------------------------------------------
// Walls: the method of images, exact for rigid planes.
//---------------------------------------------------------------------------
enum class Walls
{
	Open = 0,///< nothing reflects
	Floor,   ///< the frame's bottom edge
	Box,     ///< all four edges
	Count
};

/// The deepest image order Box goes to.
constexpr int kMaxImageOrder = 6;

struct Image
{
	double x, y;     ///< metres, frame coordinates (y up)
	double amplitude;///< reflection^order, signed
	int order;
};

/// The speaker and its images. The frame is [ 0, width ] x [ 0, height ].
/// `reflection` is the walls' pressure reflection coefficient; negative is a
/// pressure-release wall (the harness's negative control, never the host's).
std::vector< Image > Images( Walls walls, double reflection, double x, double y, double width, double height,
                             int maxOrder = kMaxImageOrder );

/// The nearest and farthest a point is from the frame rectangle.
void FrameDistances( double x, double y, double width, double height, double& nearest, double& farthest );

} // namespace schlieren::acoustics
