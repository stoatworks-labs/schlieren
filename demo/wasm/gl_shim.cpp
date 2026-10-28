/*
    The five GL entry points emscripten's WebGL2 library cannot take from the
    plugin as they are: glShaderSource, glEnable, glDisable, glIsEnabled and
    glTexImage2D. Every other GL call the plugin and the FFGL SDK make goes
    straight to emscripten's implementation, on the page's own WebGL2 context.
    zoetrope's shim (polyhedral's before it), with Schlieren's names and one
    entry point more.

    glShaderSource
        The plugin hands GL desktop GLSL 4.10 (`#version 410 core`); WebGL2
        compiles GLSL ES 3.00. The page's `schlierenShaderSource` first
        REQUIRES the text to be byte for byte one of the page's own assemblies
        of the plugin's shaders (demo/shaders.js, which
        demo/tools/check_shaders.py holds to source/Shaders.cpp, and its
        ASSEMBLY table to Assemble() and InitGL), then applies the demo kit's
        `port()`: the version line and the ES precision defaults, nothing
        else. A shader the plugin assembled differently from the page's copy
        is refused (an empty source, so the plugin's InitGL fails and logs
        which pass), never compiled quietly.

    glEnable / glDisable / glIsEnabled
        GL_PROGRAM_POINT_SIZE is desktop-only state (in ES a point is always
        sized by the shader, and WebGL2 rejects the enum with INVALID_ENUM).
        The plugin's GLState.h saves and restores it on every frame, as a
        plugin inside Resolume must. Here it reads as off and enabling it is
        ignored; the plugin draws no points. Every other capability is passed
        through unchanged.

    glTexImage2D
        The pressure on the frame and the projection table are R32F render
        targets, allocated by the FFGL SDK's FFGLFBO::Initialise (PassBuffer's
        base) as glTexImage2D( ..., GL_R32F, ..., GL_RGBA, GL_FLOAT, NULL ).
        Desktop GL takes any client format with no data; OpenGL ES 3.0, and so
        WebGL2, takes exactly one per sized format (ES 3.0.6, table 3.2), and
        for R32F it is GL_RED -- with GL_RGBA it answers INVALID_OPERATION, the
        texture is never allocated, and the plugin's passes draw into nothing.
        So an allocation with NO data is given the client format and type ES
        pairs with its internal format. With no data there is nothing for a
        client format to describe, so nothing about what is allocated changes.
        Only the plugin's three no-data allocations are carried (R32F from the
        SDK; RG32F and RGBA32F from Schlieren.cpp's makeDataTexture, which
        already pass the ES pair and go through as they are); anything else --
        and any call that carries data -- aborts, by design, so a change in the
        plugin cannot be absorbed here quietly. The shells and the terms reach
        their textures through glTexSubImage2D, which is emscripten's own.

    Nothing else needed one. Rendering into an R32F target needs
    EXT_color_buffer_float, which the page asks for and stops without; both
    R32F targets and both data textures are read only by texelFetch, with
    nearest filtering, so no float filtering is needed.
*/
#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
constexpr GLenum kProgramPointSize = 0x8642;///< GL_PROGRAM_POINT_SIZE, desktop GL 3.2

// clang-format off
EM_JS( void, szShaderSource, ( GLuint shader, const char* source ), {
	const object = GL.shaders[ shader ];
	const stage  = GLctx.getShaderParameter( object, 0x8B4F /* GL_SHADER_TYPE */ ) === 0x8B31 ? 'vertex' : 'fragment';
	GLctx.shaderSource( object, Module[ 'schlierenShaderSource' ]( stage, UTF8ToString( source ) ) );
} );
EM_JS( void, szEnable, ( GLenum cap ), { GLctx.enable( cap ); } );
EM_JS( void, szDisable, ( GLenum cap ), { GLctx.disable( cap ); } );
EM_JS( int, szIsEnabled, ( GLenum cap ), { return GLctx.isEnabled( cap ) ? 1 : 0; } );
EM_JS( void, szTexImage2DEmpty, ( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type ), {
	GLctx.texImage2D( target, level, internalformat, width, height, border, format, type, null );
} );
// clang-format on

[[noreturn]] void refuse( const char* why, GLint internalformat, GLenum format, GLenum type )
{
	std::fprintf( stderr, "schlieren demo: glTexImage2D( internal 0x%x, format 0x%x, type 0x%x ) %s -- not a call this page's GL shim carries\n",
	              static_cast< unsigned >( internalformat ), format, type, why );
	std::abort();
}
} // namespace

extern "C"
{
void glShaderSource( GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length )
{
	std::string source;
	for( GLsizei i = 0; i < count; ++i )
	{
		if( string[ i ] == nullptr )
			continue;
		const size_t n = ( length != nullptr && length[ i ] >= 0 ) ? static_cast< size_t >( length[ i ] ) : std::strlen( string[ i ] );
		source.append( string[ i ], n );
	}
	szShaderSource( shader, source.c_str() );
}

void glEnable( GLenum cap )
{
	if( cap != kProgramPointSize )
		szEnable( cap );
}

void glDisable( GLenum cap )
{
	if( cap != kProgramPointSize )
		szDisable( cap );
}

GLboolean glIsEnabled( GLenum cap )
{
	if( cap == kProgramPointSize )
		return GL_FALSE;
	return szIsEnabled( cap ) ? GL_TRUE : GL_FALSE;
}

void glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format,
                   GLenum type, const void* pixels )
{
	if( target != GL_TEXTURE_2D || level != 0 )
		refuse( "is not a level-0 2D texture", internalformat, format, type );
	if( pixels != nullptr )
		refuse( "carries data", internalformat, format, type );
	if( type != GL_FLOAT )
		refuse( "is not float", internalformat, format, type );

	//The one pair ES 3.0 accepts for each sized float format, with no data.
	GLenum esFormat = 0;
	switch( internalformat )
	{
	case GL_R32F: esFormat = GL_RED; break;
	case GL_RG32F: esFormat = GL_RG; break;
	case GL_RGBA32F: esFormat = GL_RGBA; break;
	default: refuse( "is not one of the plugin's three float formats", internalformat, format, type );
	}
	szTexImage2DEmpty( target, level, internalformat, width, height, border, esFormat, GL_FLOAT );
}
} // extern "C"
