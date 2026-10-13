#include "Air.h"

#include <algorithm>
#include <cmath>

namespace schlieren
{
using namespace acoustics;

namespace
{
/// The most beat boundaries one frame may fire. A frame that crossed more
/// was a seek or a stall, not music.
constexpr int kMaxBeatsPerFrame = 8;

/// An image this much weaker than the speaker (reflection^order) is not
/// waited for: a pulse with nothing nearer to come is let go.
constexpr double kQuietImage = 1e-3;

/// A pulse is let go once its shell is this many frame diagonals out, if
/// nothing else has let it go: a rigid box would otherwise ring for ever.
constexpr double kFarthest = 50.0;

/// The fraction of a just-visible deflection below which a term is not sent.
constexpr double kInvisible = 1e-3;
} // namespace

void Air::Clear()
{
	pulses.clear();
}

double Air::SoundSpeed() const
{
	return ( hooks & kHookSoundSpeed ) != 0 ? kSoundSpeed * 1.02 : kSoundSpeed;
}

double Air::CentreOf( const Pulse& pulse, double airNow, double soundSpeed )
{
	return kSpeakerRadius + soundSpeed * std::max( 0.0, airNow - pulse.emitted - Centroid( pulse.shape, pulse.length ) );
}

void Air::fire( double at, const FireInputs& in, double level )
{
	Pulse pulse;
	pulse.emitted = at;
	pulse.x       = in.x;
	pulse.y       = in.y;
	//Accent sets how far below Loudness the softest hit lands. With no audio
	//routed there is nothing to accent by, and every pulse is at Loudness --
	//rather than every pulse being as quiet as silence would make it.
	const bool accented = in.hasSignal || ( hooks & kHookColdAccent ) != 0;
	pulse.db = in.loudness - ( accented ? in.accent * ( 1.0 - std::clamp( level, 0.0, 1.0 ) ) : 0.0 );
	pulse.surfacePressure = SurfacePressure( pulse.db );
	pulse.shape           = in.shape;
	pulse.length          = in.length;
	pulses.push_back( pulse );
	++fired;
	while( static_cast< int >( pulses.size() ) > kMaxPulses )
		pulses.pop_front();
}

void Air::Advance( double now, double dt, double slow, const FireInputs& in )
{
	const double airDt = dt > 0.0 && slow > 0.0 ? dt / slow : 0.0;
	const double start = airTime;

	//The button and the onsets, at the frame's start.
	for( int i = 0; i < in.presses; ++i )
		fire( start, in, 1.0 );
	if( in.fireOn == FireOn::Onset && in.onset )
		fire( start, in, in.kick );

	//The beat. A host that never moves barPhase gets a clock of the plugin's
	//own at its BPM, so Fire On is never dead.
	const double tempo    = in.bpm > 1.0 ? in.bpm : 120.0;
	const double phaseNow = in.hostBeat ? std::clamp( in.barPhase, 0.0, 1.0 ) : std::fmod( now * tempo / 240.0, 1.0 );
	double step           = 0.0;
	switch( in.fireOn )
	{
	case FireOn::Beat: step = 0.25; break;
	case FireOn::HalfBeat: step = 0.125; break;
	case FireOn::TwoBeats: step = 0.5; break;
	case FireOn::Bar: step = 1.0; break;
	default: break;
	}
	if( step > 0.0 && lastPhase >= 0.0 && dt > 0.0 )
	{
		double p0 = lastPhase, p1 = phaseNow;
		if( p1 < p0 )
			p1 += 1.0;
		if( p1 > p0 )
		{
			double boundary = ( std::floor( p0 / step + 1e-9 ) + 1.0 ) * step;
			for( int n = 0; boundary <= p1 + 1e-12 && n < kMaxBeatsPerFrame; ++n, boundary += step )
			{
				const double inside = ( hooks & kHookBeatAtFrameStart ) != 0 ? 0.0 : ( boundary - p0 ) / ( p1 - p0 );
				fire( start + inside * airDt, in, in.level );
			}
		}
	}
	lastPhase = phaseNow;

	airTime += airDt;
}

Plan Air::Build( Walls walls, double reflection, double width, double height, double visibleSlope )
{
	Plan plan;
	const double c        = SoundSpeed();
	const double diagonal = std::sqrt( width * width + height * height );
	const double floor    = kInvisible * visibleSlope;

	struct Candidate
	{
		Term term;
		double strength;
	};
	std::vector< Candidate > candidates;
	std::vector< char > spent( pulses.size(), 0 );

	plan.shells.resize( pulses.size() );
	for( size_t row = 0; row < pulses.size(); ++row )
	{
		Pulse& pulse          = pulses[ row ];
		const double elapsed  = airTime - pulse.emitted;
		if( !( elapsed > 0.0 ) )
			continue;

		//Steepened at the radius its centre has reached. The age moves as a
		//log, so a fifth of a percent is invisible and saves the work.
		const double age = AgeAt( CentreOf( pulse, airTime, c ) );
		if( pulse.profileAge < 0.0 || std::fabs( age - pulse.profileAge ) > 2e-3 * std::max( pulse.profileAge, 1e-6 ) )
		{
			Profile fresh       = Steepen( pulse.shape, pulse.length, pulse.surfacePressure, age, kSamples );
			pulse.profile.shocks = fresh.shocks;
			pulse.profile.vertices = Simplify( fresh.vertices, 1e-4 );
			pulse.profileAge       = age;
		}

		const Shell shell = ShellAt( pulse.profile, elapsed, pulse.surfacePressure, 2e-4, c );
		plan.shells[ row ] = shell;
		if( shell.vertices.size() < 2 )
			continue;

		bool pending = false, any = false;
		for( const Image& image : Images( walls, reflection, pulse.x, pulse.y, width, height ) )
		{
			const double weight = std::fabs( image.amplitude );
			if( weight <= 0.0 )
				continue;
			double nearest = 0.0, farthest = 0.0;
			FrameDistances( image.x, image.y, width, height, nearest, farthest );
			if( nearest >= shell.outer )
			{
				//Still on its way to the frame.
				if( weight >= kQuietImage )
					pending = true;
				continue;
			}
			if( farthest < shell.inner )
			{
				//The whole frame is in the still air inside the shell, where
				//all that is left is the projection of the far side of it.
				if( InteriorSlopeBound( shell, farthest ) * weight * kIndexPerPascal < floor )
					continue;
			}
			any = true;
			Candidate candidate;
			candidate.term     = { static_cast< float >( image.x ), static_cast< float >( image.y ),
                               static_cast< float >( image.amplitude ), static_cast< int >( row ) };
			candidate.strength = weight * shell.gain * shell.peak / std::sqrt( std::max( shell.outer, 1e-3 ) );
			candidates.push_back( candidate );
		}
		if( ( !any && !pending ) || shell.inner > kFarthest * diagonal )
			spent[ row ] = 1;
	}

	std::stable_sort( candidates.begin(), candidates.end(),
	                  []( const Candidate& a, const Candidate& b ) { return a.strength > b.strength; } );
	if( static_cast< int >( candidates.size() ) > kMaxTerms )
	{
		plan.dropped = static_cast< int >( candidates.size() ) - kMaxTerms;
		candidates.resize( kMaxTerms );
	}
	for( const Candidate& candidate : candidates )
		plan.terms.push_back( candidate.term );

	//Let go of what can no longer reach the picture, and renumber the rows.
	if( std::find( spent.begin(), spent.end(), 1 ) != spent.end() )
	{
		std::vector< int > remap( pulses.size(), -1 );
		std::deque< Pulse > kept;
		std::vector< Shell > shells;
		for( size_t row = 0; row < pulses.size(); ++row )
			if( !spent[ row ] )
			{
				remap[ row ] = static_cast< int >( kept.size() );
				kept.push_back( pulses[ row ] );
				shells.push_back( plan.shells[ row ] );
			}
		pulses.swap( kept );
		plan.shells.swap( shells );
		std::vector< Term > terms;
		for( const Term& term : plan.terms )
			if( remap[ static_cast< size_t >( term.row ) ] >= 0 )
			{
				Term moved = term;
				moved.row  = remap[ static_cast< size_t >( term.row ) ];
				terms.push_back( moved );
			}
		plan.terms.swap( terms );
	}
	return plan;
}

} // namespace schlieren
