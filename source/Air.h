#pragma once

#include "Acoustics.h"
#include "Controls.h"

#include <deque>
#include <vector>

/**
    The pulses in flight: when they are fired, and what the GPU is given of
    them each frame. Plain C++, no GL, so `sztest --offline` checks it.

    **Two clocks.** The beats arrive on the host's clock, in real seconds. The
    air runs on the camera's: Slow Motion times slower. A pulse is stamped
    with the AIR time it left the cone, so changing Slow Motion mid-flight
    changes how fast every ring moves from then on and moves none of them.

    **A beat inside a frame.** The host's bar phase is read every frame; a
    beat boundary crossed between two frames fires a pulse at the moment
    inside the frame where the phase crossed it, interpolated -- so a ring's
    radius is right to the sample, not to the frame (fiche's lesson). A press
    of Fire and an onset carry no such moment and are placed at the frame's
    start, so they show on the frame that saw them.

    **What the GPU is given.** Each pulse becomes a Shell -- its waveform in
    radius, simplified -- and the speaker's images become Terms that point at
    it. A term is sent only if it can still change the picture: its shell
    reaches the frame, or the still air inside it bends the light by more than
    a thousandth of what Sensitivity (or a pixel at Distance) would show. The
    strongest kMaxTerms are kept.
*/
namespace schlieren
{
struct Pulse
{
	double emitted = 0.0;///< air seconds
	double x = 0.0, y = 0.0;///< metres, frame coordinates, y up: where the speaker was
	double surfacePressure = 0.0;///< P_a, Pa
	double db = 0.0;///< the level it was fired at, dB SPL at 1 m
	acoustics::Shape shape = acoustics::Shape::Pop;
	double length = 0.5e-3;///< s

	/// The steepened waveform, simplified, and the age it was steepened at.
	/// Recomputed when the age has moved by more than a fifth of a percent.
	acoustics::Profile profile;
	double profileAge = -1.0;
};

/// One image of the speaker, pointing at a pulse's shell.
struct Term
{
	float x, y;     ///< metres
	float amplitude;///< reflection^order, signed
	int row;        ///< which pulse
};

/// What one frame sends to the GPU.
struct Plan
{
	std::vector< acoustics::Shell > shells;///< one per pulse, in pulse order (empty: nothing out yet)
	std::vector< Term > terms;
	int dropped = 0;///< terms over kMaxTerms, weakest first
};

/// What a frame knows about the beat, the audio and the controls.
struct FireInputs
{
	FireOn fireOn   = FireOn::Beat;
	int presses     = 0;
	bool onset      = false;
	double kick     = 0.0;  ///< the analyser's latched hit level, 0..1
	double level    = 0.0;  ///< its running level, 0..1
	bool hasSignal  = false;///< any audio routed at all
	double barPhase = 0.0;  ///< 0..1
	double bpm      = 120.0;
	bool hostBeat   = false;///< the host has been moving barPhase
	double loudness = 165.0;///< dB SPL at 1 m
	double accent   = 0.0;  ///< dB
	acoustics::Shape shape = acoustics::Shape::Pop;
	double length = 0.5e-3;
	double x = 0.0, y = 0.0;///< the speaker, metres
};

/// Hooks for the negative controls. A shipped instance sets 0.
enum AirHook : int
{
	kHookBeatAtFrameStart = 1 << 0,///< a beat fires at the start of its frame, not where it fell
	kHookSoundSpeed       = 1 << 1,///< sound 2% faster than it is
	kHookColdAccent       = 1 << 2 ///< Accent ignores whether any audio is routed
};

class Air
{
public:
	static constexpr int kMaxPulses = 24;
	static constexpr int kMaxTerms  = 128;
	/// Samples Steepen() is given.
	static constexpr int kSamples = 2048;

	/// Forget every pulse. The clocks carry on.
	void Clear();

	/// One host frame: `now` and `dt` in host seconds, `slow` the slow-motion
	/// factor. Fires whatever is due, then advances the air.
	void Advance( double now, double dt, double slow, const FireInputs& in );

	/// The shells and terms for the picture this frame, and drop the pulses
	/// that can no longer reach it. `visibleSlope` is the smallest deflection
	/// that would show, radians.
	Plan Build( acoustics::Walls walls, double reflection, double width, double height, double visibleSlope );

	const std::deque< Pulse >& Pulses() const
	{
		return pulses;
	}
	double Time() const
	{
		return airTime;
	}
	/// Pulses fired since the plugin loaded.
	unsigned long long Fired() const
	{
		return fired;
	}

	/// The speed of sound the air runs at (the harness's hook can move it).
	double SoundSpeed() const;

	/// The radius of a pulse's shell centre, where it is steepened.
	static double CentreOf( const Pulse& pulse, double airNow, double soundSpeed );

	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}

private:
	void fire( double at, const FireInputs& in, double level );

	std::deque< Pulse > pulses;
	double airTime   = 0.0;
	double lastPhase = -1.0;
	unsigned long long fired = 0;
	int hooks = 0;
};

} // namespace schlieren
