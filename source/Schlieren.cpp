#include "Schlieren.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;

namespace schlieren
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. A stalled host must
/// not age every ring by a minute in one frame.
constexpr double kMaxFrameDelta = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

GLuint makeDataTexture( GLint format, GLenum layout, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	//Bound and unbound by hand: this runs before anything is bound, and must
	//leave nothing.
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, layout, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree.
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
SchlierenPlugin::SchlierenPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The rings run on the host's clock: they travel at so many metres a
	//second, so the plugin has to know what a second is.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged parameter is
	// a plain 0..1 float: SetParamInfo clamps a STANDARD default into 0..1
	// before a range could be attached (SDK b1afaf9).
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]          = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::XPos: SetParamInfo( id, info.name, FF_TYPE_XPOS, info.defaultValue ); break;
		case Kind::YPos: SetParamInfo( id, info.name, FF_TYPE_YPOS, info.defaultValue ); break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::Event: SetParamInfo( id, info.name, FF_TYPE_EVENT, false ); break;
		case Kind::Buffer:
			//An FFT buffer: Resolume shows it as an audio-source picker and
			//writes the spectrum into it every frame. With nothing routed,
			//Onset never fires and Accent leaves every pulse at Loudness.
			SetBufferParamInfo( id, info.name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );
}

