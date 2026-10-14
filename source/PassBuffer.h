#pragma once

#include <FFGLSDK.h>

namespace schlieren
{
/**
    An off-screen buffer for one stage of the chain.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame and
    is a no-op in the overwhelming majority of them.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first. It matters here: the
    pressure buffer reallocates whenever the raster changes.

    **It owns its filtering and its wrap.** Both of this plugin's buffers, the
    projection table and the pressure on the frame (a pixel wider each side,
    for the central difference), are data read texel-for-texel, so they ask
    for Nearest and Clamp. The other answers are kept from millpond, whose
    class this is, for the next plugin that wants them.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Wrap
	{
		Clamp, ///< a picture: past the edge is more of the edge
		Repeat ///< a periodic domain: past the edge is the other edge
	};

	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back -- and for the edge
	/// history buffers, which feed back into themselves, it is noise that never
	/// washes out.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling,
	             Wrap wrap = Wrap::Clamp );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. `textureQueryLevels`
	/// is GLSL 4.30 and these shaders are 4.10.
	float MaxMipLevel() const;

	/// Clear to transparent black. For the surface, that is still water.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
	Wrap wrap         = Wrap::Clamp;
};

} // namespace schlieren
