/*
    The page's way in to the plugin: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call.

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Schlieren.cpp` and everything it calls (Air, Acoustics,
    Audio, Controls, Shaders, PassBuffer, Diag), compiled UNMODIFIED into the
    same WebAssembly module as this file, with the FFGL SDK pieces it needs
    (see demo/tools/build-wasm.sh for the list). This file only plays the host:

      - it constructs a `SchlierenPlugin`, as PluginEntry.cpp's registration
        would through CFFGLPluginInfo, and calls InitGL / DeInitGL;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters (GetParamName, GetParamType, GetParamGroup,
        GetParamDefault, the element and usage getters -- the SDK compiled
        unmodified too), so the page's panel is built from the plugin's
        constructor rather than from a copy of it;
      - it forwards the host's calls: SetFloatParameter, and each frame
        SetTime, SetBeatInfo and ProcessOpenGL with one input texture, in that
        order, as tools/sztest's Rig does. SetTextParameter is here too, but
        the plugin's one text parameter, the About line, is display only and
        the page never sends it.

    Two things are the page's and not the plugin's, and the page says so:

      - The transport. SetBeatInfo is handed a BPM and a bar phase from a
        clock the page keeps (a BPM field under the picture, 120 at first,
        and a bar phase advanced from the page's own seconds), because a
        browser has no Resolume transport. The plugin cannot tell it from
        one: it reads the bar phase moving and fires on its boundaries, placed
        inside the frame where each fell.
      - The audio. The plugin declares an FFT buffer (Audio) that Resolume
        fills every frame. This file never writes it, so the plugin's onset
        analyser hears silence: Fire On Onset never fires, and with no signal
        Accent leaves every pulse at Loudness -- what the plugin does in
        Resolume with nothing routed.

    The clock is the page's too: SetTime is handed the kit's seconds, and the
    plugin's own vote decides the unit (seconds, within four frames, as in
    Arena). Before the vote settles the plugin runs on its wall clock, which
    here is the browser's performance.now().

    The read-outs use the plugin's harness accessors (AirState, LastPlan,
    FrameWidthMetres, FrameHeightMetres) or its own tables (Controls.cpp, the
    About block's buttons); they report and never change anything. The one
    exception, sz_set_clock_scale_for_test, is SetClockScaleForTest, the
    accessor sztest uses to declare its clock in seconds from the first frame;
    the page never calls it, demo/tools/crosscheck does, so its frames line up
    with sztest's frame for frame.

    GL calls go to the page's WebGL2 context through emscripten's GL library;
    demo/wasm/gl_shim.cpp replaces the five entry points that cannot be passed
    straight through.
*/
#include "Acoustics.h"
#include "Controls.h"
#include "Diag.h"
#include "Schlieren.h"

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using namespace schlieren;

namespace
{
/// One plugin instance and the host-side structs it is handed.
struct Instance
{
	SchlierenPlugin plugin;
	bool initialised = false;
	unsigned long long frames = 0;///< ProcessOpenGL calls that succeeded

