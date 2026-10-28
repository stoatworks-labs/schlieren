// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kCommon, source/Shaders.cpp
export const COMMON = `
//log( 1 + x ) for x >= 0. GLSL promises log() only 2^-21 ABSOLUTE near 1,
//which is most of a short segment's whole contribution, so below x = 1 it is
//2 atanh( t ), t = x / ( 2 + x ) <= 1/3, by its series: fifteen odd terms
//leave ( 1/3 )^31 / 31, far below a float.
float logOnePlus( float x )
{
	if( x > 1.0 )
		return log( 1.0 + x );
	float t    = x / ( 2.0 + x );
	float t2   = t * t;
	float term = t;
	float sum  = 0.0;
	for( int k = 0; k < 15; ++k )
	{
		sum += term / float( 2 * k + 1 );
		term *= t2;
	}
	return 2.0 * sum;
}

//sRGB, both ways. The knife and the shadowgraph change how much LIGHT
//arrives, so they multiply in linear light.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}

//Code value 0.5, in linear light: the bare schlieren field.
const float kMidGrey = 0.21404114;
`;

// kAbelFragment, source/Shaders.cpp
export const ABEL_FRAGMENT = `
uniform sampler2D Profiles;//RG32F: ( r, f ) per vertex, one row per pulse, r ascending
uniform vec4 Rows[ 24 ];   //( outer radius, its square root, gain 2 a P_a, vertex count )
uniform int LutSize;
uniform float SpeakerRadius;
uniform int Hooks;

out vec4 fragColor;

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	vec4 row   = Rows[ cell.y ];
	int count  = int( row.w + 0.5 );
	if( count < 2 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	//Uniform in q = sqrt( R - rho ): the table's last texel is the centre.
	float q   = row.y * float( cell.x ) / float( LutSize - 1 );
	float rho = max( row.x - q * q, 0.0 );

	//P = gain * integral f( r ) dr / sqrt( r^2 - rho^2 ): the line of sight
	//crosses the shell at radius r once on the way in and once on the way
	//out, and p's 1/r cancels against dz = r dr / sqrt( r^2 - rho^2 ). On a
	//segment f = f( lo ) + slope ( r - lo ), and both halves have closed
	//forms: integral dr / S = ln( r + S ) and integral r dr / S = S, with
	//S = sqrt( r^2 - rho^2 ). Written as differences that do not cancel:
	//S_hi - S_lo = dr ( hi + lo ) / ( S_hi + S_lo ).
	float sum = 0.0;
	vec2 a    = texelFetch( Profiles, ivec2( 0, cell.y ), 0 ).xy;
	for( int k = 1; k < count; ++k )
	{
		vec2 b = texelFetch( Profiles, ivec2( k, cell.y ), 0 ).xy;
		if( b.x > a.x && b.x > rho )
		{
			float slope = ( b.y - a.y ) / ( b.x - a.x );
			float lo    = max( a.x, rho );
			float flo   = a.y + slope * ( lo - a.x );
			float slo   = sqrt( max( ( lo - rho ) * ( lo + rho ), 0.0 ) );
			float shi   = sqrt( max( ( b.x - rho ) * ( b.x + rho ), 0.0 ) );
			float dr    = b.x - lo;
			float dS    = dr * ( b.x + lo ) / max( shi + slo, 1e-30 );
			float L     = logOnePlus( ( dr + dS ) / ( lo + slo ) );
			if( ( Hooks & 1 ) != 0 )
				//The negative control: p held at its value at the cone, no 1/r.
				//integral f r / S, with integral r^2 / S = ( r S + rho^2 ln( r + S ) ) / 2.
				sum += ( flo * dS + slope * ( 0.5 * ( b.x * shi - lo * slo + rho * rho * L ) - lo * dS ) ) / SpeakerRadius;
			else
				sum += flo * L + slope * ( dS - lo * L );
		}
		a = b;
	}
	fragColor = vec4( row.z * sum, 0.0, 0.0, 1.0 );
}
`;