//---------------------------------------------------------------------------
FFResult SchlierenPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	};
	const Stage stages[] = {
		{ &abelShader, shaders::Pass::Abel, "abel" },
		{ &fieldShader, shaders::Pass::Field, "field" },
		{ &compositeShader, shaders::Pass::Composite, "composite" },
	};
	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Schlieren: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	profileTexture = makeDataTexture( GL_RG32F, GL_RG, acoustics::kMaxVertices, Air::kMaxPulses );
	termTexture    = makeDataTexture( GL_RGBA32F, GL_RGBA, Air::kMaxTerms, 1 );
	if( profileTexture == 0 || termTexture == 0 )
	{
		diag::error( "the data textures failed to allocate" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool SchlierenPlugin::ensureBuffers( GLsizei width, GLsizei height )
{
	//All of it up front, before anything is bound: every ffglex::Scoped*
	//binding clears to 0 on exit rather than restoring, and
	//FFGLFBO::Initialise binds under one.
	//
	//A new raster loses nothing: the field is rebuilt from the pulses every
	//frame, and the pulses live in metres, not pixels.
	bool ok = table.Ensure( shaders::kLutSize, Air::kMaxPulses, GL_R32F, PassBuffer::Sampling::Nearest );
	ok      = ok && pressure.Ensure( width + 2, height + 2, GL_R32F, PassBuffer::Sampling::Nearest );
	return ok;
}

//---------------------------------------------------------------------------
void SchlierenPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//millpond.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
void SchlierenPlugin::UpdateAudio( double dt )
{
	float bins[ audio::kBins ] = {};
	int binCount               = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	}

	audio::Settings settings;
	settings.sensitivity = static_cast< float >( SensitivityFromThreshold( params[ PT_THRESHOLD ] ) );
	analyser.Update( bins, binCount, static_cast< float >( dt ), settings );
}

//---------------------------------------------------------------------------
void SchlierenPlugin::uploadPlan()
{
	//The shells, one row each, ( r, f ) in metres and peak-1 units.
	std::vector< float > texels( static_cast< size_t >( acoustics::kMaxVertices ) * 2 );
	std::fill( std::begin( rows ), std::end( rows ), 0.0f );
	glBindTexture( GL_TEXTURE_2D, profileTexture );
	for( size_t row = 0; row < plan.shells.size() && row < static_cast< size_t >( Air::kMaxPulses ); ++row )
	{
		const acoustics::Shell& shell = plan.shells[ row ];
		const int count               = static_cast< int >( std::min< size_t >( shell.vertices.size(), acoustics::kMaxVertices ) );
		if( count < 2 )
			continue;
		for( int k = 0; k < count; ++k )
		{
			texels[ static_cast< size_t >( k ) * 2 + 0 ] = static_cast< float >( shell.vertices[ static_cast< size_t >( k ) ].x );
			texels[ static_cast< size_t >( k ) * 2 + 1 ] = static_cast< float >( shell.vertices[ static_cast< size_t >( k ) ].f );
		}
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, static_cast< GLint >( row ), count, 1, GL_RG, GL_FLOAT, texels.data() );
		rows[ row * 4 + 0 ] = static_cast< float >( shell.outer );
		rows[ row * 4 + 1 ] = static_cast< float >( std::sqrt( shell.outer ) );
		rows[ row * 4 + 2 ] = static_cast< float >( shell.gain );
		rows[ row * 4 + 3 ] = static_cast< float >( count );
	}

	std::vector< float > terms( static_cast< size_t >( Air::kMaxTerms ) * 4, 0.0f );
	const size_t termCount = std::min< size_t >( plan.terms.size(), Air::kMaxTerms );
	for( size_t t = 0; t < termCount; ++t )
	{
		terms[ t * 4 + 0 ] = plan.terms[ t ].x;
		terms[ t * 4 + 1 ] = plan.terms[ t ].y;
		terms[ t * 4 + 2 ] = plan.terms[ t ].amplitude;
		terms[ t * 4 + 3 ] = static_cast< float >( plan.terms[ t ].row );
	}
	glBindTexture( GL_TEXTURE_2D, termTexture );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, Air::kMaxTerms, 1, GL_RGBA, GL_FLOAT, terms.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

//---------------------------------------------------------------------------
FFResult SchlierenPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	const GLsizei width             = static_cast< GLsizei >( source.Width );
	const GLsizei height            = static_cast< GLsizei >( source.Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// Time, the audio and the beat.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;
	UpdateAudio( dt );

	if( barPhase != lastBarPhase && lastBarPhase >= 0.0f )
		hostBeatSeen = true;
	lastBarPhase = barPhase;

	//---------------------------------------------------------------------
	// The air.
	//---------------------------------------------------------------------
	frameWidth  = FieldWidthFromParam( params[ PT_FIELD_WIDTH ] );
	frameHeight = frameWidth * static_cast< double >( height ) / static_cast< double >( width );
	const double pitch = frameWidth / static_cast< double >( width );

	FireInputs in;
	in.fireOn    = static_cast< FireOn >( OptionIndex( params[ PT_FIRE_ON ], static_cast< int >( FireOn::Count ) ) );
	in.presses   = firePresses;
	in.onset     = analyser.Fired();
	in.kick      = analyser.Kick();
	in.level     = analyser.Level();
	in.hasSignal = analyser.HasSignal();
	in.barPhase  = barPhase;
	in.bpm       = bpm;
	in.hostBeat  = hostBeatSeen;
	in.loudness  = LoudnessFromParam( params[ PT_LOUDNESS ] );
	in.accent    = AccentFromParam( params[ PT_ACCENT ] );
	in.shape     = static_cast< acoustics::Shape >( OptionIndex( params[ PT_PULSE ], static_cast< int >( PulseKind::Count ) ) );
	in.length    = LengthFromParam( params[ PT_LENGTH ] );
	in.x         = std::clamp( static_cast< double >( params[ PT_SPEAKER_X ] ), 0.0, 1.0 ) * frameWidth;
	in.y         = std::clamp( static_cast< double >( params[ PT_SPEAKER_Y ] ), 0.0, 1.0 ) * frameHeight;
	firePresses  = 0;

	if( shapeOverride >= 0 )
		in.shape = static_cast< acoustics::Shape >( shapeOverride );
	if( resizeClears && lastWidth > 0 && ( width != lastWidth || height != lastHeight ) )
		clearWanted = true;
	lastWidth  = width;
	lastHeight = height;

	if( clearWanted )
	{
		air.Clear();
		clearWanted = false;
	}
	air.Advance( now, dt, SlowMotionFromParam( params[ PT_SLOW_MOTION ] ), in );

	const auto optics     = static_cast< Optics >( OptionIndex( params[ PT_OPTICS ], static_cast< int >( Optics::Count ) ) );
	const auto view       = static_cast< View >( OptionIndex( params[ PT_VIEW ], static_cast< int >( View::Count ) ) );
	const double cutoff   = CutoffFromParam( params[ PT_SENSITIVITY ] );
	const double distance = DistanceFromParam( params[ PT_DISTANCE ] );

	//The smallest deflection that would show: Sensitivity's, or a pixel at
	//Distance, whichever the optics can see.
	double visible = cutoff;
	if( distance > 0.0 && view == View::Picture )
	{
		const double pixelAtDistance = pitch / distance;
		visible = ( optics == Optics::KnifeEdge || optics == Optics::Colour ) ? std::min( cutoff, pixelAtDistance ) : pixelAtDistance;
	}
	const auto walls = static_cast< acoustics::Walls >( OptionIndex( params[ PT_WALLS ], static_cast< int >( acoustics::Walls::Count ) ) );
	plan = air.Build( walls, ReflectionFromParam( params[ PT_REFLECTION ] ) * reflectionSign, frameWidth, frameHeight, visible );

	if( !ensureBuffers( width, height ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}
	uploadPlan();

	//---------------------------------------------------------------------
	// 1 and 2: the table, then the field. Nothing in flight: still air.
	//---------------------------------------------------------------------
	const int rowCount = static_cast< int >( std::min< size_t >( plan.shells.size(), Air::kMaxPulses ) );
	if( plan.terms.empty() || rowCount == 0 )
		pressure.Clear();
	else
	{
		{
			ScopedFBOBinding fbo( table.GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, shaders::kLutSize, rowCount );
			ScopedShaderBinding shader( abelShader.GetGLID() );
			bindUnit( 0, profileTexture );
			abelShader.Set( "Profiles", 0 );
			glUniform4fv( abelShader.FindUniform( "Rows" ), Air::kMaxPulses, rows );
			abelShader.Set( "LutSize", shaders::kLutSize );
			abelShader.Set( "SpeakerRadius", static_cast< float >( acoustics::kSpeakerRadius ) );
			abelShader.Set( "Hooks", hooks );
			quad.Draw();
			unbindTextureUnits( 1 );
		}
		{
			ScopedFBOBinding fbo( pressure.GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, width + 2, height + 2 );
			ScopedShaderBinding shader( fieldShader.GetGLID() );
			bindUnit( 0, table.TextureID() );
			bindUnit( 1, termTexture );
			fieldShader.Set( "Lut", 0 );
			fieldShader.Set( "Terms", 1 );
			fieldShader.Set( "TermCount", static_cast< int >( std::min< size_t >( plan.terms.size(), Air::kMaxTerms ) ) );
			glUniform4fv( fieldShader.FindUniform( "Rows" ), Air::kMaxPulses, rows );
			fieldShader.Set( "LutSize", shaders::kLutSize );
			fieldShader.Set( "Pitch", static_cast< float >( pitch ), static_cast< float >( pitch ) );
			fieldShader.Set( "Hooks", hooks );
			quad.Draw();
			unbindTextureUnits( 2 );
		}
	}

	//---------------------------------------------------------------------
	// 3. The optics, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );
		const double knife            = KnifeAngleFromParam( params[ PT_KNIFE_ANGLE ] ) * acoustics::kPi / 180.0;

		//What the Pressure view calls full scale: the loudest shell in
		//flight's P near its front, gain peak sqrt( 2 w / R ) with w its width.
		double scale = 0.0;
		for( const acoustics::Shell& shell : plan.shells )
			if( shell.vertices.size() >= 2 )
			{
				const double width_ = std::max( shell.outer - shell.inner, 1e-3 );
				scale = std::max( scale, shell.gain * shell.peak * std::sqrt( 2.0 * width_ / std::max( shell.outer, width_ ) ) );
			}

		ScopedShaderBinding shader( compositeShader.GetGLID() );
		bindUnit( 0, source.Handle );
		bindUnit( 1, pressure.TextureID() );
		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "Pressure", 1 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		setInt2( compositeShader, "Size", width, height );
		compositeShader.Set( "Slope", static_cast< float >( acoustics::kIndexPerPascal / ( 2.0 * pitch ) ) );
		compositeShader.Set( "Bend", static_cast< float >( distance * acoustics::kIndexPerPascal / ( pitch * pitch ) ) );
		compositeShader.Set( "Throw", static_cast< float >( distance / pitch ) );
		compositeShader.Set( "Cutoff", static_cast< float >( cutoff ) );
		compositeShader.Set( "Knife", static_cast< float >( std::cos( knife ) ), static_cast< float >( std::sin( knife ) ) );
		compositeShader.Set( "OpticsMode", static_cast< int >( optics ) );
		compositeShader.Set( "ViewMode", static_cast< int >( view ) );
		compositeShader.Set( "PressureScale", static_cast< float >( scale > 0.0 ? scale : 1.0 ) );
		compositeShader.Set( "GreyField", std::clamp( params[ PT_GREY_FIELD ], 0.0f, 1.0f ) );
		compositeShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		compositeShader.Set( "Hooks", hooks );
		setInt2( compositeShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		quad.Draw();
		unbindTextureUnits( 2 );
		glActiveTexture( GL_TEXTURE0 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SchlierenPlugin::DeInitGL()
{
	abelShader.FreeGLResources();
	fieldShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	table.Destroy();
	pressure.Destroy();
	if( profileTexture != 0 )
		glDeleteTextures( 1, &profileTexture );
	if( termTexture != 0 )
		glDeleteTextures( 1, &termTexture );
	profileTexture = termTexture = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SchlierenPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void SchlierenPlugin::SetBeatInfo( float newBpm, float newBarPhase )
{
	CFFGLPlugin::SetBeatInfo( newBpm, newBarPhase );
	if( newBpm > 0.0f && std::isfinite( newBpm ) )
		bpm = newBpm;
	if( std::isfinite( newBarPhase ) )
		barPhase = newBarPhase - std::floor( newBarPhase );
}

char* SchlierenPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult SchlierenPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult SchlierenPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//The two buttons act on the press, once: an event arrives as 1 on press
	//and 0 on release, and a host is free to restate either.
	const bool down = value >= 0.5f;
	if( index == PT_FIRE )
	{
		if( down && !fireHeld )
			++firePresses;
		fireHeld = down;
	}
	else if( index == PT_CLEAR )
	{
		if( down && !clearHeld )
			clearWanted = true;
		clearHeld = down;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float SchlierenPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

} // namespace schlieren
