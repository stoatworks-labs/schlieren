#include "Controls.h"

#include "Acoustics.h"

#include <algorithm>
#include <cmath>

namespace schlieren
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
double linear( float v, double low, double high )
{
	return low + ( high - low ) * clamp01( v );
}
float inverseLinear( double value, double low, double high )
{
	return static_cast< float >( std::clamp( ( value - low ) / ( high - low ), 0.0, 1.0 ) );
}

constexpr double kAccentMost  = 30.0;
constexpr double kLoudLow     = 120.0, kLoudHigh = 190.0;
constexpr double kLengthLow   = 0.05e-3, kLengthHigh = 20e-3;
constexpr double kFieldLow    = 0.5, kFieldHigh = 50.0;
constexpr double kSlowLow     = 30.0, kSlowHigh = 30000.0;
constexpr double kCutoffLow   = 50.0 * kArcsecond, kCutoffHigh = 0.05 * kArcsecond;
constexpr double kDistanceLow = 10.0, kDistanceHigh = 1e5;

const char* const kFireOnNames[]  = { "Beat", "Half Beat", "2 Beats", "Bar", "Onset", "Off" };
const char* const kPulseNames[]   = { "Pop", "Kick", "Blast" };
const char* const kWallsNames[]   = { "Open", "Floor", "Box" };
const char* const kOpticsNames[]  = { "Knife Edge", "Colour", "Shadowgraph", "Background" };
const char* const kViewNames[]    = { "Picture", "Pressure", "Deflection" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0 };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count };
}
ControlInfo event( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Event, 0.0f, nullptr, 0 };
}
ControlInfo of( unsigned int id, const char* name, const char* group, Kind kind, float value )
{
	return { id, name, group, kind, value, nullptr, 0 };
}

//---------------------------------------------------------------------------
// The defaults. A starting pistol's crack, half a millisecond long, fired on
// every beat from the middle of a 6 m field of air, filmed at 400x and seen
// through a knife edge (12 arcseconds to white) focused 150 m beyond it:
// rings light on one side and dark on the other, sharpening as they spread,
// with the clip bent through them.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_FIRE_ON, "Fire On", "Beat", 0.0f, kFireOnNames, static_cast< int >( FireOn::Count ) ),
	event( PT_FIRE, "Fire", "Beat" ),
	of( PT_AUDIO, "Audio", "Beat", Kind::Buffer, 0.0f ),
	standard( PT_THRESHOLD, "Threshold", "Beat", 0.4f ),
	standard( PT_ACCENT, "Accent", "Beat", 0.5f ),

	standard( PT_LOUDNESS, "Loudness", "Speaker", ParamFromLoudness( 165.0 ) ),
	option( PT_PULSE, "Pulse", "Speaker", 0.0f, kPulseNames, static_cast< int >( PulseKind::Count ) ),
	standard( PT_LENGTH, "Length", "Speaker", ParamFromLength( 0.5e-3 ) ),
	of( PT_SPEAKER_X, "Speaker X", "Speaker", Kind::XPos, 0.5f ),
	of( PT_SPEAKER_Y, "Speaker Y", "Speaker", Kind::YPos, 0.5f ),

	standard( PT_FIELD_WIDTH, "Field Width", "Air", ParamFromFieldWidth( 6.0 ) ),
	standard( PT_SLOW_MOTION, "Slow Motion", "Air", ParamFromSlowMotion( 400.0 ) ),
	option( PT_WALLS, "Walls", "Air", 0.0f, kWallsNames, static_cast< int >( acoustics::Walls::Count ) ),
	standard( PT_REFLECTION, "Reflection", "Air", 0.8f ),
	event( PT_CLEAR, "Clear", "Air" ),

	option( PT_OPTICS, "Optics", "Optics", 0.0f, kOpticsNames, static_cast< int >( Optics::Count ) ),
	standard( PT_KNIFE_ANGLE, "Knife Angle", "Optics", 0.0f ),
	standard( PT_SENSITIVITY, "Sensitivity", "Optics", ParamFromCutoff( 12.0 * kArcsecond ) ),
	standard( PT_DISTANCE, "Distance", "Optics", ParamFromDistance( 150.0 ) ),
	standard( PT_GREY_FIELD, "Grey Field", "Optics", 0.0f ),

	option( PT_VIEW, "View", "Output", 0.0f, kViewNames, static_cast< int >( View::Count ) ),
	standard( PT_MIX, "Mix", "Output", 1.0f ),
};
static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in the table" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double SensitivityFromThreshold( float value )
{
	return 1.0 - clamp01( value );
}

double AccentFromParam( float value )
{
	return linear( value, 0.0, kAccentMost );
}

double LoudnessFromParam( float value )
{
	return linear( value, kLoudLow, kLoudHigh );
}
float ParamFromLoudness( double db )
{
	return inverseLinear( db, kLoudLow, kLoudHigh );
}

double LengthFromParam( float value )
{
	return geometric( value, kLengthLow, kLengthHigh );
}
float ParamFromLength( double seconds )
{
	return inverseGeometric( seconds, kLengthLow, kLengthHigh );
}

double FieldWidthFromParam( float value )
{
	return geometric( value, kFieldLow, kFieldHigh );
}
float ParamFromFieldWidth( double metres )
{
	return inverseGeometric( metres, kFieldLow, kFieldHigh );
}

double SlowMotionFromParam( float value )
{
	return geometric( value, kSlowLow, kSlowHigh );
}
float ParamFromSlowMotion( double factor )
{
	return inverseGeometric( factor, kSlowLow, kSlowHigh );
}

double ReflectionFromParam( float value )
{
	return clamp01( value );
}

double KnifeAngleFromParam( float value )
{
	return linear( value, 0.0, 360.0 );
}

double CutoffFromParam( float value )
{
	return geometric( value, kCutoffLow, kCutoffHigh );
}
float ParamFromCutoff( double radians )
{
	return inverseGeometric( radians, kCutoffLow, kCutoffHigh );
}

double DistanceFromParam( float value )
{
	//Exactly zero at the bottom: focused on the air is a thing an operator
	//wants, and "very near" is not the same thing.
	if( value <= 0.0f )
		return 0.0;
	return geometric( value, kDistanceLow, kDistanceHigh );
}
float ParamFromDistance( double metres )
{
	if( metres <= 0.0 )
		return 0.0f;
	return std::max( 1e-6f, inverseGeometric( metres, kDistanceLow, kDistanceHigh ) );
}

} // namespace schlieren
