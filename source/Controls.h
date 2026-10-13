#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean in physical units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index; Fire and Clear are presses;
    Audio is Resolume's 64-bin FFT buffer.

    Units: metres, seconds, pascals (as dB SPL at 1 m), radians of
    deflection (shown as arcseconds, which is how schlieren people talk).
*/
namespace schlieren
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Beat: what fires a pulse --------------------------------------------
	PT_FIRE_ON = 0,
	PT_FIRE,
	PT_AUDIO,
	PT_THRESHOLD,
	PT_ACCENT,

	// -- Speaker: the pulse ---------------------------------------------------
	PT_LOUDNESS,
	PT_PULSE,
	PT_LENGTH,
	PT_SPEAKER_X,
	PT_SPEAKER_Y,

	// -- Air: the room and the camera -----------------------------------------
	PT_FIELD_WIDTH,
	PT_SLOW_MOTION,
	PT_WALLS,
	PT_REFLECTION,
	PT_CLEAR,

	// -- Optics: what turns bent light into a picture --------------------------
	PT_OPTICS,
	PT_KNIFE_ANGLE,
	PT_SENSITIVITY,
	PT_DISTANCE,
	PT_GREY_FIELD,

	// -- Output ----------------------------------------------------------------
	PT_VIEW,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	Event,   ///< a press
	Buffer,  ///< the FFT
	XPos,    ///< 0..1 across the frame
	YPos     ///< 0..1 up the frame
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, or the index
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options.
//---------------------------------------------------------------------------
enum class FireOn
{
	Beat = 0,///< every beat of Resolume's clock
	HalfBeat,///< every quaver
	TwoBeats,
	Bar,
	Onset,   ///< every onset the audio analyser finds
	Off,     ///< only the Fire button
	Count
};

/// The waveform: acoustics::Shape's first three, in the same order.
enum class PulseKind
{
	Pop = 0,
	Kick,
	Blast,
	Count
};

enum class Optics
{
	KnifeEdge = 0,///< schlieren: a knife edge at the light source's image
	Colour,       ///< colour schlieren: a colour wheel there instead
	Shadowgraph,  ///< no filter: the light lands on a screen Distance beyond
	Background,   ///< background-oriented schlieren: the clip is a backdrop Distance behind
	Count
};

enum class View
{
	Picture = 0,
	Pressure,  ///< the line-integrated pressure P, signed grey
	Deflection,///< the deflection, x as red and y as green
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

/// The onset detector's bar: Threshold 0 fires on anything, 1 on very little.
/// Returned as millpond's analyser sensitivity (1 - Threshold).
double SensitivityFromThreshold( float value );

/// Accent: 0 to 30 dB, linearly -- how far below Loudness the softest hit
/// lands. 0 makes every pulse the same.
double AccentFromParam( float value );

/// Loudness: peak dB SPL at 1 m, 120 to 190, linearly. 150 is a big PA's
/// peak; 165 a starting pistol at a metre; 190 is past where weak-shock
/// theory is honest (AGENTS.md).
double LoudnessFromParam( float value );
float ParamFromLoudness( double db );

/// Length: 0.05 to 20 ms, geometrically: from a spark to a low kick.
double LengthFromParam( float value );
float ParamFromLength( double seconds );

/// The frame's width in metres: 0.5 to 50, geometrically.
double FieldWidthFromParam( float value );
float ParamFromFieldWidth( double metres );

/// Slow motion: 30x to 30 000x, geometrically -- a 1 800 to 1 800 000 fps
/// camera played back at 60.
double SlowMotionFromParam( float value );
float ParamFromSlowMotion( double factor );

/// The walls' pressure reflection coefficient, 0 to 1, linearly.
double ReflectionFromParam( float value );

/// The knife's angle, 0 to 360 degrees, linearly. 0: the knife edge is
/// vertical and a deflection to the right uncovers the light.
double KnifeAngleFromParam( float value );

/// Sensitivity: the deflection that takes the knife from grey to white,
/// 50 arcseconds at 0 down to 0.05 at 1, geometrically. In radians.
double CutoffFromParam( float value );
float ParamFromCutoff( double radians );

/// Distance: the camera's focus (or the screen, or the backdrop) beyond the
/// air, metres. Exactly 0 at the bottom of the travel; then 10 m to 100 km,
/// geometrically. Kilometres are not a misprint: see AGENTS.md.
double DistanceFromParam( float value );
float ParamFromDistance( double metres );

/// One arcsecond in radians.
constexpr double kArcsecond = 3.14159265358979323846 / ( 180.0 * 3600.0 );

} // namespace schlieren