	FFGLTextureStruct input {};
	FFGLTextureStruct* inputs[ 1 ] = { &input };
	ProcessOpenGLStruct process {};

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}

constexpr double kNaN = std::numeric_limits< double >::quiet_NaN();
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// The plugin's log. Diag.cpp opens it in InitGL (diag::init), so this must run
// before the first sz_init_gl: a directory in the page's in-memory file
// system, named through the override Diag.cpp reads. Its own `mkdir -p` goes
// through system(), which a browser does not have.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE void sz_prepare_log()
{
	setenv( "SCHLIEREN_LOG_DIR", "/schlieren/logs", 1 );
	mkdir( "/schlieren", 0755 );
	mkdir( "/schlieren/logs", 0755 );
}

EMSCRIPTEN_KEEPALIVE const char* sz_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE Instance* sz_new()
{
	return new Instance();
}

EMSCRIPTEN_KEEPALIVE void sz_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int sz_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width        = static_cast< GLuint >( width );
	viewport.height       = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

/// The harness's clock declaration (SetClockScaleForTest): seconds from the
/// first frame, no vote. For demo/tools/crosscheck only; the page never calls it.
EMSCRIPTEN_KEEPALIVE void sz_set_clock_scale_for_test( Instance* instance, double scale )
{
	instance->plugin.SetClockScaleForTest( scale );
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int sz_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* sz_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int sz_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE int sz_param_usage( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamUsage( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* sz_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter (FF_TYPE_STANDARD already
/// clamped into 0..1 by the SDK, as a host sees it).
EMSCRIPTEN_KEEPALIVE float sz_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a text parameter.
EMSCRIPTEN_KEEPALIVE const char* sz_param_default_text( Instance* instance, int index )
{
	const FFMixed mixed = instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) );
	return mixed.PointerValue ? static_cast< const char* >( mixed.PointerValue ) : "";
}

EMSCRIPTEN_KEEPALIVE int sz_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* sz_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float sz_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE int sz_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int sz_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float sz_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

EMSCRIPTEN_KEEPALIVE int sz_set_text( Instance* instance, int index, const char* value )
{
	return instance->plugin.SetTextParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* sz_get_text( Instance* instance, int index )
{
	const char* text = instance->plugin.GetTextParameter( static_cast< unsigned int >( index ) );
	return text ? text : "";
}

/// One frame, in the order tools/sztest's Rig makes the calls: SetTime,
/// SetBeatInfo with the page's transport, then ProcessOpenGL with one input of
/// clipWidth x clipHeight whose GL name is `clip` (a texture the page
/// registered with emscripten's GL tables), drawing into whatever framebuffer
/// and viewport the page has bound, as a host's. 1 on success.
EMSCRIPTEN_KEEPALIVE int sz_process( Instance* instance, double seconds, float bpm, float barPhase, int clip, int clipWidth,
                                     int clipHeight )
{
	instance->plugin.SetTime( seconds );
	instance->plugin.SetBeatInfo( bpm, barPhase );
	instance->input.Width = instance->input.HardwareWidth = static_cast< FFUInt32 >( clipWidth );
	instance->input.Height = instance->input.HardwareHeight = static_cast< FFUInt32 >( clipHeight );
	instance->input.Handle                                 = static_cast< GLuint >( clip );
	instance->process.numInputTextures                     = 1;
	instance->process.inputTextures                        = instance->inputs;
	instance->process.HostFBO                              = 0;
	const bool ok = instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS;
	if( ok )
		++instance->frames;
	return ok ? 1 : 0;
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.cpp's conversion for a parameter, the
// one Schlieren.cpp's ProcessOpenGL applies to it, in the unit the page prints
// (Sensitivity's radians as arcseconds by the plugin's own kArcsecond). NaN
// where the plugin uses the host's 0..1 as it is (Grey Field, Mix) or scales
// it by something else (Speaker X and Y, by the frame's size in metres), and
// for the controls that are not ranged.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double sz_convert( int index, float v )
{
	switch( index )
	{
	case PT_THRESHOLD: return SensitivityFromThreshold( v );
	case PT_ACCENT: return AccentFromParam( v );
	case PT_LOUDNESS: return LoudnessFromParam( v );
	case PT_LENGTH: return LengthFromParam( v );
	case PT_FIELD_WIDTH: return FieldWidthFromParam( v );
	case PT_SLOW_MOTION: return SlowMotionFromParam( v );
	case PT_REFLECTION: return ReflectionFromParam( v );
	case PT_KNIFE_ANGLE: return KnifeAngleFromParam( v );
	case PT_SENSITIVITY: return CutoffFromParam( v ) / kArcsecond;
	case PT_DISTANCE: return DistanceFromParam( v );
	default: return kNaN;
	}
}

/// The other way, for the page's presets: a setting in sz_convert's units to
/// the host's 0..1, by Controls.cpp's own inverse (ParamFromLoudness and the
/// rest). NaN for a control that has none.
EMSCRIPTEN_KEEPALIVE double sz_param_from( int index, double x )
{
	switch( index )
	{
	case PT_LOUDNESS: return ParamFromLoudness( x );
	case PT_LENGTH: return ParamFromLength( x );
	case PT_FIELD_WIDTH: return ParamFromFieldWidth( x );
	case PT_SLOW_MOTION: return ParamFromSlowMotion( x );
	case PT_SENSITIVITY: return ParamFromCutoff( x * kArcsecond );
	case PT_DISTANCE: return ParamFromDistance( x );
	default: return kNaN;
	}
}

//---------------------------------------------------------------------------
// The About block's buttons: the URL each one opens, from the
// StoatworksAboutLinks.h the plugin is built with. The host getters give only
// a button's label; the page needs the address the plugin's own press would
// have opened, because that press calls system(), which a browser does not
// have.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int sz_about_first()
{
	return static_cast< int >( PT_ABOUT_FIRST );
}

EMSCRIPTEN_KEEPALIVE const char* sz_about_button_label( int button )
{
	const auto& list = stoatworks::about::buttons();
	return button >= 0 && static_cast< size_t >( button ) < list.size() ? list[ static_cast< size_t >( button ) ].label : "";
}

EMSCRIPTEN_KEEPALIVE const char* sz_about_button_url( int button )
{
	const auto& list = stoatworks::about::buttons();
	return button >= 0 && static_cast< size_t >( button ) < list.size() ? list[ static_cast< size_t >( button ) ].url : "";
}

//---------------------------------------------------------------------------
// What the air is doing, for the status line, through the harness's
// accessors. 0 = pulses in flight; 1 = pulses fired since the instance began;
// 2 = terms (images) the last frame sent to the GPU; 3 = terms it dropped over
// 128; 4 = shocks in flight (the hull's jumps over 1e-6 of the peak, summed
// over the pulses' current profiles); 5 = the air's clock, seconds; 6 and 7 =
// the frame's width and height, metres; 8 = the newest pulse's level, dB SPL
// at 1 m; 9 = frames rendered. NaN before the first frame, and 8 with nothing
// in flight.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double sz_state( Instance* instance, int what )
{
	if( instance->frames == 0 )
		return kNaN;
	const Air& air   = instance->plugin.AirState();
	const Plan& plan = instance->plugin.LastPlan();
	switch( what )
	{
	case 0: return static_cast< double >( air.Pulses().size() );
	case 1: return static_cast< double >( air.Fired() );
	case 2: return static_cast< double >( plan.terms.size() );
	case 3: return static_cast< double >( plan.dropped );
	case 4:
	{
		int shocks = 0;
		for( const Pulse& pulse : air.Pulses() )
			if( pulse.profileAge >= 0.0 )
				shocks += pulse.profile.shocks;
		return shocks;
	}
	case 5: return air.Time();
	case 6: return instance->plugin.FrameWidthMetres();
	case 7: return instance->plugin.FrameHeightMetres();
	case 8: return air.Pulses().empty() ? kNaN : air.Pulses().back().db;
	case 9: return static_cast< double >( instance->frames );
	default: return kNaN;
	}
}

} // extern "C"
