#include "Acoustics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace schlieren::acoustics
{
namespace
{
//---------------------------------------------------------------------------
// The shapes, in x = theta / Length, each with its support in x.
//---------------------------------------------------------------------------

/// Pop: f = -u exp( 1/2 - u^2 / 2 ), u = ( theta - centre ) / sigma, sigma =
/// Length / 2, centred five sigmas in. The Gaussian's tails are not zero at
/// +-5 sigma (3e-5), so the line between the two end values is taken off:
/// the ends become exactly 0 and, because the two ends are equal and
/// opposite, the impulse stays exactly 0.
constexpr double kPopSpan = 5.0;///< support, in Lengths (ten sigmas)
double popRaw( double x )
{
	const double u = 2.0 * x - kPopSpan;//( theta - 5 sigma ) / sigma
	return -u * std::exp( 0.5 - 0.5 * u * u );
}
double pop( double x )
{
	const double start = popRaw( 0.0 ), end = popRaw( kPopSpan );
	return popRaw( x ) - ( start + ( end - start ) * x / kPopSpan );
}

/// Kick: the cone's velocity is v = sin( pi x ) ( 1 - e^( -x / 0.25 ) ) e^( -x / 2 )
/// -- a sine of period 2 Lengths, faded in over a quarter Length and out over
/// two -- and the pressure is its derivative. v starts at rest with zero
/// slope, so the pressure starts at 0 with no jump; v ends at rest, so the
/// impulse is 0. Normalised by its peak, found once.
constexpr double kKickSpan = 24.0;///< twelve fade-outs: e^-12
constexpr double kKickRise = 0.25;
constexpr double kKickFall = 2.0;
double kickRaw( double x )
{
	const double rise = std::exp( -x / kKickRise ), fall = std::exp( -x / kKickFall );
	const double g      = ( 1.0 - rise ) * fall;
	const double gSlope = rise / kKickRise * fall - ( 1.0 - rise ) * fall / kKickFall;
	return kPi * std::cos( kPi * x ) * g + std::sin( kPi * x ) * gSlope;
}
double kickPeak()
{
	static const double peak = [] {
		double best = 0.0;
		for( int i = 0; i <= 240000; ++i )
			best = std::max( best, std::fabs( kickRaw( kKickSpan * i / 240000.0 ) ) );
		return best;
	}();
	return peak;
}

/// Blast: Friedlander with b = 1, f = ( 1 - u ) e^-u, u = theta / t+, t+ =
/// Length / 2. Its impulse is exactly 0 (b = 1 is the one decay that makes
/// it so), and it starts with a jump: a blast wave arrives as a shock.
constexpr double kBlastSpan = 10.0;///< twenty t+: e^-20

double shapeSpan( Shape shape )
{
	switch( shape )
	{
	case Shape::Pop: return kPopSpan;
	case Shape::Kick: return kKickSpan;
	case Shape::Blast: return kBlastSpan;
	case Shape::Box:
	case Shape::NWave: return 1.0;
	default: return 1.0;
	}
}

bool jumpAtStart( Shape shape )
{
	return shape == Shape::Blast || shape == Shape::Box || shape == Shape::NWave;
}
bool jumpAtEnd( Shape shape )
{
	return shape == Shape::Box || shape == Shape::NWave;
}

/// The shape in units of x = theta / Length, on its closed support.
double shapeAt( Shape shape, double x )
{
	switch( shape )
	{
	case Shape::Pop: return pop( x );
	case Shape::Kick: return kickRaw( x ) / kickPeak();
	case Shape::Blast:
	{
		const double u = 2.0 * x;
		return ( 1.0 - u ) * std::exp( -u );
	}
	case Shape::Box: return 1.0;
	case Shape::NWave: return 1.0 - 2.0 * x;
	default: return 0.0;
	}
}

/// Gauss-Legendre, 16 points on [ -1, 1 ].
constexpr double kGaussX[ 8 ] = { 0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
	                              0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499 };
constexpr double kGaussW[ 8 ] = { 0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
	                              0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541 };

/// integral of ( alpha / r + beta ) dz over [ a, b ], r = sqrt( rho^2 + z^2 ).
double gaussPiece( double alpha, double beta, double rho, double a, double b )
{
	const double half = 0.5 * ( b - a ), mid = 0.5 * ( a + b );
	double sum = 0.0;
	for( int i = 0; i < 8; ++i )
		for( int sign = -1; sign <= 1; sign += 2 )
		{
			const double z = mid + sign * half * kGaussX[ i ];
			const double r = std::sqrt( rho * rho + z * z );
			sum += kGaussW[ i ] * ( alpha / r + beta );
		}
	return sum * half;
}

/// sqrt( r^2 - rho^2 ) without the cancellation, for r >= rho.
double chord( double r, double rho )
{
	return std::sqrt( std::max( 0.0, ( r - rho ) * ( r + rho ) ) );
}
} // namespace

//---------------------------------------------------------------------------
double PressureAtMetre( double db )
{
	return kReferencePressure * std::pow( 10.0, db / 20.0 );
}

double SurfacePressure( double db )
{
	//Spherical spreading back in from 1 m to the cone.
	return PressureAtMetre( db ) * ( 1.0 / kSpeakerRadius );
}

double Waveform( Shape shape, double length, double theta )
{
	if( !( length > 0.0 ) )
		return 0.0;
	const double x = theta / length, span = shapeSpan( shape );
	if( x < 0.0 || x > span )
		return 0.0;
	return shapeAt( shape, x );
}

double Support( Shape shape, double length )
{
	return shapeSpan( shape ) * length;
}

double MaxRise( Shape shape, double length )
{
	//Differences over the open support: a jump at either end is a shock
	//already, not a slope that will become one.
	const double span = shapeSpan( shape );
	constexpr int kSteps = 200000;
	double best = 0.0;
	for( int i = 1; i < kSteps - 1; ++i )
	{
		const double x0 = span * i / kSteps, x1 = span * ( i + 1 ) / kSteps;
		best = std::max( best, ( shapeAt( shape, x1 ) - shapeAt( shape, x0 ) ) / ( x1 - x0 ) );
	}
	return best / length;
}

double Centroid( Shape shape, double length )
{
	auto measure = []( Shape which ) {
		const double span = shapeSpan( which );
		double weight = 0.0, moment = 0.0;
		constexpr int kSteps = 100000;
		for( int i = 0; i < kSteps; ++i )
		{
			const double x = span * ( i + 0.5 ) / kSteps, f = std::fabs( shapeAt( which, x ) );
			weight += f;
			moment += f * x;
		}
		return weight > 0.0 ? moment / weight : 0.0;
	};
	static const double pop = measure( Shape::Pop ), kick = measure( Shape::Kick ), blast = measure( Shape::Blast ),
	                    box = measure( Shape::Box ), nwave = measure( Shape::NWave );
	switch( shape )
	{
	case Shape::Pop: return pop * length;
	case Shape::Kick: return kick * length;
	case Shape::Blast: return blast * length;
	case Shape::Box: return box * length;
	case Shape::NWave: return nwave * length;
	default: return 0.0;
	}
}

double AgeAt( double radius )
{
	return radius > kSpeakerRadius ? kSpeakerRadius * std::log( radius / kSpeakerRadius ) : 0.0;
}

//---------------------------------------------------------------------------
// Steepening.
//
// In Y = -theta and w = kappa P_a f0( -Y ) the weak-shock equation is the
// inviscid Burgers equation w_s + w w_X = 0, X = -tau: a part of the wave at
// w moves AHEAD by w per metre of age. Its entropy solution is the
// Lax-Oleinik formula
//
//     w( X, s ) = ( X - y* ) / s,   y* = argmin_y [ s W( y ) + y^2 / 2 - X y ],
//
// W the running integral of w. The minimiser over the samples is a vertex of
// the lower convex hull of H = s W + Y^2 / 2: while the wave is smooth every
// sample is on it (H'' = 1 + s w' > 0) and the solution is Poisson's,
// X = Y + s w, exact for the piecewise-linear w; once it folds, the samples
// under a shock lie above the hull, and the hull edge that bridges them has
// slope X_s, the shock's position, with equal areas cut off either side --
// which is the equal-area rule, not an approximation to it.
//---------------------------------------------------------------------------
Profile Steepen( Shape shape, double length, double surfacePressure, double age, int samples, int hooks )
{
	Profile out;
	const int n       = std::max( samples, 16 );
	const double span = Support( shape, length );
	const double k    = kNonlinearity * std::max( surfacePressure, 0.0 );
	const double s    = std::max( age, 0.0 );
	const double most = k * s;//the biggest shift, seconds (peak |f0| is 1)

	if( !( span > 0.0 ) )
		return out;

	if( most < 1e-3 * span / ( n - 1 ) )
	{
		//Linear: the waveform itself, its jumps kept as jumps.
		if( jumpAtStart( shape ) )
			out.vertices.push_back( { 0.0, 0.0 } );
		for( int i = 0; i < n; ++i )
		{
			const double theta = span * i / ( n - 1 );
			out.vertices.push_back( { theta, Waveform( shape, length, theta ) } );
		}
		if( jumpAtEnd( shape ) )
			out.vertices.push_back( { span, 0.0 } );
		return out;
	}

	//Room either side for the wave to move into: it moves by at most `most`.
	//The grid is shifted to put a sample exactly where the cone first moves,
	//so a pulse's front is where it is and not up to a sample ahead of it.
	const double pad   = 1.05 * most + 4.0 * span / ( n - 1 );
	const double step  = ( span + 2.0 * pad ) / ( n - 1 );
	const double first = -std::ceil( pad / step ) * step;

	std::vector< double > Y( static_cast< size_t >( n ) ), w( Y.size() ), H( Y.size() );
	for( int j = 0; j < n; ++j )
	{
		const double theta = first + ( n - 1 - j ) * step;
		Y[ j ]             = -theta;
		w[ j ]             = k * Waveform( shape, length, theta );
	}
	double W = 0.0;
	for( int j = 0; j < n; ++j )
	{
		if( j > 0 )
			W += 0.5 * ( w[ j ] + w[ j - 1 ] ) * step;
		H[ j ] = s * W + 0.5 * Y[ j ] * Y[ j ];
	}

	std::vector< Vertex > line;//( X, w ), X ascending
	if( ( hooks & kHookNoEntropy ) != 0 )
	{
		//The negative control: Poisson's solution, folded, read in order.
		for( int j = 0; j < n; ++j )
			line.push_back( { Y[ j ] + s * w[ j ], w[ j ] } );
	}
	else
	{
		//The lower hull, Andrew's monotone chain: Y is already sorted.
		std::vector< int > hull;
		hull.reserve( Y.size() );
		for( int j = 0; j < n; ++j )
		{
			while( hull.size() >= 2 )
			{
				const int a = hull[ hull.size() - 2 ], b = hull.back();
				const double cross = ( Y[ b ] - Y[ a ] ) * ( H[ j ] - H[ a ] ) - ( H[ b ] - H[ a ] ) * ( Y[ j ] - Y[ a ] );
				if( cross > 0.0 )
					break;
				hull.pop_back();
			}
			hull.push_back( j );
		}

		//Each hull vertex v contributes its characteristic ( Y + s w, w ); a gap
		//in the hull is a shock at the bridging edge's slope. The state either
		//side of it is NOT the gap's end vertex's: the true foot of the shock
		//is where H's tangent touches it, generally inside a cell, and taking
		//the vertex instead costs up to a cell / s -- most of the jump, while
		//the steepening is only a few cells old. The state is read where the
		//characteristic map X( Y ) = Y + s w( Y ), exact and linear on each
		//cell, crosses the shock: walking in from the left foot for the left
		//state, and from the right foot for the right. A foot whose
		//characteristic has already run past the shock (a fan's, say) is cut
		//back to where the active polyline crosses it.
		std::vector< double > Xc( Y.size() );
		for( int j = 0; j < n; ++j )
			Xc[ j ] = Y[ j ] + s * w[ j ];
		auto crossing = [ & ]( int j0, int j1, double at ) {//w where cell j0 -> j1 meets X = at
			return Xc[ j1 ] != Xc[ j0 ] ? w[ j0 ] + ( w[ j1 ] - w[ j0 ] ) * ( at - Xc[ j0 ] ) / ( Xc[ j1 ] - Xc[ j0 ] ) : w[ j0 ];
		};

		const size_t count = hull.size();
		size_t index       = 0;
		while( index < count )
		{
			const int v = hull[ index ];
			line.push_back( { Xc[ v ], w[ v ] } );
			if( index + 1 == count || hull[ index + 1 ] == v + 1 )
			{
				++index;
				continue;
			}

			//A gap in the hull: the samples between are under a shock.
			const int b    = hull[ index + 1 ];
			double shockAt = ( H[ b ] - H[ v ] ) / ( Y[ b ] - Y[ v ] );
			if( ( hooks & kHookShockAtVertex ) != 0 )
				shockAt = 0.5 * ( Xc[ v ] + Xc[ b ] );

			double left = ( shockAt - Y[ v ] ) / s;
			if( line.back().x > shockAt )
			{
				Vertex beyond = line.back();
				line.pop_back();
				while( !line.empty() && line.back().x > shockAt )
				{
					beyond = line.back();
					line.pop_back();
				}
				if( !line.empty() && beyond.x > line.back().x )
					left = line.back().f + ( beyond.f - line.back().f ) * ( shockAt - line.back().x ) / ( beyond.x - line.back().x );
			}
			else
				for( int j = v; j < b; ++j )
					if( Xc[ j ] <= shockAt && shockAt <= Xc[ j + 1 ] )
					{
						left = crossing( j, j + 1, shockAt );
						break;
					}

			double right = ( shockAt - Y[ b ] ) / s;
			size_t next  = index + 1;
			if( Xc[ b ] < shockAt )
			{
				//Still behind the shock: skip on along the hull to where it crosses.
				while( next + 1 < count && hull[ next + 1 ] == hull[ next ] + 1 && Xc[ hull[ next + 1 ] ] < shockAt )
					++next;
				if( next + 1 < count && hull[ next + 1 ] == hull[ next ] + 1 )
				{
					right = crossing( hull[ next ], hull[ next + 1 ], shockAt );
					++next;
				}
			}
			else
				for( int j = b; j > v; --j )
					if( Xc[ j - 1 ] <= shockAt && shockAt <= Xc[ j ] )
					{
						right = crossing( j - 1, j, shockAt );
						break;
					}

			line.push_back( { shockAt, left } );
			line.push_back( { shockAt, right } );
			if( std::fabs( left - right ) > 1e-6 * k )
				++out.shocks;
			index = next;
		}
		for( size_t i = 1; i < line.size(); ++i )
			line[ i ].x = std::max( line[ i ].x, line[ i - 1 ].x );
	}

	//Back to retarded time, ascending, and to f.
	out.vertices.reserve( line.size() );
	for( size_t i = line.size(); i-- > 0; )
		out.vertices.push_back( { -line[ i ].x, line[ i ].f / k } );
	return out;
}

//---------------------------------------------------------------------------
double ValueAt( const std::vector< Vertex >& line, double x )
{
	if( line.empty() || x < line.front().x || x > line.back().x )
		return 0.0;
	//The last vertex at or before x: at a jump, the later one wins.
	const auto after = std::upper_bound( line.begin(), line.end(), x, []( double value, const Vertex& v ) { return value < v.x; } );
	if( after == line.begin() )
		return line.front().f;
	const Vertex& a = *( after - 1 );
	if( after == line.end() || after->x <= a.x )
		return a.f;
	const Vertex& b = *after;
	return a.f + ( b.f - a.f ) * ( x - a.x ) / ( b.x - a.x );
}

std::vector< Vertex > Simplify( const std::vector< Vertex >& line, double tolerance )
{
	if( line.size() <= 2 )
		return line;

	std::vector< char > keep( line.size(), 0 );
	keep.front() = keep.back() = 1;
	//A jump's two vertices are always kept: the runs between jumps are
	//simplified on their own, so nothing smooths a shock. So is the edge of
	//the silence either side: a chord from the padding to the first sound
	//would put pressure AHEAD of the wavefront -- a hair of it, inside the
	//tolerance, but out where nothing has arrived yet (--speed found it).
	for( size_t i = 0; i + 1 < line.size(); ++i )
	{
		if( line[ i + 1 ].x <= line[ i ].x )
			keep[ i ] = keep[ i + 1 ] = 1;
		if( ( line[ i ].f == 0.0 ) != ( line[ i + 1 ].f == 0.0 ) )
			keep[ i ] = keep[ i + 1 ] = 1;
	}

	std::vector< std::pair< size_t, size_t > > stack;
	size_t start = 0;
	for( size_t i = 1; i < line.size(); ++i )
		if( keep[ i ] )
		{
			if( i > start + 1 )
				stack.emplace_back( start, i );
			start = i;
		}
	while( !stack.empty() )
	{
		const auto [ a, b ] = stack.back();
		stack.pop_back();
		const double x0 = line[ a ].x, f0 = line[ a ].f, x1 = line[ b ].x, f1 = line[ b ].f;
		double worst  = -1.0;
		size_t where  = a;
		for( size_t i = a + 1; i < b; ++i )
		{
			const double chordValue = x1 > x0 ? f0 + ( f1 - f0 ) * ( line[ i ].x - x0 ) / ( x1 - x0 ) : f0;
			const double off        = std::fabs( line[ i ].f - chordValue );
			if( off > worst )
			{
				worst = off;
				where = i;
			}
		}
		if( worst > tolerance )
		{
			keep[ where ] = 1;
			if( where > a + 1 )
				stack.emplace_back( a, where );
			if( b > where + 1 )
				stack.emplace_back( where, b );
		}
	}

	std::vector< Vertex > out;
	for( size_t i = 0; i < line.size(); ++i )
		if( keep[ i ] )
			out.push_back( line[ i ] );
	return out;
}

//---------------------------------------------------------------------------
Shell ShellAt( const Profile& profile, double elapsed, double surfacePressure, double tolerance, double soundSpeed )
{
	Shell shell;
	const std::vector< Vertex >& v = profile.vertices;
	if( v.empty() || !( elapsed > 0.0 ) )
		return shell;

	//What has left the cone: tau <= elapsed. The rest is cut at r = a.
	std::vector< Vertex > emitted;
	for( const Vertex& vertex : v )
	{
		if( vertex.x <= elapsed )
		{
			emitted.push_back( vertex );
			continue;
		}
		if( !emitted.empty() && emitted.back().x < elapsed )
		{
			const Vertex& a = emitted.back();
			emitted.push_back( { elapsed, a.f + ( vertex.f - a.f ) * ( elapsed - a.x ) / ( vertex.x - a.x ) } );
		}
		break;
	}

	//Trim the silence either side, keeping one zero vertex as the foot of
	//the ramp (or the low side of the jump) at each end.
	double peak = 0.0;
	for( const Vertex& vertex : emitted )
		peak = std::max( peak, std::fabs( vertex.f ) );
	if( !( peak > 0.0 ) )
		return shell;
	const double quiet = 1e-12 * peak;
	size_t lo = 0, hi = emitted.size();
	while( lo < emitted.size() && std::fabs( emitted[ lo ].f ) <= quiet )
		++lo;
	while( hi > lo && std::fabs( emitted[ hi - 1 ].f ) <= quiet )
		--hi;
	if( lo > 0 )
		--lo;
	if( hi < emitted.size() )
		++hi;

	//Into radius, ascending: the earliest retarded time is the farthest out.
	std::vector< Vertex > radial;
	radial.reserve( hi - lo );
	for( size_t i = hi; i-- > lo; )
		radial.push_back( { kSpeakerRadius + soundSpeed * ( elapsed - emitted[ i ].x ), emitted[ i ].f } );

	double tol = tolerance * peak;
	shell.vertices = Simplify( radial, tol );
	//Jumps and the edges of silence are always kept, so past some count no
	//tolerance will do: give up doubling at the peak itself and thin evenly.
	//(A mutant with its radii reversed made every vertex a "jump" and looped
	//here for ever -- in the host's render thread.)
	while( static_cast< int >( shell.vertices.size() ) > kMaxVertices && tol < peak )
	{
		tol *= 2.0;
		shell.vertices = Simplify( radial, tol );
	}
	if( static_cast< int >( shell.vertices.size() ) > kMaxVertices )
	{
		std::vector< Vertex > thinned;
		const size_t last = shell.vertices.size() - 1;
		for( int i = 0; i < kMaxVertices; ++i )
			thinned.push_back( shell.vertices[ static_cast< size_t >( std::llround( static_cast< double >( i ) * last / ( kMaxVertices - 1 ) ) ) ] );
		shell.vertices.swap( thinned );
	}

	shell.inner = shell.vertices.front().x;
	shell.outer = shell.vertices.back().x;
	shell.gain  = 2.0 * kSpeakerRadius * surfacePressure;
	shell.peak  = peak;
	for( size_t i = 0; i + 1 < shell.vertices.size(); ++i )
	{
		const Vertex& a = shell.vertices[ i ];
		const Vertex& b = shell.vertices[ i + 1 ];
		const double dr = b.x - a.x;
		if( dr <= 0.0 )
			continue;
		if( a.f * b.f >= 0.0 )
			shell.impulse += 0.5 * ( std::fabs( a.f ) + std::fabs( b.f ) ) * dr;
		else
		{
			const double t = a.f / ( a.f - b.f );
			shell.impulse += 0.5 * ( std::fabs( a.f ) * t + std::fabs( b.f ) * ( 1.0 - t ) ) * dr;
		}
	}
	return shell;
}

//---------------------------------------------------------------------------
double ProjectedReference( const Shell& shell, double rho, double metrePressure )
{
	const std::vector< Vertex >& v = shell.vertices;
	if( v.size() < 2 || rho >= shell.outer )
		return 0.0;

	double sum = 0.0;
	for( size_t i = 0; i + 1 < v.size(); ++i )
	{
		const double r0 = v[ i ].x, r1 = v[ i + 1 ].x;
		if( r1 <= r0 || r1 <= rho )
			continue;
		const double beta  = ( v[ i + 1 ].f - v[ i ].f ) / ( r1 - r0 );
		const double alpha = v[ i ].f - beta * r0;
		double z           = chord( std::max( r0, rho ), rho );
		const double end   = chord( r1, rho );
		//1/r is analytic in z with poles at +-i rho, so a piece no longer
		//than its distance from them converges like any smooth integrand.
		while( z < end )
		{
			const double next = std::min( end, z + std::max( { rho, z, 1e-9 } ) );
			sum += gaussPiece( alpha, beta, rho, z, next );
			z = next;
		}
	}
	//The line of sight crosses each radius twice: in front of the speaker
	//and behind it.
	return metrePressure >= 0.0 ? 2.0 * metrePressure * 1.0 * sum : shell.gain * sum;
}

double InteriorSlopeBound( const Shell& shell, double rho )
{
	if( rho >= shell.inner )
		return std::numeric_limits< double >::infinity();
	const double gap = ( shell.inner - rho ) * ( shell.inner + rho );
	return shell.gain * rho * shell.impulse / ( gap * std::sqrt( gap ) );
}

//---------------------------------------------------------------------------
std::vector< Image > Images( Walls walls, double reflection, double x, double y, double width, double height, int maxOrder )
{
	std::vector< Image > images;
	images.push_back( { x, y, 1.0, 0 } );
	if( walls == Walls::Floor )
		images.push_back( { x, -y, reflection, 1 } );
	else if( walls == Walls::Box )
	{
		//Between mirrors at 0 and L, a point's images are 2mL + x (reflected an
		//even number of times) and 2mL - x (an odd number). Index i walks both:
		//even i is i L + x, odd i is ( i + 1 ) L - x, and |i| is the order.
		auto along = []( int i, double p, double side ) {
			return ( i % 2 == 0 ) ? i * side + p : ( i + 1 ) * side - p;
		};
		for( int i = -maxOrder; i <= maxOrder; ++i )
			for( int j = -maxOrder; j <= maxOrder; ++j )
			{
				if( i == 0 && j == 0 )
					continue;
				const int order = std::abs( i ) + std::abs( j );
				images.push_back( { along( i, x, width ), along( j, y, height ), std::pow( reflection, order ), order } );
			}
	}
	return images;
}

void FrameDistances( double x, double y, double width, double height, double& nearest, double& farthest )
{
	const double dx = std::max( { 0.0, -x, x - width } );
	const double dy = std::max( { 0.0, -y, y - height } );
	nearest         = std::sqrt( dx * dx + dy * dy );
	const double fx = std::max( std::fabs( x ), std::fabs( x - width ) );
	const double fy = std::max( std::fabs( y ), std::fabs( y - height ) );
	farthest        = std::sqrt( fx * fx + fy * fy );
}

} // namespace schlieren::acoustics
