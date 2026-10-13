#pragma once

#include "Air.h"
#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace schlieren
{
/**
    The plugin.

    Two models in sequence: the air (Air.h and Acoustics.h, on the CPU: which
    pulses are in flight and what their shells look like) and the light
    (Shaders.h, on the GPU: what the beam sees of them, then the optics). See
    AGENTS.md for why each is built the way it is.
*/
class SchlierenPlugin : public CFFGLPlugin
{
public:
	SchlierenPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `sztest` can measure what the shipping passes produced,
	// rather than a copy of it computed for the test.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock -- a harness
	/// renders hundreds of frames a second, which the vote would read as
	/// milliseconds.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	void SetAirHooksForTest( int bits )
	{
		air.SetHooksForTest( bits );
	}
	/// -1 makes every wall pressure-release: the --walls negative control.
	void SetReflectionSignForTest( double sign )
	{
		reflectionSign = sign;
	}
	/// Fire the harness's shapes (Box, NWave), which the host never sees.
	void SetShapeForTest( acoustics::Shape shape )
	{
		shapeOverride = static_cast< int >( shape );
	}
	/// The --resize negative control: a new raster forgets the air.
	void SetResizeClearsForTest( bool clears )
	{
		resizeClears = clears;
	}

	/// P on the frame and its one-pixel margin, ( w + 2 ) x ( h + 2 ), R32F, Pa m.
	GLuint PressureTextureID() const
	{
		return pressure.TextureID();
	}
	/// The projection table, kLutSize x Air::kMaxPulses, R32F, Pa m.
	GLuint TableTextureID() const
	{
		return table.TextureID();
	}
	/// What the last frame sent to the GPU.
	const Plan& LastPlan() const
	{
		return plan;
	}
	const Air& AirState() const
	{
		return air;
	}
	/// The frame, metres, as of the last frame.
	double FrameWidthMetres() const
	{
		return frameWidth;
	}
	double FrameHeightMetres() const
	{
		return frameHeight;
	}

private:
	void UpdateClock();
	void UpdateAudio( double dt );
	bool ensureBuffers( GLsizei width, GLsizei height );
	void uploadPlan();

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader abelShader;
	ffglex::FFGLShader fieldShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer table;   ///< R32F, kLutSize x kMaxPulses: P( rho ) per pulse
	PassBuffer pressure;///< R32F, ( w + 2 ) x ( h + 2 ): P on the frame
	GLuint profileTexture = 0;///< RG32F, kMaxVertices x kMaxPulses: the shells
	GLuint termTexture    = 0;///< RGBA32F, kMaxTerms x 1: the images
	float rows[ Air::kMaxPulses * 4 ] = {};

	Air air;
	Plan plan;
	audio::Analyser analyser;

	double frameWidth  = 6.0;
	double frameHeight = 3.375;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;

	//-------------------------------------------------------------------
	// The beat, as SetBeatInfo last said.
	//-------------------------------------------------------------------
	float bpm          = 120.0f;
	float barPhase     = 0.0f;
	float lastBarPhase = -1.0f;
	bool hostBeatSeen  = false;

	//-------------------------------------------------------------------
	// Presses.
	//-------------------------------------------------------------------
	int firePresses   = 0;
	bool fireHeld     = false;
	bool clearWanted  = false;
	bool clearHeld    = false;

	int hooks             = 0;
	double reflectionSign = 1.0;
	int shapeOverride     = -1;
	bool resizeClears     = false;
	GLsizei lastWidth     = 0;
	GLsizei lastHeight    = 0;
};

} // namespace schlieren
