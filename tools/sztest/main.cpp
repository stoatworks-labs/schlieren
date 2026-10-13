/**
    sztest -- render Schlieren offline, and measure what the air and the
    optics are doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads back the
    host's output -- and the plugin's own pressure field and projection table
    -- as floats. What they are measured against is independent of the code
    that made them: a brute-force line-of-sight quadrature
    (acoustics::ProjectedReference), a Godunov solver for the shocks, closed
    forms where the physics has one, and the clock.

        sztest --out /tmp/schlieren.png   the defaults on the harness's card, at 120 BPM
        sztest --list                     every parameter and its default
        sztest --pipe                     raw RGBA frames in on stdin, out on stdout
        sztest --film N                   N frames of the card, raw RGBA on stdout
        sztest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option, an event or a position
    STEPS... a position ramps, like a standard control.

    SZTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Acoustics.h"
#include "Air.h"
#include "Audio.h"
#include "Controls.h"
#include "Schlieren.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace schlieren;
using namespace schlieren::acoustics;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// One float32 ULP at 1.0.
constexpr double kFloatEps = 1.0 / 8388608.0;

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "SZTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "sztest: SZTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_XPOS: return "x";
	case FF_TYPE_YPOS: return "y";
	case FF_TYPE_BUFFER: return "buffer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch or a press: cues STEP between
/// keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// The look card: a sunset sky, a sun, a skyline, a stripe of saturated bars
/// and a white title block -- edges, flats and colour for the air to bend.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			const double k = std::clamp( 0.5 + v, 0.0, 1.0 );
			double r = 0.95 - 0.8 * k, g = 0.45 - 0.3 * k, b = 0.25 + 0.5 * k;
			const double d = std::sqrt( ( u + 0.25 ) * ( u + 0.25 ) + ( v + 0.02 ) * ( v + 0.02 ) );
			if( d < 0.2 )
			{
				r = 1.0;
				g = 0.85 - 0.5 * d;
				b = 0.35;
			}
			const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 11.0 ) );
			const double top = -0.12 + 0.16 * hash01( static_cast< uint32_t >( block ), 7 );
			if( v < top )
			{
				r = g = b = 0.05;
				const int wx = static_cast< int >( std::floor( ( u + 2.0 ) * 70.0 ) ), wy = static_cast< int >( std::floor( v * 60.0 + 100.0 ) );
				if( ( wx % 3 ) != 0 && ( wy % 2 ) == 0 && hash01( static_cast< uint32_t >( wx * 131 + wy ), 9 ) > 0.55 )
				{
					r = 1.0;
					g = 0.9;
					b = 0.55;
				}
			}
			if( v < -0.36 )
			{
				const int bar               = std::clamp( static_cast< int >( std::floor( ( x + 0.5 ) / width * 7.0 ) ), 0, 6 );
				const double bars[ 7 ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
				r = 0.75 * bars[ bar ][ 0 ];
				g = 0.75 * bars[ bar ][ 1 ];
				b = 0.75 * bars[ bar ][ 2 ];
			}
			if( std::fabs( u - 0.42 ) < 0.28 && std::fabs( v - 0.3 ) < 0.06 )
				r = g = b = 0.95;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

Floats flatCard( int width, int height, float r, float g, float b )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = r;
		card[ i + 1 ] = g;
		card[ i + 2 ] = b;
		card[ i + 3 ] = 1.0f;
	}
	return card;
}

/// Each pixel's own centre, as a fraction of the frame: red across, green up.
/// Linear, so bilinear sampling reproduces it exactly between texels.
Floats coordinateCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( ( x + 0.5 ) / width );
			o[ 1 ]   = static_cast< float >( ( y + 0.5 ) / height );
			o[ 2 ]   = 0.5f;
			o[ 3 ]   = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is: the background check samples it
	//between texels.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// A whole R32F texture, read back.
Floats readRed( GLuint texture, int width, int height )
{
	Floats out( static_cast< size_t >( width ) * height );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, out.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return out;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	SchlierenPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 0.0;///< 0: no beat sent
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/schlieren for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( Seconds( frame ) );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Press Fire on the next frame and render it.
	bool Fire()
	{
		Set( PT_FIRE, 1.0f );
		const bool ok = Render( 1 );
		Set( PT_FIRE, 0.0f );
		return ok;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// P on the frame and its margin, ( w + 2 ) x ( h + 2 ), Pa m.
	Floats Pressure() const
	{
		return readRed( plugin.PressureTextureID(), width + 2, height + 2 );
	}

	/// The projection table, kLutSize x kMaxPulses.
	Floats Table() const
	{
		return readRed( plugin.TableTextureID(), shaders::kLutSize, Air::kMaxPulses );
	}

	double Pitch() const
	{
		return plugin.FrameWidthMetres() / width;
	}
};

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( SchlierenPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements; anything else is refused, never read as 0 (polyhedral's
/// trap: strtof( "Fixed" ) is 0, silently).
bool applySetting( SchlierenPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_BUFFER )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( SchlierenPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

/// The harness's music: a busy hi-hat (a fresh level every frame, so the
/// onset detector's floor is not zero) and a kick every half second, soft and
/// hard by turns, written into the Audio buffer the way the host writes it.
void feedKicks( SchlierenPlugin& plugin, int frame )
{
	const int since = frame % 30;
	const bool hard = ( frame / 30 ) % 2 == 1;
	const double hat = 0.3 * hash01( static_cast< uint32_t >( frame ), 11 );
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		double v = bin > 40 ? 0.02 + hat : 0.02;
		if( since < 4 && bin < 10 )
			v += ( hard ? 0.9 : 0.25 ) * std::exp( -since * 0.8 );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), static_cast< float >( v ) );
	}
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise).
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings )
{
	Rig rig;
	rig.fps = fps;
	rig.bpm = bpm;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks          = 0;    ///< shaders::Hook bits
	int airHooks       = 0;    ///< AirHook bits
	int steepenHooks   = 0;    ///< acoustics::SteepenHook bits
	double reflection  = 1.0;  ///< -1: pressure-release walls
	bool resizeClears  = false;///< a new raster forgets the air
	bool cuesRamp      = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

//===========================================================================
// Shared by the checks.
//===========================================================================

/// The ring's own quiet: nothing fires but Fire, no accent, no walls, a
/// focused knife edge, the field and the camera at known values, a linear
/// pulse (140 dB steepens it by a percent or two, which the plan carries).
void quiet( Rig& rig )
{
	rig.Set( PT_FIRE_ON, static_cast< float >( FireOn::Off ) );
	rig.Set( PT_ACCENT, 0.0f );
	rig.Set( PT_LOUDNESS, ParamFromLoudness( 140.0 ) );
	rig.Set( PT_PULSE, static_cast< float >( PulseKind::Pop ) );
	rig.Set( PT_LENGTH, ParamFromLength( 1e-3 ) );
	rig.Set( PT_SPEAKER_X, 0.5f );
	rig.Set( PT_SPEAKER_Y, 0.5f );
	rig.Set( PT_FIELD_WIDTH, ParamFromFieldWidth( 6.0 ) );
	rig.Set( PT_SLOW_MOTION, ParamFromSlowMotion( 300.0 ) );
	rig.Set( PT_WALLS, static_cast< float >( Walls::Open ) );
	rig.Set( PT_REFLECTION, 1.0f );
	rig.Set( PT_OPTICS, static_cast< float >( Optics::KnifeEdge ) );
	rig.Set( PT_KNIFE_ANGLE, 0.0f );
	rig.Set( PT_SENSITIVITY, ParamFromCutoff( 0.3 * kArcsecond ) );
	rig.Set( PT_DISTANCE, 0.0f );
	rig.Set( PT_GREY_FIELD, 0.0f );
	rig.Set( PT_VIEW, static_cast< float >( View::Picture ) );
	rig.Set( PT_MIX, 1.0f );
}

bool startRig( Rig& rig, const Raster& raster, const Floats& picture, const Perturb& perturb )
{
	if( !rig.Init( raster.w, raster.h, &picture ) )
		return false;
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetAirHooksForTest( perturb.airHooks );
	rig.plugin.SetReflectionSignForTest( perturb.reflection );
	rig.plugin.SetResizeClearsForTest( perturb.resizeClears );
	quiet( rig );
	return true;
}

/// The same shell with | f |: its projection is the sum of the magnitudes a
/// float32 sum of the real one adds up, which is what its rounding scales with.
Shell magnitudes( const Shell& shell )
{
	Shell out = shell;
	for( Vertex& v : out.vertices )
		v.f = std::fabs( v.f );
	return out;
}

/// The reference field at a point, and a bound on how far the GPU's may be
/// from it, derived term by term:
///
///   - the table: the GPU interpolates P linearly in u between two table
///     texels, so the reference is evaluated at those two texels' radii and
///     the interpolation error is MEASURED, not assumed;
///   - the table's own rounding: 2e-5 of the magnitudes' projection (a float32
///     sum of at most 256 segments, 256 x 2^-24 = 1.5e-5), plus the radius of
///     a texel rounded in float (4 R ulp) times the table's local slope;
///   - the pixel's distance rounded in float: 8 ulp of the coordinates, times
///     the slope of the table where it is read.
struct Bounded
{
	double value = 0.0;
	double bound = 0.0;
};

struct FieldReference
{
	const Plan& plan;
	std::vector< Shell > absolute;
	std::vector< double > metre;///< each pulse's peak at 1 m, from its dB

	FieldReference( const Plan& p, const Air& air ) : plan( p )
	{
		for( const Shell& shell : plan.shells )
			absolute.push_back( magnitudes( shell ) );
		for( const Pulse& pulse : air.Pulses() )
			metre.push_back( PressureAtMetre( pulse.db ) );
	}

	double reference( int row, double rho, bool magnitude = false ) const
	{
		const Shell& shell = magnitude ? absolute[ static_cast< size_t >( row ) ] : plan.shells[ static_cast< size_t >( row ) ];
		return ProjectedReference( shell, rho, metre[ static_cast< size_t >( row ) ] );
	}

	Bounded at( double x, double y ) const
	{
		Bounded out;
		const double n = shaders::kLutSize - 1;
		for( const Term& term : plan.terms )
		{
			const Shell& shell = plan.shells[ static_cast< size_t >( term.row ) ];
			const double dx = x - term.x, dy = y - term.y;
			const double rho = std::sqrt( dx * dx + dy * dy );
			if( rho >= shell.outer )
				continue;
			const double amp   = std::fabs( term.amplitude );
			const double exact = reference( term.row, rho );
			out.value += term.amplitude * exact;

			const double root = std::sqrt( shell.outer );
			const double u    = std::sqrt( shell.outer - rho ) / root * n;
			const int i       = std::min( static_cast< int >( u ), shaders::kLutSize - 2 );
			auto nodeRho      = [ & ]( int k ) {
                const double q = root * k / n;
                return std::max( shell.outer - q * q, 0.0 );
			};
			const double p0 = reference( term.row, nodeRho( i ) ), p1 = reference( term.row, nodeRho( i + 1 ) );
			const double lerp   = p0 + ( p1 - p0 ) * ( u - i );
			const double interp = std::fabs( lerp - exact );
			const double table  = 2e-5 * reference( term.row, rho, true );
			//dP/du is the secant across the cell; du/drho = n / ( 2 root sqrt( R - rho ) ).
			const double perU   = std::fabs( p1 - p0 );
			const double duDrho = n / ( 2.0 * root * std::sqrt( std::max( shell.outer - rho, 1e-12 ) ) );
			const double drho   = 8.0 * kFloatEps * ( std::fabs( x ) + std::fabs( y ) + std::fabs( term.x ) + std::fabs( term.y ) )
			                    + 4.0 * kFloatEps * shell.outer;
			out.bound += amp * ( interp + table + perU * std::min( duDrho * drho, 1.0 ) ) + amp * 4.0 * kFloatEps * std::fabs( exact );
		}
		return out;
	}
};

/// The centre, in metres, of pressure texel ( i, j ) -- frame pixel ( i - 1, j - 1 ).
double texelCentre( int index, double pitch )
{
	return ( index - 0.5 ) * pitch;
}

double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}

/// Fire one pulse after two settling frames, then run `frames` more.
bool fireAndRun( Rig& rig, int frames )
{
	return rig.Render( 2 ) && rig.Fire() && rig.Render( frames );
}

//===========================================================================
// --steepen (no GL): weak-shock theory.
//===========================================================================

/// Godunov's method for w_s + ( w^2 / 2 )_X = 0: an independent solver of the
/// same entropy problem, first order, cell averages.
std::vector< double > godunov( std::vector< double > w, double dx, double s )
{
	auto flux = []( double a, double b ) {
		if( a > b )
			return std::max( 0.5 * a * a, 0.5 * b * b );
		if( a > 0.0 )
			return 0.5 * a * a;
		if( b < 0.0 )
			return 0.5 * b * b;
		return 0.0;
	};
	std::vector< double > next( w.size() );
	double t = 0.0;
	while( t < s )
	{
		double fastest = 1e-30;
		for( double v : w )
			fastest = std::max( fastest, std::fabs( v ) );
		const double dt = std::min( 0.45 * dx / fastest, s - t );
		for( size_t i = 0; i < w.size(); ++i )
		{
			const double left  = i > 0 ? w[ i - 1 ] : 0.0;
			const double right = i + 1 < w.size() ? w[ i + 1 ] : 0.0;
			next[ i ]          = w[ i ] - dt / dx * ( flux( w[ i ], right ) - flux( left, w[ i ] ) );
		}
		w.swap( next );
		t += dt;
	}
	return w;
}

/// The integral of a polyline (x ascending, jumps allowed) from its start to x.
double runningIntegral( const std::vector< Vertex >& line, double x )
{
	double sum = 0.0;
	for( size_t i = 0; i + 1 < line.size(); ++i )
	{
		const double a = line[ i ].x, b = line[ i + 1 ].x;
		if( b <= a || a >= x )
			continue;
		const double end = std::min( b, x );
		const double fa = line[ i ].f, fb = line[ i ].f + ( line[ i + 1 ].f - line[ i ].f ) * ( end - a ) / ( b - a );
		sum += 0.5 * ( fa + fb ) * ( end - a );
	}
	return sum;
}

/// The cell averages of k f( -X ) on [ x0 + i dx, x0 + ( i + 1 ) dx ].
std::vector< double > cellsOf( const std::vector< Vertex >& profile, double k, double x0, double dx, int cells )
{
	std::vector< double > out( static_cast< size_t >( cells ) );
	for( int i = 0; i < cells; ++i )
	{
		const double xa = x0 + i * dx, xb = xa + dx;
		out[ static_cast< size_t >( i ) ] = k * ( runningIntegral( profile, -xa ) - runningIntegral( profile, -xb ) ) / dx;
	}
	return out;
}

/// Cell averages of the waveform itself, 64 samples a cell.
std::vector< double > initialCells( Shape shape, double length, double k, double x0, double dx, int cells )
{
	std::vector< double > out( static_cast< size_t >( cells ) );
	for( int i = 0; i < cells; ++i )
	{
		double sum = 0.0;
		for( int j = 0; j < 64; ++j )
			sum += Waveform( shape, length, -( x0 + ( i + ( j + 0.5 ) / 64.0 ) * dx ) );
		out[ static_cast< size_t >( i ) ] = k * sum / 64.0;
	}
	return out;
}

double l1( const std::vector< double >& a, const std::vector< double >& b )
{
	double d = 0.0, n = 0.0;
	for( size_t i = 0; i < a.size(); ++i )
	{
		d += std::fabs( a[ i ] - b[ i ] );
		n += std::fabs( b[ i ] );
	}
	return n > 0.0 ? d / n : d;
}

/// The jumps in a profile, as ( tau, size ).
std::vector< std::pair< double, double > > jumpsOf( const Profile& profile )
{
	std::vector< std::pair< double, double > > out;
	const auto& v = profile.vertices;
	for( size_t i = 0; i + 1 < v.size(); ++i )
		if( v[ i + 1 ].x <= v[ i ].x && std::fabs( v[ i + 1 ].f - v[ i ].f ) > 1e-6 )
			out.emplace_back( v[ i ].x, v[ i + 1 ].f - v[ i ].f );
	return out;
}

int runSteepen( const Perturb& perturb )
{
	std::printf( "\n=== steepen: weak-shock theory -- the shocks form when, where and how big it says\n" );
	const int hooks = perturb.steepenHooks;
	const double length = 0.5e-3;

	//The steepest rise of a Pop, in closed form: f0 = -u e^( 1/2 - u^2/2 ),
	//u = ( theta - c ) / sigma, rises fastest at u = sqrt 3, at 2 e^-1 / sigma.
	const double sigma = length / 2.0, closedRise = 2.0 * std::exp( -1.0 ) / sigma;
	const double rise  = MaxRise( Shape::Pop, length );
	Check( std::fabs( rise / closedRise - 1.0 ) < 1e-4,
	       fmt( "a Pop's steepest rise %.6g /s against 2 e^-1 / sigma = %.6g /s (the end correction is 1e-5 of it)", rise, closedRise ) );

	//The shock forms at s* = 1 / ( kappa P_a max f0' ), and not before.
	const double pressure = SurfacePressure( 170.0 );
	const double k = kNonlinearity * pressure, born = 1.0 / ( k * closedRise );
	const Profile before = Steepen( Shape::Pop, length, pressure, 0.95 * born, 4096, hooks );
	const Profile after  = Steepen( Shape::Pop, length, pressure, 1.10 * born, 4096, hooks );
	Check( before.shocks == 0 && after.shocks >= 1,
	       fmt( "170 dB Pop: shock age s* = %.4f m (r = %.2f m); %d shocks at 0.95 s*, %d at 1.1 s*", born,
	            kSpeakerRadius * std::exp( born / kSpeakerRadius ), before.shocks, after.shocks ) );

	//A top hat's entropy solution, in closed form: a rarefaction fan and a
	//shock that meet at s = 2 L / k; after that a triangle, w = ( X + L ) / s,
	//its shock at X = -L + sqrt( 2 k L s ). Its area -- the impulse -- is kL
	//for ever, which Burgers conserves.
	{
		const double boxLength = 0.5e-3;
		const double s         = 6.0 * boxLength / k;
		const Profile box      = Steepen( Shape::Box, boxLength, pressure, s, 4096, hooks );
		const double front     = -boxLength + std::sqrt( 2.0 * k * boxLength * s );//X
		const auto jumps       = jumpsOf( box );
		double shockTau        = 1e9;
		for( const auto& j : jumps )
			if( std::fabs( j.second ) > 0.1 && std::fabs( -j.first - front ) < std::fabs( -shockTau - front ) )
				shockTau = j.first;
		//The grid: 4096 samples over the padded span. The top hat's edges fall
		//between samples, so its jumps are one-cell ramps: the fan's origin
		//is uncertain by a cell, which moves the triangle by cell / s, and
		//the impulse the solver can conserve is the SAMPLED top hat's.
		const double pad  = 1.05 * k * s + 4.0 * boxLength / 4095.0;
		const double cell = ( boxLength + 2.0 * pad ) / 4095.0;
		//The solver's grid starts a whole number of cells before theta = 0.
		const double first = -std::ceil( pad / cell ) * cell;
		double sampled     = 0.0;
		for( int i = 0; i + 1 < 4096; ++i )
			sampled += 0.5 * ( Waveform( Shape::Box, boxLength, first + i * cell ) + Waveform( Shape::Box, boxLength, first + ( i + 1 ) * cell ) ) * cell;
		const double peakW = std::sqrt( 2.0 * k * boxLength / s );
		double worst       = 0.0;
		for( int i = 1; i < 400; ++i )
		{
			const double X = -boxLength + ( front + boxLength ) * i / 400.0;
			if( std::fabs( X - front ) < 3.0 * cell || std::fabs( X + boxLength ) < 3.0 * cell )
				continue;
			worst = std::max( worst, std::fabs( ValueAt( box.vertices, -X ) * k - ( X + boxLength ) / s ) / peakW );
		}
		const double impulse = runningIntegral( box.vertices, 1.0 );
		//Conservation is exact for the hull's shocks (the equal-area edge
		//telescopes); cutting a fan at a shock reads it at slope
		//dw / ( dY + s dw ), not 1/s, which costs at most cell / ( s k ).
		const double conserved = cell / ( s * k );
		Check( std::fabs( -shockTau - front ) < 2.0 * cell && worst < 2.0 * cell / s / peakW && std::fabs( impulse / sampled - 1.0 ) < conserved,
		       fmt( "top hat at s = 6 L / k: shock at X = %.6g ms against -L + sqrt( 2 k L s ) = %.6g ms (2 cells = %.2g ms); "
		            "the triangle within %.2g of its peak (a cell's shift is %.2g); impulse %.7f of the sampled top hat's (bound %.1g)",
		            -shockTau * 1e3, front * 1e3, 2e3 * cell, worst, cell / s / peakW, impulse / sampled, conserved ) );
	}

	//An N-wave lengthens as L sqrt( 1 + 2 k s / L ): each lobe's area is
	//conserved and its slope falls as 1 / ( s + L / 2k ).
	{
		const double nLength = 0.5e-3;
		const double s       = 10.0 * nLength / k;
		const Profile n      = Steepen( Shape::NWave, nLength, pressure, s, 4096, hooks );
		const auto jumps     = jumpsOf( n );
		const double cell    = ( nLength + 2.0 * ( 1.05 * k * s + 4.0 * nLength / 4095.0 ) ) / 4095.0;
		const double want    = nLength * std::sqrt( 1.0 + 2.0 * k * s / nLength );
		const double got     = jumps.size() >= 2 ? jumps.back().first - jumps.front().first : 0.0;
		Check( jumps.size() == 2 && std::fabs( got - want ) < 2.0 * cell,
		       fmt( "N-wave at s = 10 L / k: %zu shocks, %.6g ms apart against L sqrt( 1 + 2 k s / L ) = %.6g ms (2 cells = %.2g ms)",
		            jumps.size(), got * 1e3, want * 1e3, 2e3 * cell ) );
	}

	//Every shape against Godunov. A first-order scheme's L1 error halves with
	//its cell; its own error is estimated as the gap between M and 2M cells,
	//and the hull's answer must sit within twice that of the finer one.
	for( Shape shape : { Shape::Pop, Shape::Kick, Shape::Blast } )
	{
		const double s     = shape == Shape::Kick ? 1.5 * born : 3.0 * born;
		const Profile ours = Steepen( shape, length, pressure, s, 4096, hooks );
		const double span  = Support( shape, length ), pad = 1.05 * k * s + 4.0 * span / 4095.0;
		const double x0 = -span - pad, width = span + 2.0 * pad;
		const int coarse = 1200;
		const double dxC = width / coarse, dxF = dxC / 2.0;
		const auto gC    = godunov( initialCells( shape, length, k, x0, dxC, coarse ), dxC, s );
		const auto gF    = godunov( initialCells( shape, length, k, x0, dxF, 2 * coarse ), dxF, s );
		std::vector< double > gFC( gC.size() );
		for( size_t i = 0; i < gC.size(); ++i )
			gFC[ i ] = 0.5 * ( gF[ 2 * i ] + gF[ 2 * i + 1 ] );
		const double godunovError = l1( gC, gFC );
		const double gap          = l1( cellsOf( ours.vertices, k, x0, dxF, 2 * coarse ), gF );
		static const char* const kNames[] = { "Pop", "Kick", "Blast" };
		Check( gap <= 2.0 * godunovError,
		       fmt( "%-5s at %.1f s*: %d shocks; L1 from Godunov (2400 cells) %.4f, Godunov's own error %.4f", kNames[ static_cast< int >( shape ) ],
		            s / born, ours.shocks, gap, godunovError ) );
	}

	//Simplify keeps every jump and stays within its tolerance everywhere.
	{
		const Profile full = Steepen( Shape::Blast, length, pressure, 3.0 * born, 4096, hooks );
		const auto simple  = Simplify( full.vertices, 1e-4 );
		double worst       = 0.0;
		for( size_t i = 0; i < full.vertices.size(); ++i )
		{
			const bool jump = ( i + 1 < full.vertices.size() && full.vertices[ i + 1 ].x <= full.vertices[ i ].x )
			                  || ( i > 0 && full.vertices[ i - 1 ].x >= full.vertices[ i ].x );
			if( !jump )
				worst = std::max( worst, std::fabs( ValueAt( simple, full.vertices[ i ].x ) - full.vertices[ i ].f ) );
		}
		size_t fullJumps = 0, simpleJumps = 0;
		for( size_t i = 0; i + 1 < full.vertices.size(); ++i )
			fullJumps += full.vertices[ i + 1 ].x <= full.vertices[ i ].x;
		for( size_t i = 0; i + 1 < simple.size(); ++i )
			simpleJumps += simple[ i + 1 ].x <= simple[ i ].x;
		Check( worst <= 1e-4 * ( 1.0 + 1e-9 ) && fullJumps == simpleJumps,
		       fmt( "simplifying a shocked Blast: %zu -> %zu vertices, worst vertical error %.3g (tolerance 1e-4), %zu of %zu jumps kept",
		            full.vertices.size(), simple.size(), worst, simpleJumps, fullJumps ) );
	}
	return Verdict();
}

//===========================================================================
// --beat (no GL): a pulse leaves at the beat, inside the frame.
//===========================================================================
int runBeat( const Perturb& perturb )
{
	std::printf( "\n=== beat: every pulse leaves the speaker at the beat, placed inside its frame, on both clocks\n" );
	struct Case
	{
		FireOn on;
		double perBar;
		const char* name;
	};
	const Case cases[] = { { FireOn::Beat, 4, "Beat" }, { FireOn::HalfBeat, 8, "Half Beat" }, { FireOn::TwoBeats, 2, "2 Beats" }, { FireOn::Bar, 1, "Bar" } };
	const double bpm = 128.0, fps = 60.0, slow = 400.0;
	const int frames = 600;
	for( bool host : { true, false } )
		for( const Case& c : cases )
		{
			Air air;
			air.SetHooksForTest( perturb.airHooks );
			FireInputs in;
			in.fireOn   = c.on;
			in.hostBeat = host;
			in.bpm      = bpm;
			for( int f = 0; f < frames; ++f )
			{
				const double now = f / fps;
				in.barPhase      = std::fmod( now * bpm / 240.0, 1.0 );
				air.Advance( now, f == 0 ? 0.0 : 1.0 / fps, slow, in );
				air.Build( Walls::Open, 1.0, 6.0, 3.375, 1e-6 );//lets the spent go; changes no stamp
			}
			//Beat k of the bar's subdivision is at k * 240 / ( bpm perBar ) s.
			const double period = 240.0 / ( bpm * c.perBar );
			const int expected  = static_cast< int >( std::floor( ( frames - 1 ) / fps / period + 1e-9 ) );
			double worst        = 0.0;
			int index           = 0;
			for( const Pulse& p : air.Pulses() )
			{
				const int beat = static_cast< int >( std::lround( p.emitted * slow / period ) );
				worst          = std::max( worst, std::fabs( p.emitted - beat * period / slow ) * slow );
				++index;
			}
			Check( static_cast< long long >( air.Fired() ) == expected && worst < 1e-9,
			       fmt( "%-9s (%s clock): %llu pulses fired, %d beats in %.2f s; worst departure from the beat %.2g s of host time",
			            c.name, host ? "host's" : "own", air.Fired(), expected, ( frames - 1 ) / fps, worst ) );
		}

	//The speaker moved, Slow Motion changed: a pulse in flight keeps where and
	//when it left.
	{
		Air air;
		FireInputs in;
		in.fireOn = FireOn::Off;
		in.x = 1.0, in.y = 1.0, in.presses = 1;
		air.Advance( 0.0, 0.0, 300.0, in );
		in.presses = 0;
		air.Advance( 1.0 / 60.0, 1.0 / 60.0, 300.0, in );
		in.x = 4.0, in.presses = 1;
		air.Advance( 2.0 / 60.0, 1.0 / 60.0, 3000.0, in );
		const auto& p = air.Pulses();
		Check( p.size() == 2 && p[ 0 ].x == 1.0 && p[ 1 ].x == 4.0 && p[ 0 ].emitted == 0.0 && std::fabs( p[ 1 ].emitted - 1.0 / 60.0 / 300.0 ) < 1e-15,
		       "a press fires at its frame's start; a later press from a moved speaker leaves the first pulse where it was" );
	}
	return Verdict();
}

//===========================================================================
// --abel: the projection table against a brute-force line of sight.
//===========================================================================
int runAbel( const Perturb& perturb )
{
	std::printf( "\n=== abel: each pulse's table is the line-of-sight integral of its shell (raster-independent: the table is in metres)\n" );
	struct Case
	{
		Shape shape;
		double db;
		int frames;
		const char* what;
	};
	const Case cases[] = {
		{ Shape::Pop, 140.0, 40, "Pop, 140 dB" },     { Shape::Blast, 140.0, 25, "Blast, 140 dB" },
		{ Shape::Kick, 140.0, 60, "Kick, 140 dB" },   { Shape::Pop, 185.0, 90, "Pop, 185 dB (shocked)" },
		{ Shape::Box, 140.0, 30, "top hat, 140 dB" },
	};
	const Floats grey = flatCard( 320, 180, 0.5f, 0.5f, 0.5f );
	for( const Case& c : cases )
	{
		Rig rig;
		if( !startRig( rig, { 320, 180 }, grey, perturb ) )
			return 1;
		rig.plugin.SetShapeForTest( c.shape );
		rig.Set( PT_LOUDNESS, ParamFromLoudness( c.db ) );
		if( !fireAndRun( rig, c.frames ) )
			return 1;
		const Plan& plan = rig.plugin.LastPlan();
		if( plan.shells.empty() || plan.shells[ 0 ].vertices.size() < 2 )
		{
			Check( false, fmt( "%s: no shell in flight", c.what ) );
			continue;
		}
		const Shell& shell   = plan.shells[ 0 ];
		const Shell absolute = magnitudes( shell );
		const double metre   = PressureAtMetre( rig.plugin.AirState().Pulses()[ 0 ].db );
		const Floats table   = rig.Table();
		const double n = shaders::kLutSize - 1, root = std::sqrt( shell.outer );
		double worst = 0.0, peak = 0.0, closedWorst = 0.0;
		int bad = 0;
		std::vector< double > ref( static_cast< size_t >( shaders::kLutSize ) );
		auto rhoOf = [ & ]( int i ) {
			const double q = root * i / n;
			return std::max( shell.outer - q * q, 0.0 );
		};
		for( int i = 0; i < shaders::kLutSize; ++i )
		{
			ref[ static_cast< size_t >( i ) ] = ProjectedReference( shell, rhoOf( i ), metre );
			peak                              = std::max( peak, std::fabs( ref[ static_cast< size_t >( i ) ] ) );
		}
		for( int i = 0; i < shaders::kLutSize; ++i )
		{
			const double rho = rhoOf( i ), got = table[ static_cast< size_t >( i ) ], want = ref[ static_cast< size_t >( i ) ];
			//The texel's radius is rounded in float (a few ulp of R): times the
			//table's slope there, which the neighbours give.
			const double slope = ( i > 0 && i + 1 < shaders::kLutSize ) ? std::fabs( ref[ i + 1 ] - ref[ i - 1 ] ) / std::max( rhoOf( i - 1 ) - rhoOf( i + 1 ), 1e-12 ) : 0.0;
			const double bound = 2e-5 * ProjectedReference( absolute, rho, metre ) + slope * 4.0 * kFloatEps * shell.outer + 1e-7 * peak;
			if( std::fabs( got - want ) > bound )
				++bad;
			worst = std::max( worst, std::fabs( got - want ) / std::max( peak, 1e-30 ) );
			if( c.shape == Shape::Box && rho > 0.0 )
			{
				//The top hat in closed form: gain ( acosh( r2 / rho ) - acosh( max( r1, rho ) / rho ) ).
				//Over its flat top (steepening has spread its back edge into a
				//fan, which is not in the closed form) the line of sight sees
				//f = 1 from rho to r2: gain acosh( r2 / rho ).
				double r1 = 1e9, r2 = 0.0;
				for( const Vertex& v : shell.vertices )
					if( v.f > 0.5 )
					{
						r1 = std::min( r1, v.x );
						r2 = std::max( r2, v.x );
					}
				if( rho >= r1 && rho < r2 )
					closedWorst = std::max( closedWorst, std::fabs( 2.0 * metre * std::acosh( r2 / rho ) - want ) / std::max( peak, 1e-30 ) );
			}
		}
		Check( bad == 0 && closedWorst < 1e-9,
		       fmt( "%-22s R %.3f m, %zu vertices: %d of %d texels outside their bound; worst %.2g of the peak%s", c.what, shell.outer,
		            shell.vertices.size(), bad, shaders::kLutSize, worst,
		            c.shape == Shape::Box ? fmt( "; over the flat top the reference is acosh to %.2g", closedWorst ).c_str() : "" ) );
	}
	return Verdict();
}

//===========================================================================
// --field: the pressure texture against the reference, summed over images.
//===========================================================================
int runField( const Perturb& perturb )
{
	std::printf( "\n=== field: P at every pixel is the sum over the speaker's images of their shells' projections\n" );
	for( const Raster& raster : kRasters )
		for( Walls walls : { Walls::Open, Walls::Box } )
		{
			const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
			Rig rig;
			if( !startRig( rig, raster, grey, perturb ) )
				return 1;
			rig.Set( PT_WALLS, static_cast< float >( walls ) );
			rig.Set( PT_REFLECTION, 0.8f );
			rig.Set( PT_SPEAKER_X, 0.35f );
			rig.Set( PT_SPEAKER_Y, 0.4f );
			if( !fireAndRun( rig, 150 ) )
				return 1;
			const Plan& plan = rig.plugin.LastPlan();
			const FieldReference reference( plan, rig.plugin.AirState() );
			const Floats field = rig.Pressure();
			const double pitch = rig.Pitch();
			const int w = raster.w + 2, h = raster.h + 2, step = std::max( 1, raster.w / 160 );
			int bad = 0, sampled = 0;
			double worst = 0.0, peak = 0.0;
			for( int j = 0; j < h; j += step )
				for( int i = 0; i < w; i += step )
				{
					const Bounded want = reference.at( texelCentre( i, pitch ), texelCentre( j, pitch ) );
					const double got   = field[ static_cast< size_t >( j ) * w + i ];
					peak               = std::max( peak, std::fabs( want.value ) );
					worst              = std::max( worst, std::fabs( got - want.value ) );
					bad += std::fabs( got - want.value ) > want.bound;
					++sampled;
				}
			Check( bad == 0 && !plan.terms.empty(),
			       fmt( "%dx%d %-4s %3zu terms: %d of %d texels outside their bound; worst %.2g of the peak %.4g Pa m", raster.w, raster.h,
			            walls == Walls::Box ? "Box" : "Open", plan.terms.size(), bad, sampled, worst / std::max( peak, 1e-30 ), peak ) );
		}
	return Verdict();
}

//===========================================================================
// --speed: the front is at a + c t, to the clock.
//===========================================================================
int runSpeed( const Perturb& perturb )
{
	std::printf( "\n=== speed: a pulse's front is at a + c t, t from the press by the host's clock over Slow Motion\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
		Rig rig;
		if( !startRig( rig, raster, grey, perturb ) )
			return 1;
		//A Pop: its leading foot is at zero pressure, which travels at exactly c
		//at any loudness. (A Blast's front is a shock, and outruns sound by half
		//its strength -- Rankine-Hugoniot, which --steepen checks.)
		rig.Set( PT_LOUDNESS, ParamFromLoudness( 125.0 ) );
		const double slow = SlowMotionFromParam( ParamFromSlowMotion( 300.0 ) );
		if( !rig.Render( 2 ) )
			return 1;
		const int pressFrame = rig.frame;
		if( !rig.Fire() )
			return 1;
		for( int stage = 1; stage <= 3; ++stage )
		{
			if( !rig.Render( 30 ) )
				return 1;
			//The press fires at the start of its frame: the end of the frame
			//before it, on the host's clock.
			const double elapsed = ( rig.Seconds( rig.frame - 1 ) - rig.Seconds( pressFrame - 1 ) ) / slow;
			const double want    = kSpeakerRadius + kSoundSpeed * elapsed;
			const Floats field   = rig.Pressure();
			const double pitch = rig.Pitch(), sx = 0.5 * rig.plugin.FrameWidthMetres(), sy = 0.5 * rig.plugin.FrameHeightMetres();
			const int w = raster.w + 2, j = raster.h / 2 + 1;
			double inside = 0.0, outside = 1e9;
			for( int i = w / 2; i < w; ++i )
			{
				const double dx = texelCentre( i, pitch ) - sx, dy = texelCentre( j, pitch ) - sy, d = std::sqrt( dx * dx + dy * dy );
				if( field[ static_cast< size_t >( j ) * w + i ] != 0.0f )
					inside = std::max( inside, d );
				else if( d > inside )
					outside = std::min( outside, d );
			}
			const double slack = 1e-5;
			Check( want >= inside - slack && want <= outside + slack,
			       fmt( "%dx%d after %.4f ms of air: front at %.5f m, between the last moving pixel %.5f and the first still one %.5f", raster.w,
			            raster.h, elapsed * 1e3, want, inside, outside ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --spreading: in projection a spherical pulse fades as R^-1/2.
//===========================================================================
int runSpreading( const Perturb& perturb )
{
	std::printf( "\n=== spreading: the beam sees a spherical pulse fade as R^-1/2, not as 1/R (raster-independent: read from the table)\n" );
	const Floats grey = flatCard( 320, 180, 0.5f, 0.5f, 0.5f );
	Rig rig;
	if( !startRig( rig, { 320, 180 }, grey, perturb ) )
		return 1;
	rig.Set( PT_LOUDNESS, ParamFromLoudness( 120.0 ) );//linear: the shape must not change
	const double length = 0.2e-3;
	rig.Set( PT_LENGTH, ParamFromLength( length ) );
	rig.Set( PT_SLOW_MOTION, ParamFromSlowMotion( 30.0 ) );
	//A field wide enough that the ring is still in it at 80 widths out: a
	//pulse that has left the frame is let go.
	rig.Set( PT_FIELD_WIDTH, ParamFromFieldWidth( 50.0 ) );
	const double width = 2.0 * length * kSoundSpeed;//the Pop's working width, peak to trough and either side
	std::vector< double > radii, slopes;
	if( !rig.Render( 2 ) || !rig.Fire() )
		return 1;
	for( double target : { 20.0 * width, 40.0 * width, 80.0 * width } )
	{
		for( int guard = 0; rig.plugin.LastPlan().shells.empty() || rig.plugin.LastPlan().shells[ 0 ].outer < target; ++guard )
			if( guard > 2000 || !rig.Render( 1 ) )
			{
				Check( false, fmt( "the pulse never reached %.2f m", target ) );
				return Verdict();
			}
		const Shell& shell = rig.plugin.LastPlan().shells[ 0 ];
		const Floats table = rig.Table();
		const double n = shaders::kLutSize - 1, root = std::sqrt( shell.outer );
		double steepest = 0.0;
		for( int i = 1; i < shaders::kLutSize; ++i )
		{
			const double q0 = root * ( i - 1 ) / n, q1 = root * i / n;
			const double drho = q1 * q1 - q0 * q0;
			steepest = std::max( steepest, std::fabs( table[ static_cast< size_t >( i ) ] - table[ static_cast< size_t >( i - 1 ) ] ) / drho );
		}
		radii.push_back( shell.outer );
		slopes.push_back( steepest );
	}
	//Asymptotically P( R - eta ) = 2 a P_a / sqrt( 2R ) integral f dxi / sqrt( eta - xi ) ( 1 + O( w / R ) ): every
	//radius is the same profile, scaled by R^-1/2. The correction is under w / R at each end.
	const double exponent = std::log( slopes[ 2 ] / slopes[ 0 ] ) / std::log( radii[ 2 ] / radii[ 0 ] );
	const double ratio    = ( slopes[ 0 ] / slopes[ 1 ] ) / std::sqrt( radii[ 1 ] / radii[ 0 ] );
	const double tol      = 2.0 * width / radii[ 0 ];
	Check( std::fabs( exponent + 0.5 ) < tol && std::fabs( ratio - 1.0 ) < tol,
	       fmt( "the steepest dP/drho at R = %.2f, %.2f, %.2f m: exponent %.4f (want -0.5), first ratio %.4f of sqrt( R2 / R1 ); tolerance 2 w / R = %.3f",
	            radii[ 0 ], radii[ 1 ], radii[ 2 ], exponent, ratio, tol ) );
	return Verdict();
}

//===========================================================================
// The optics. A reference deflection per pixel from the reference field.
//===========================================================================
struct Deflection
{
	double x = 0.0, y = 0.0;
	double bound = 0.0;///< on each component, radians
	double px = 0.0, py = 0.0, pc = 0.0;///< second differences for the Jacobian (Pa m)
	double pxy = 0.0;
};

/// The central difference of the reference over two pixels, as the shader
/// takes it, and its bound from the field's.
Deflection deflectionAt( const FieldReference& reference, double x, double y, double pitch, bool second = false )
{
	Deflection d;
	const Bounded e = reference.at( x + pitch, y ), w = reference.at( x - pitch, y );
	const Bounded n = reference.at( x, y + pitch ), s = reference.at( x, y - pitch );
	const double scale = kIndexPerPascal / ( 2.0 * pitch );
	d.x     = ( e.value - w.value ) * scale;
	d.y     = ( n.value - s.value ) * scale;
	d.bound = std::max( e.bound + w.bound, n.bound + s.bound ) * scale;
	if( second )
	{
		const Bounded c = reference.at( x, y );
		d.pc  = c.value;
		d.px  = e.value - 2.0 * c.value + w.value;
		d.py  = n.value - 2.0 * c.value + s.value;
		d.pxy = 0.25 * ( reference.at( x + pitch, y + pitch ).value - reference.at( x - pitch, y + pitch ).value
		                 - reference.at( x + pitch, y - pitch ).value + reference.at( x - pitch, y - pitch ).value );
		d.bound = std::max( d.bound, ( 4.0 * c.bound + e.bound + w.bound + n.bound + s.bound ) * scale );
	}
	return d;
}

/// The pixels the optics checks look at: every pixel of three rows and three
/// columns through the rings, and a sparse grid.
std::vector< std::pair< int, int > > opticsSample( int w, int h )
{
	std::vector< std::pair< int, int > > out;
	for( int r : { h / 2, h / 2 + h / 7, h / 3 } )
		for( int i = 2; i < w - 2; ++i )
			out.emplace_back( i, r );
	for( int c : { w / 2, w / 2 + w / 9 } )
		for( int j = 2; j < h - 2; ++j )
			out.emplace_back( c, j );
	const int step = std::max( 4, w / 40 );
	for( int j = 3; j < h - 2; j += step )
		for( int i = 3; i < w - 2; i += step )
			out.emplace_back( i, j );
	return out;
}

const float* pixelOf( const Floats& picture, int w, int i, int j )
{
	return &picture[ ( static_cast< size_t >( j ) * w + i ) * 4 ];
}

//===========================================================================
// --knife: I / I0 = clamp( 1 + eps.k / eps_c, 0, 2 ).
//===========================================================================
int runKnife( const Perturb& perturb )
{
	std::printf( "\n=== knife: through a focused knife edge the light is clamp( 1 + eps . k / eps_c, 0, 2 ) of the undisturbed\n" );
	for( const Raster& raster : kRasters )
		for( double angle : { 0.0, 90.0, 210.0 } )
		{
			const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
			Rig rig;
			if( !startRig( rig, raster, grey, perturb ) )
				return 1;
			rig.Set( PT_KNIFE_ANGLE, static_cast< float >( angle / 360.0 ) );
			if( !fireAndRun( rig, 60 ) )
				return 1;
			const double cutoff = CutoffFromParam( ParamFromCutoff( 0.3 * kArcsecond ) );
			const double knife  = KnifeAngleFromParam( static_cast< float >( angle / 360.0 ) ) * kPi / 180.0;
			const double kx = std::cos( knife ), ky = std::sin( knife );
			const FieldReference reference( rig.plugin.LastPlan(), rig.plugin.AirState() );
			const Floats out = rig.Output();
			const double pitch = rig.Pitch(), grey0 = linearOf( 0.5 );
			int bad = 0, clamped = 0, sampled = 0;
			double worst = 0.0;
			for( const auto& [ i, j ] : opticsSample( raster.w, raster.h ) )
			{
				const Deflection d   = deflectionAt( reference, ( i + 0.5 ) * pitch, ( j + 0.5 ) * pitch, pitch );
				const double uncover = ( d.x * kx + d.y * ky ) / cutoff;
				const double want    = std::clamp( 1.0 + uncover, 0.0, 2.0 );
				const double got     = linearOf( pixelOf( out, raster.w, i, j )[ 0 ] ) / grey0;
				//The deflection's bound through the knife's slope, plus the
				//sRGB round trip in float (a few ulp of a value near 1).
				const double bound = ( std::fabs( kx ) + std::fabs( ky ) ) * d.bound / cutoff + 1e-5;
				clamped += want == 0.0 || want == 2.0;
				bad += std::fabs( got - want ) > bound;
				worst = std::max( worst, std::fabs( got - want ) );
				++sampled;
			}
			Check( bad == 0 && clamped > 0 && clamped < sampled,
			       fmt( "%dx%d knife at %3.0f deg: %d of %d pixels outside their bound (worst %.2g), %d at the clamp", raster.w, raster.h, angle,
			            bad, sampled, worst, clamped ) );
		}
	return Verdict();
}

//===========================================================================
// --colour: the colour wheel's three laws.
//===========================================================================
int runColour( const Perturb& perturb )
{
	std::printf( "\n=== colour: the wheel passes the light of the clear middle; opposite deflections are complementary; turning the knife 120 degrees cycles the channels\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
		Floats outputs[ 2 ];
		int deflected = 0;
		for( int turn = 0; turn < 2; ++turn )
		{
			Rig rig;
			if( !startRig( rig, raster, grey, perturb ) )
				return 1;
			rig.Set( PT_OPTICS, static_cast< float >( Optics::Colour ) );
			rig.Set( PT_KNIFE_ANGLE, static_cast< float >( ( 20.0 + 120.0 * turn ) / 360.0 ) );
			if( !fireAndRun( rig, 60 ) )
				return 1;
			outputs[ turn ] = rig.Output();
		}
		const double grey0 = linearOf( 0.5 );
		int meanBad = 0, pairBad = 0, cycleBad = 0, pairs = 0, sampled = 0;
		for( const auto& [ i, j ] : opticsSample( raster.w, raster.h ) )
		{
			const float* a = pixelOf( outputs[ 0 ], raster.w, i, j );
			const float* b = pixelOf( outputs[ 1 ], raster.w, i, j );
			double t[ 3 ], u[ 3 ];
			for( int c = 0; c < 3; ++c )
			{
				t[ c ] = linearOf( a[ c ] ) / grey0;
				u[ c ] = linearOf( b[ c ] ) / grey0;
			}
			deflected += std::fabs( t[ 0 ] - 1.0 ) > 1e-3;
			meanBad += std::fabs( ( t[ 0 ] + t[ 1 ] + t[ 2 ] ) / 3.0 - 1.0 ) > 1e-4;
			//Knife + 120: red takes green's, green blue's, blue red's.
			cycleBad += std::fabs( u[ 0 ] - t[ 1 ] ) > 1e-4 || std::fabs( u[ 1 ] - t[ 2 ] ) > 1e-4 || std::fabs( u[ 2 ] - t[ 0 ] ) > 1e-4;
			//The speaker is at the frame's centre: pixel ( w - 1 - i, h - 1 - j )
			//is deflected the opposite way by the same amount.
			const float* m = pixelOf( outputs[ 0 ], raster.w, raster.w - 1 - i, raster.h - 1 - j );
			for( int c = 0; c < 3; ++c )
				if( std::fabs( t[ c ] + linearOf( m[ c ] ) / grey0 - 2.0 ) > 2e-3 )
				{
					++pairBad;
					break;
				}
			++pairs;
			++sampled;
		}
		Check( meanBad == 0 && cycleBad == 0 && pairBad == 0 && deflected > sampled / 20,
		       fmt( "%dx%d: %d coloured of %d; channel mean != 1 at %d; the 120-degree cycle broken at %d; opposite pixels not complementary at %d of %d",
		            raster.w, raster.h, deflected, sampled, meanBad, cycleBad, pairBad, pairs ) );
	}
	return Verdict();
}

//===========================================================================
// --shadow: the shadowgraph.
//===========================================================================
int runShadow( const Perturb& perturb )
{
	std::printf( "\n=== shadow: on a screen L beyond, the light is concentrated by 1 / det( I + L grad eps ) -- conserved overall, "
	             "and to first order 1 - L div eps\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
		const double grey0 = linearOf( 0.5 );
		//Two screens: one near enough for the first-order law, one far enough
		//to bend the light by a fifth.
		for( double strength : { 0.01, 0.2 } )
		{
			Rig rig;
			if( !startRig( rig, raster, grey, perturb ) )
				return 1;
			rig.Set( PT_OPTICS, static_cast< float >( Optics::Shadowgraph ) );
			if( !fireAndRun( rig, 60 ) )
				return 1;
			const double pitch = rig.Pitch();
			FieldReference reference( rig.plugin.LastPlan(), rig.plugin.AirState() );
			//Find the screen distance that bends the light by `strength` at most.
			double most = 0.0;
			const auto sample = opticsSample( raster.w, raster.h );
			std::vector< Deflection > d;
			for( const auto& [ i, j ] : sample )
			{
				d.push_back( deflectionAt( reference, ( i + 0.5 ) * pitch, ( j + 0.5 ) * pitch, pitch, true ) );
				most = std::max( { most, std::fabs( d.back().px ), std::fabs( d.back().py ) } );
			}
			const float distanceParam = ParamFromDistance( strength / ( kIndexPerPascal * most / ( pitch * pitch ) ) );
			const double distance     = DistanceFromParam( distanceParam );
			//The same history again with the screen in place: the air is a
			//function of the presses and the clock, so it is the same air.
			Rig again;
			if( !startRig( again, raster, grey, perturb ) )
				return 1;
			again.Set( PT_OPTICS, static_cast< float >( Optics::Shadowgraph ) );
			again.Set( PT_DISTANCE, distanceParam );
			if( !fireAndRun( again, 60 ) )
				return 1;
			const Floats out = again.Output();
			const double bend = distance * kIndexPerPascal / ( pitch * pitch );
			if( strength < 0.05 )
			{
				//First order: I / I0 - 1 = -L tr( grad eps ) + O( ( L grad eps )^2 ).
				int bad = 0;
				double worst = 0.0;
				for( size_t k = 0; k < sample.size(); ++k )
				{
					const auto [ i, j ] = sample[ k ];
					const double got  = linearOf( pixelOf( out, raster.w, i, j )[ 0 ] ) / grey0 - 1.0;
					const double want = -bend * ( d[ k ].px + d[ k ].py );
					const double a = bend * std::fabs( d[ k ].px ), b = bend * std::fabs( d[ k ].py ), c = bend * std::fabs( d[ k ].pxy );
					const double bound = 2.0 * ( a * a + b * b + a * b + c * c ) + bend * d[ k ].bound * 2.0 * pitch / kIndexPerPascal * 4.0 + 1e-5;
					bad += std::fabs( got - want ) > bound;
					worst = std::max( worst, std::fabs( got - want ) );
				}
				Check( bad == 0,
				       fmt( "%dx%d screen %.0f m (bends by %.3f at most): I/I0 - 1 against -L div eps, %d of %zu pixels outside their second-order "
				            "bound (worst %.2g)",
				            raster.w, raster.h, distance, bend * most, bad, sample.size(), worst ) );
			}
			else
			{
				double total = 0.0;
				for( int j = 0; j < raster.h; ++j )
					for( int i = 0; i < raster.w; ++i )
						total += linearOf( pixelOf( out, raster.w, i, j )[ 0 ] ) / grey0;
				const double mean = total / ( static_cast< double >( raster.w ) * raster.h );
				double darkest = 2.0, brightest = 0.0;
				for( const auto& [ i, j ] : sample )
				{
					const double v = linearOf( pixelOf( out, raster.w, i, j )[ 0 ] ) / grey0;
					darkest        = std::min( darkest, v );
					brightest      = std::max( brightest, v );
				}
				Check( std::fabs( mean - 1.0 ) < 1e-3 && brightest - darkest > 0.1,
				       fmt( "%dx%d screen %.0f m (bends by %.3f): the frame's light %.6f of the undisturbed, from %.3f to %.3f", raster.w, raster.h,
				            distance, bend * most, mean, darkest, brightest ) );
			}
		}
	}
	return Verdict();
}

//===========================================================================
// --background: the backdrop is seen at x + L eps.
//===========================================================================
int runBackground( const Perturb& perturb )
{
	std::printf( "\n=== background: through the air, the backdrop L behind is seen displaced by L eps, exactly\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = coordinateCard( raster.w, raster.h );
		Rig rig;
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		rig.Set( PT_OPTICS, static_cast< float >( Optics::Background ) );
		const float distanceParam = ParamFromDistance( 20000.0 );
		rig.Set( PT_DISTANCE, distanceParam );
		if( !fireAndRun( rig, 60 ) )
			return 1;
		const double pitch = rig.Pitch(), distance = DistanceFromParam( distanceParam );
		const FieldReference reference( rig.plugin.LastPlan(), rig.plugin.AirState() );
		const Floats out = rig.Output();
		int bad = 0, sampled = 0, moved = 0;
		double worst = 0.0, farthest = 0.0;
		for( const auto& [ i, j ] : opticsSample( raster.w, raster.h ) )
		{
			if( i < 12 || j < 12 || i >= raster.w - 12 || j >= raster.h - 12 )
				continue;
			const Deflection d = deflectionAt( reference, ( i + 0.5 ) * pitch, ( j + 0.5 ) * pitch, pitch );
			const float* o     = pixelOf( out, raster.w, i, j );
			const double gx = o[ 0 ] * raster.w - 0.5 - i, gy = o[ 1 ] * raster.h - 0.5 - j;
			const double wx = distance / pitch * d.x, wy = distance / pitch * d.y;
			//A bilinear sampler's weights are fixed point (1/256 of a texel on
			//some GPUs); the coordinate card itself is exact in float to
			//w ulp; and the deflection's own bound, thrown L / pitch.
			const double bound = 1.0 / 256.0 + 4.0 * kFloatEps * raster.w + distance / pitch * d.bound;
			bad += std::fabs( gx - wx ) > bound || std::fabs( gy - wy ) > bound;
			worst = std::max( { worst, std::fabs( gx - wx ), std::fabs( gy - wy ) } );
			farthest = std::max( farthest, std::sqrt( wx * wx + wy * wy ) );
			moved += std::fabs( wx ) + std::fabs( wy ) > 0.5;
			++sampled;
		}
		Check( bad == 0 && moved > 0,
		       fmt( "%dx%d backdrop %.0f m: %d of %d pixels outside their bound (worst %.3g px), %d moved by more than half a pixel, up to %.2f px",
		            raster.w, raster.h, distance, bad, sampled, worst, moved, farthest ) );
	}
	return Verdict();
}

//===========================================================================
// --walls: a rigid wall mirrors the field exactly.
//===========================================================================
int runWalls( const Perturb& perturb )
{
	std::printf( "\n=== walls: a rigid wall is a mirror for the pressure -- the field either side of the frame's edge is the same\n" );
	for( const Raster& raster : kRasters )
		for( Walls walls : { Walls::Floor, Walls::Box } )
		{
			const Floats grey = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
			Rig rig;
			if( !startRig( rig, raster, grey, perturb ) )
				return 1;
			rig.Set( PT_WALLS, static_cast< float >( walls ) );
			rig.Set( PT_REFLECTION, 1.0f );
			rig.Set( PT_SPEAKER_X, 0.3f );
			rig.Set( PT_SPEAKER_Y, 0.25f );
			if( !fireAndRun( rig, walls == Walls::Box ? 160 : 90 ) )
				return 1;
			const Floats field = rig.Pressure();
			const int w = raster.w + 2, h = raster.h + 2;
			auto at = [ & ]( int i, int j ) { return static_cast< double >( field[ static_cast< size_t >( j ) * w + i ] ); };
			//The margin texel and the first frame texel are mirror images about
			//the wall between them.
			double worst = 0.0, scale = 0.0;
			int edges = 0;
			auto compare = [ & ]( int i0, int j0, int i1, int j1 ) {
				worst = std::max( worst, std::fabs( at( i0, j0 ) - at( i1, j1 ) ) );
				scale = std::max( scale, std::fabs( at( i1, j1 ) ) );
			};
			for( int i = 1; i < w - 1; ++i )
				compare( i, 0, i, 1 );
			++edges;
			if( walls == Walls::Box )
			{
				for( int i = 1; i < w - 1; ++i )
					compare( i, h - 1, i, h - 2 );
				for( int j = 1; j < h - 1; ++j )
				{
					compare( 0, j, 1, j );
					compare( w - 1, j, w - 2, j );
				}
				edges += 3;
			}
			//Mirrored texels sum the same terms in a different order (they are
			//sorted by strength): a few ulp of the sum of magnitudes.
			const double bound = 64.0 * kFloatEps * scale * static_cast< double >( std::max< size_t >( rig.plugin.LastPlan().terms.size(), 1 ) );
			Check( worst <= bound && scale > 0.0,
			       fmt( "%dx%d %-5s, reflection 1: %d edge%s, worst difference across a wall %.3g of %.4g Pa m (bound %.2g)", raster.w, raster.h,
			            walls == Walls::Box ? "Box" : "Floor", edges, edges == 1 ? "" : "s", worst, scale, bound ) );
		}
	return Verdict();
}

//===========================================================================
// --audio: onsets fire, the first frame is primed, and Accent follows the hit.
//===========================================================================
int runAudio( const Perturb& perturb )
{
	std::printf( "\n=== audio: an onset fires a pulse, a hard hit louder than a soft one; nothing fires from the cold start; with no audio, Accent leaves every pulse at Loudness\n" );
	const Floats grey = flatCard( 160, 90, 0.5f, 0.5f, 0.5f );
	const double loud = 150.0;
	{
		Rig rig;
		if( !startRig( rig, { 160, 90 }, grey, perturb ) )
			return 1;
		rig.Set( PT_FIRE_ON, static_cast< float >( FireOn::Onset ) );
		rig.Set( PT_ACCENT, 1.0f );
		rig.Set( PT_LOUDNESS, ParamFromLoudness( loud ) );
		rig.Set( PT_THRESHOLD, 0.4f );
		//A bed of hi-hat, a kick every half second, soft and hard by turns;
		//the first frame is already loud (a clip triggered mid-song).
		std::vector< int > kicks;
		rig.beforeFrame = [ & ]( int f ) {
			const int since = ( f - 20 ) % 30;
			const bool hard = ( ( f - 20 ) / 30 ) % 2 == 1;
			for( int bin = 0; bin < audio::kBins; ++bin )
			{
				double v = bin > 40 ? 0.05 : 0.02;
				if( f >= 20 && since < 4 && bin < 10 )
					v += ( hard ? 0.9 : 0.25 ) * std::exp( -since * 0.8 );
				if( f < 2 )
					v += 0.3;
				rig.plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), static_cast< float >( v ) );
			}
			if( f >= 20 && since == 0 )
				kicks.push_back( f );
		};
		if( !rig.Render( 200 ) )
			return 1;
		const auto& pulses = rig.plugin.AirState().Pulses();
		//A pulse fires on the frame its onset is seen, stamped at the frame's
		//start: emitted / ( 1 / 60 / slow ) is the frame before.
		const double slow = SlowMotionFromParam( ParamFromSlowMotion( 300.0 ) );
		std::vector< int > fired;
		std::vector< double > levels;
		for( const Pulse& p : pulses )
		{
			fired.push_back( static_cast< int >( std::lround( p.emitted * slow * 60.0 ) ) + 1 );
			levels.push_back( p.db );
		}
		int matched = 0;
		for( int k : kicks )
			for( int f : fired )
				matched += f == k;
		double soft = 1e9, hard = -1e9;
		for( size_t i = 0; i < fired.size(); ++i )
		{
			const bool isHard = ( ( fired[ i ] - 20 ) / 30 ) % 2 == 1;
			if( isHard )
				hard = std::max( hard, levels[ i ] );
			else
				soft = std::min( soft, levels[ i ] );
		}
		const bool early = !fired.empty() && fired.front() < 20;
		Check( matched == static_cast< int >( kicks.size() ) && fired.size() == kicks.size() && !early,
		       fmt( "%zu kicks, %zu pulses, %d on the kick's own frame; %s before the first kick", kicks.size(), fired.size(), matched,
		            early ? "one FIRED" : "none" ) );
		//Loudness as the plugin reads it: through a float parameter.
		const double ceiling = LoudnessFromParam( ParamFromLoudness( loud ) );
		Check( hard - soft > 3.0 && hard <= ceiling + 1e-9,
		       fmt( "Accent 30 dB: hard hits up to %.2f dB, soft ones down to %.2f dB (Loudness %.0f)", hard, soft, loud ) );
	}
	{
		Rig rig;
		if( !startRig( rig, { 160, 90 }, grey, perturb ) )
			return 1;
		rig.bpm = 120.0;
		rig.Set( PT_FIRE_ON, static_cast< float >( FireOn::Beat ) );
		rig.Set( PT_ACCENT, 1.0f );
		rig.Set( PT_LOUDNESS, ParamFromLoudness( loud ) );
		if( !rig.Render( 130 ) )
			return 1;
		double worst = 0.0;
		for( const Pulse& p : rig.plugin.AirState().Pulses() )
			worst = std::max( worst, std::fabs( p.db - LoudnessFromParam( ParamFromLoudness( loud ) ) ) );
		Check( !rig.plugin.AirState().Pulses().empty() && worst < 1e-9,
		       fmt( "no audio routed, Fire On Beat, Accent 30 dB: %zu pulses, every one within %.2g dB of Loudness",
		            rig.plugin.AirState().Pulses().size(), worst ) );
	}
	return Verdict();
}

//===========================================================================
// --resize: a new raster loses nothing in the air.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: after the host's raster changes, the picture is exactly a fresh instance's that saw the same beats\n" );
	//Twice the last raster, then the raster: the air is in metres, so it must
	//not notice. (Small, so the software renderer gets through it.)
	const Raster small = kRasters.back(), big = { 2 * small.w, 2 * small.h };
	const Floats a = buildCard( big.w, big.h ), b = buildCard( small.w, small.h );
	auto configure = []( Rig& rig ) {
		quiet( rig );
		rig.bpm = 120.0;
		rig.Set( PT_FIRE_ON, static_cast< float >( FireOn::Beat ) );
		rig.Set( PT_DISTANCE, ParamFromDistance( 300.0 ) );
	};
	Rig resized;
	if( !resized.Init( big.w, big.h, &a ) )
		return 1;
	resized.plugin.SetHooksForTest( perturb.hooks );
	resized.plugin.SetResizeClearsForTest( perturb.resizeClears );
	configure( resized );
	if( !resized.Render( 70 ) || !resized.Resize( small.w, small.h, &b ) || !resized.Render( 3 ) )
		return 1;
	Rig fresh;
	if( !fresh.Init( small.w, small.h, &b ) )
		return 1;
	configure( fresh );
	if( !fresh.Render( 73 ) )
		return 1;
	const Floats x = resized.Output(), y = fresh.Output();
	int differ = 0;
	for( size_t i = 0; i < x.size(); ++i )
		differ += byteOf( x[ i ] ) != byteOf( y[ i ] );
	Check( differ == 0 && !fresh.plugin.LastPlan().terms.empty(),
	       fmt( "%dx%d for 70 frames at 120 BPM, then %dx%d for 3, against %dx%d for all 73: %d bytes differ", big.w, big.h, small.w, small.h,
	            small.w, small.h, differ ) );
	return Verdict();
}

//===========================================================================
// --air (no GL): the constants, from somewhere else.
//===========================================================================
int runAir( const Perturb& )
{
	std::printf( "\n=== air: the constants, derived independently -- dry air at 20 C and one atmosphere\n" );
	//The ideal gas: c = sqrt( gamma R T / M ), rho = p M / ( R T ), with
	//CODATA's R and dry air's molar mass 28.965 g/mol.
	const double R = 8.314462618, T = 293.15, M = 0.028965, gamma = 1.4, p0 = 101325.0;
	const double c = std::sqrt( gamma * R * T / M ), rho = p0 * M / ( R * T );
	Check( std::fabs( kSoundSpeed / c - 1.0 ) < 5e-4, fmt( "the speed of sound %.2f m/s against sqrt( gamma R T / M ) = %.2f m/s", kSoundSpeed, c ) );
	Check( std::fabs( kDensity / rho - 1.0 ) < 5e-4, fmt( "the density %.4f kg/m^3 against p M / ( R T ) = %.4f kg/m^3", kDensity, rho ) );
	Check( std::fabs( kBeta - ( gamma + 1.0 ) / 2.0 ) < 1e-12, fmt( "beta %.3f against ( gamma + 1 ) / 2 = %.3f", kBeta, ( gamma + 1.0 ) / 2.0 ) );
	//Air's refractive index at 589 nm: n - 1 = 2.7700e-4 for Edlen's standard
	//air at 15 C (1.000277), scaled by density to 20 C.
	const double index = 2.7700e-4 * 288.15 / 293.15;
	Check( std::fabs( kGladstoneDale * kDensity / index - 1.0 ) < 5e-3,
	       fmt( "n - 1 = K rho = %.4e against Edlen's standard air scaled to 20 C, %.4e", kGladstoneDale * kDensity, index ) );
	//dB SPL is 20 log10( p / 20 uPa ): 140 dB is 200 Pa exactly, at a metre.
	Check( std::fabs( PressureAtMetre( 140.0 ) - 200.0 ) < 1e-9 && std::fabs( SurfacePressure( 140.0 ) * kSpeakerRadius - 200.0 ) < 1e-9,
	       fmt( "140 dB SPL at 1 m is %.9f Pa, and the cone's %.3f Pa times a / 1 m gives it back", PressureAtMetre( 140.0 ), SurfacePressure( 140.0 ) ) );
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	rig.Set( PT_WALLS, static_cast< float >( Walls::Box ) );
	rig.Set( PT_OPTICS, static_cast< float >( Optics::Shadowgraph ) );
	GLuint hostArray = 0, hostBuffer = 0;
	glGenVertexArrays( 1, &hostArray );
	glGenBuffers( 1, &hostBuffer );
	int problems = 0;
	std::string what;
	for( int frame = 0; frame < 6; ++frame )
	{
		rig.Set( PT_FIRE, frame == 1 ? 1.0f : 0.0f );
		glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
		glViewport( 0, 0, 320, 180 );
		glBindVertexArray( hostArray );
		glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
		glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 320, 180 );
		glActiveTexture( GL_TEXTURE0 );
		glUseProgram( 0 );
		rig.plugin.SetTime( frame / 60.0 );
		if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
			return 1;
		GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
		GLfloat clear[ 4 ]   = {};
		GLboolean mask[ 4 ]  = {};
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
		glGetIntegerv( GL_BLEND_SRC_RGB, &src );
		glGetIntegerv( GL_BLEND_DST_RGB, &dst );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
		glGetBooleanv( GL_COLOR_WRITEMASK, mask );
		auto expect = [ & ]( bool ok, const char* name ) {
			if( !ok )
			{
				++problems;
				what += std::string( " " ) + name;
			}
		};
		expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == 320 && viewport[ 3 ] == 180, "viewport" );
		expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
		expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
		expect( program == 0, "program" );
		expect( unit == GL_TEXTURE0, "active-unit" );
		expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
		expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
		expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
		expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
		expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
		for( int u = 0; u < 8; ++u )
		{
			GLint bound = 0;
			glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
			expect( bound == 0, "texture-unit" );
		}
		glActiveTexture( GL_TEXTURE0 );
	}
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glDeleteVertexArrays( 1, &hostArray );
	glDeleteBuffers( 1, &hostBuffer );
	Check( problems == 0 && !rig.plugin.LastPlan().terms.empty(),
	       fmt( "six frames, a pulse in flight in a box: viewport, vertex array, array buffer, program, active unit, framebuffer, "
	            "blend, scissor, clear colour, colour mask, eight texture units (%d wrong:%s)",
	            problems, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults at 120 BPM", []( Rig& ) {} },
		{ "Half Beat, a box at reflection 1, Shadowgraph", []( Rig& r ) {
			 r.Set( PT_FIRE_ON, static_cast< float >( FireOn::HalfBeat ) );
			 r.Set( PT_WALLS, static_cast< float >( Walls::Box ) );
			 r.Set( PT_REFLECTION, 1.0f );
			 r.Set( PT_OPTICS, static_cast< float >( Optics::Shadowgraph ) );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 240 frames of warm-up, median and worst of 120\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			rig.bpm = 120.0;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 240 ) )
				return 1;
			constexpr int kTimed = 120;
			std::vector< double > times, cpu;
			size_t terms = 0;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
				terms = std::max( terms, rig.plugin.LastPlan().terms.size() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-46s %-6s GPU median %6.2f ms, worst %6.2f  (%4.1f%% of a 60 fps frame); wall %5.2f ms; up to %zu terms\n", c.what,
			             size.name, times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ], terms );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

//===========================================================================
// --cues (no GL): options and events step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and events, and ramps standard controls\n" );
	std::istringstream sheet( "0 Optics 0\n60 Optics 3\n0 Fire 0\n60 Fire 1\n0 Loudness 0\n60 Loudness 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	SchlierenPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	SchlierenPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Schlieren: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "air", runAir, true },
		{ "steepen", runSteepen, true },
		{ "beat", runBeat, true },
		{ "abel", runAbel, false },
		{ "field", runField, false },
		{ "speed", runSpeed, false },
		{ "spreading", runSpreading, false },
		{ "knife", runKnife, false },
		{ "colour", runColour, false },
		{ "shadow", runShadow, false },
		{ "background", runBackground, false },
		{ "walls", runWalls, false },
		{ "audio", runAudio, false },
		{ "resize", runResize, false },
		{ "state", runState, false },
		{ "cues", runCues, true },
		{ "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "steepen", runSteepen, "the shock where the hull's feet meet, not where the equal-area rule puts it", []( Perturb& p ) { p.steepenHooks = kHookShockAtVertex; } );
	add( "steepen", runSteepen, "Poisson's folded solution, no entropy condition", []( Perturb& p ) { p.steepenHooks = kHookNoEntropy; } );
	add( "beat", runBeat, "a beat fires at the start of its frame", []( Perturb& p ) { p.airHooks = kHookBeatAtFrameStart; } );
	add( "abel", runAbel, "the shell's pressure without its 1/r", []( Perturb& p ) { p.hooks = kHookNoSpreading; } );
	add( "field", runField, "the table read as if uniform in rho", []( Perturb& p ) { p.hooks = kHookLinearTable; } );
	add( "speed", runSpeed, "sound 2% faster than it is", []( Perturb& p ) { p.airHooks = kHookSoundSpeed; } );
	add( "spreading", runSpreading, "the shell's pressure without its 1/r", []( Perturb& p ) { p.hooks = kHookNoSpreading; } );
	add( "knife", runKnife, "the knife on the other side", []( Perturb& p ) { p.hooks = kHookKnifeFlip; } );
	add( "knife", runKnife, "the deflection a one-sided difference", []( Perturb& p ) { p.hooks = kHookOneSided; } );
	add( "colour", runColour, "the colour wheel turned the wrong way", []( Perturb& p ) { p.hooks = kHookColourSwap; } );
	add( "shadow", runShadow, "the light moved but not concentrated", []( Perturb& p ) { p.hooks = kHookNoJacobian; } );
	add( "background", runBackground, "the backdrop seen at x - L eps", []( Perturb& p ) { p.hooks = kHookBackwards; } );
	add( "walls", runWalls, "pressure-release walls", []( Perturb& p ) { p.reflection = -1.0; } );
	add( "audio", runAudio, "Accent applied with no audio routed", []( Perturb& p ) { p.airHooks = kHookColdAccent; } );
	add( "resize", runResize, "a new raster forgets the air", []( Perturb& p ) { p.resizeClears = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/schlieren.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 150;
	double fps = 60.0, bpm = 120.0;
	std::string mode, scriptPath;
	int filmFrames = -1;
	bool sizeGiven = false, kicks = false;
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "sztest -- render Schlieren offline and measure its air and its optics\n\n"
			             "  --out PATH        render the card and write a PNG (default /tmp/schlieren.png)\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 150)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --bpm N           the synthetic beat (default 120; 0 sends none)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead; an event is released the frame after\n"
			             "  --kicks           (with --out) the harness's music in the Audio buffer: a kick every half second\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bpm" && hasNext )
			bpm = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--kicks" )
			kicks = true;
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		SchlierenPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, bpm, scriptPath, 0, true, settings );
	else if( mode == "film" )
		result = runPipe( width, height, fps, bpm, scriptPath, filmFrames, false, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		rig.bpm = bpm;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			bool rendered = true;
			std::vector< std::pair< int, std::string > > release;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				if( kicks )
					feedKicks( rig.plugin, f );
				for( const auto& r : release )
					if( r.first == f )
					{
						std::string ignored;
						applySetting( rig.plugin, r.second + "=0", ignored );
					}
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( rig.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
						//An event is a press: released on the next frame, as a host would.
						const std::string name = setting.substr( 0, setting.find( '=' ) );
						for( const NamedParameter& parameter : listParameters( rig.plugin ) )
							if( parameter.name == name && parameter.type == FF_TYPE_EVENT )
								release.emplace_back( f + 1, name );
					}
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), %zu pulses in flight, %zu terms\n", outPath.c_str(), width,
				             height, frames, fps, frames / fps, rig.plugin.AirState().Pulses().size(),
				             rig.plugin.LastPlan().terms.size() );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