// kFieldFragment, source/Shaders.cpp
export const FIELD_FRAGMENT = `
uniform sampler2D Lut;  //R32F, LutSize x pulses
uniform sampler2D Terms;//RGBA32F, TermCount x 1: ( x, y, amplitude, row )
uniform int TermCount;
uniform vec4 Rows[ 24 ];
uniform int LutSize;
uniform vec2 Pitch;     //metres per pixel
uniform int Hooks;

out vec4 fragColor;

void main()
{
	//Texel j is frame pixel j - 1, whose centre is j - 0.5 pixels in.
	vec2 p = ( gl_FragCoord.xy - 1.0 ) * Pitch;

	float sum = 0.0;
	for( int t = 0; t < TermCount; ++t )
	{
		vec4 term   = texelFetch( Terms, ivec2( t, 0 ), 0 );
		int row     = int( term.w + 0.5 );
		vec4 shell  = Rows[ row ];
		float rho   = length( p - term.xy );
		if( rho >= shell.x )
			continue;

		//By hand, never through the sampler: a texture unit's filter weights
		//are fixed point (1/256 on some GPUs), and the table is exact.
		float u = ( Hooks & 2 ) != 0 ? ( 1.0 - rho / shell.x ) * float( LutSize - 1 )
		                             : sqrt( shell.x - rho ) / shell.y * float( LutSize - 1 );
		int i      = min( int( u ), LutSize - 2 );
		float frac = u - float( i );
		float a    = texelFetch( Lut, ivec2( i, row ), 0 ).r;
		float b    = texelFetch( Lut, ivec2( i + 1, row ), 0 ).r;
		sum += term.z * ( a + ( b - a ) * frac );
	}
	fragColor = vec4( sum, 0.0, 0.0, 1.0 );
}
`;

// kCompositeFragment, source/Shaders.cpp
export const COMPOSITE_FRAGMENT = `
uniform sampler2D InputTexture;
uniform sampler2D Pressure;//R32F, ( w + 2 ) x ( h + 2 ): texel ( i + 1, j + 1 ) is pixel ( i, j )
uniform vec2 MaxUV;
uniform ivec2 Size;        //the frame, pixels
uniform ivec2 ViewOrigin;  //the host's viewport corner: gl_FragCoord counts from the window's
uniform float Slope;       //K / c^2 / ( 2 pitch ): radians per unit of P across two pixels
uniform float Bend;        //Distance K / c^2 / pitch^2: the Jacobian per unit of P's second difference
uniform float Throw;       //Distance / pitch: pixels of displacement per radian
uniform float Cutoff;      //radians: the deflection that clears the light source's image
uniform vec2 Knife;        //unit: the way a deflection uncovers the light
uniform int OpticsMode;    //0 knife edge, 1 colour, 2 shadowgraph, 3 background
uniform int ViewMode;      //0 picture, 1 pressure, 2 deflection
uniform float PressureScale;
uniform float GreyField;
uniform float MixAmount;
uniform int Hooks;

in vec2 uv;
out vec4 fragColor;

float pressureAt( ivec2 t )
{
	return texelFetch( Pressure, clamp( t, ivec2( 0 ), Size + 1 ), 0 ).r;
}

//P at a fractional pixel position (pixel centres on the integers), bilinear by
//hand: at a whole pixel it IS the texel, so a focused camera's deflection is
//exactly the central difference of the field pass.
float pressureNear( vec2 x )
{
	vec2 t     = x + 1.0;
	vec2 base  = floor( t );
	vec2 f     = t - base;
	ivec2 i    = ivec2( base );
	float p00  = pressureAt( i );
	float p10  = pressureAt( i + ivec2( 1, 0 ) );
	float p01  = pressureAt( i + ivec2( 0, 1 ) );
	float p11  = pressureAt( i + ivec2( 1, 1 ) );
	return mix( mix( p00, p10, f.x ), mix( p01, p11, f.x ), f.y );
}

//The deflection at x, radians: the mean gradient over two pixels.
vec2 deflection( vec2 x )
{
	if( ( Hooks & 4 ) != 0 )
		return 2.0 * Slope * vec2( pressureNear( x + vec2( 1.0, 0.0 ) ) - pressureNear( x ),
		                           pressureNear( x + vec2( 0.0, 1.0 ) ) - pressureNear( x ) );
	return Slope * vec2( pressureNear( x + vec2( 1.0, 0.0 ) ) - pressureNear( x - vec2( 1.0, 0.0 ) ),
	                     pressureNear( x + vec2( 0.0, 1.0 ) ) - pressureNear( x - vec2( 0.0, 1.0 ) ) );
}

//The clip at a pixel position, clamped to its edge.
vec4 clipAt( vec2 x )
{
	vec2 halfTexel = 0.5 / vec2( Size );
	vec2 at        = clamp( ( x + 0.5 ) / vec2( Size ), halfTexel, 1.0 - halfTexel );
	return texture( InputTexture, at * MaxUV );
}

//Colour schlieren: a colour wheel where the knife would be, clear in the
//middle. The hue is the deflection's direction measured from the knife's,
//the saturation its size against Cutoff. Three dot products, no atan: the
//wheel's axes are 120 degrees apart and their weights always sum to 1.5.
vec3 colourWheel( vec2 d )
{
	float size = length( d );
	if( !( size > 0.0 ) )
		return vec3( 1.0 );
	vec2 dir    = d / size;
	vec2 turned = vec2( dot( dir, Knife ), Knife.x * dir.y - Knife.y * dir.x );
	if( ( Hooks & 64 ) != 0 )
		turned.y = -turned.y;
	vec3 hue = 0.5 + 0.5 * vec3( turned.x, dot( turned, vec2( -0.5, 0.8660254 ) ), dot( turned, vec2( -0.5, -0.8660254 ) ) );
	//Times 2, so the wheel passes as much light on average as the clear middle.
	return mix( vec3( 1.0 ), 2.0 * hue, min( size, 1.0 ) );
}

void main()
{
	ivec2 pixel   = ivec2( gl_FragCoord.xy ) - ViewOrigin;
	vec2 here     = vec2( pixel );
	vec4 original = clipAt( here );

	if( ViewMode == 1 )
	{
		float grey = 0.5 + 0.5 * pressureAt( pixel + 1 ) / PressureScale;
		fragColor  = mix( original, vec4( vec3( grey ), 1.0 ), MixAmount );
		return;
	}
	if( ViewMode == 2 )
	{
		vec2 e    = clamp( 0.5 + 0.5 * deflection( here ) / Cutoff, 0.0, 1.0 );
		fragColor = mix( original, vec4( e, 0.5, 1.0 ), MixAmount );
		return;
	}

	vec4 result;
	if( OpticsMode == 3 )
	{
		//Background-oriented schlieren: the camera looks through the air at
		//the clip, Distance behind it. The ray through this pixel is bent by
		//eps on its way through and meets the backdrop L eps further on.
		float way = ( Hooks & 32 ) != 0 ? -1.0 : 1.0;
		result    = clipAt( here + way * Throw * deflection( here ) );
	}
	else
	{
		//A beam through a slide (the clip), through the air, to a camera
		//focused Distance beyond it (0: on the air). Light from x lands at
		//x + L eps( x ); this pixel's x is found by fixed point, and the
		//light it brings is concentrated by 1 / det of that map.
		vec2 at = here;
		if( Throw > 0.0 )
			for( int k = 0; k < 3; ++k )
				at = here - Throw * deflection( at );

		float c  = pressureNear( at );
		float e  = pressureNear( at + vec2( 1.0, 0.0 ) );
		float w  = pressureNear( at - vec2( 1.0, 0.0 ) );
		float n  = pressureNear( at + vec2( 0.0, 1.0 ) );
		float s  = pressureNear( at - vec2( 0.0, 1.0 ) );
		vec2 eps = ( Hooks & 4 ) != 0 ? 2.0 * Slope * vec2( e - c, n - c ) : Slope * vec2( e - w, n - s );

		float light = 1.0;
		if( Bend != 0.0 && ( Hooks & 16 ) == 0 )
		{
			float ne  = pressureNear( at + vec2( 1.0, 1.0 ) );
			float nw  = pressureNear( at + vec2( -1.0, 1.0 ) );
			float se  = pressureNear( at + vec2( 1.0, -1.0 ) );
			float sw  = pressureNear( at + vec2( -1.0, -1.0 ) );
			float jxx = 1.0 + Bend * ( e - 2.0 * c + w );
			float jyy = 1.0 + Bend * ( n - 2.0 * c + s );
			float jxy = Bend * 0.25 * ( ne - nw - se + sw );
			//Past a fold (a caustic) the map is many-to-one and this is the
			//nearest branch's share only: capped, not conserved (AGENTS.md).
			light = 1.0 / max( abs( jxx * jyy - jxy * jxy ), 1.0 / 16.0 );
		}

		vec4 slide = clipAt( at );
		vec3 lit   = mix( toLinear( slide.rgb ), vec3( kMidGrey ), GreyField );
		vec3 filt  = vec3( 1.0 );
		if( OpticsMode == 0 )
		{
			float uncover = dot( eps, Knife ) / Cutoff;
			if( ( Hooks & 8 ) != 0 )
				uncover = -uncover;
			filt = vec3( clamp( 1.0 + uncover, 0.0, 2.0 ) );
		}
		else if( OpticsMode == 1 )
			filt = colourWheel( eps / Cutoff );
		result = vec4( toCode( lit * filt * light ), mix( slide.a, 1.0, GreyField ) );
	}
	fragColor = mix( original, result, MixAmount );
}
`;

// Each stage's source, piece by piece, in the order the plugin joins them:
// the vertex stage as Schlieren.cpp's InitGL builds it, every fragment shader
// as Shaders.cpp's Assemble() does.
export const ASSEMBLY = {
  vertex: ['VERSION', 'QUAD_VERTEX'],
  abel: ['VERSION', 'COMMON', 'ABEL_FRAGMENT'],
  field: ['VERSION', 'COMMON', 'FIELD_FRAGMENT'],
  composite: ['VERSION', 'COMMON', 'COMPOSITE_FRAGMENT'],
};
