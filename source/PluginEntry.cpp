#include "Schlieren.h"

/**
    The one registration.

    This file is listed directly in the Schlieren MODULE target, not in
    schlieren_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Schlieren`, twelve characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class SchlierenEffect : public schlieren::SchlierenPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< SchlierenEffect >,// Create method
	"SZ01",                          // Plugin unique ID of maximum length 4
	"SW Schlieren",                  // Plugin name
	2,                               // API major version number
	1,                               // API minor version number
	0,                               // Plugin major version number
	1,                               // Plugin minor version number
	FF_EFFECT,                       // Plugin type
	"Pressure waves on the beat, seen through a schlieren camera.\n\nA loudspeaker fires a pulse into the air on every "
	"beat, and the clip is seen through that air. The pulse spreads as a spherical sound wave, steepening into a shock "
	"when it is loud; the air's density follows the pressure and its refractive index follows the density, and a knife "
	"edge turns the bent light into light and shade. The rings, their light and dark sides, the way they fade, the "
	"crack of a loud hit and the echoes off the walls all fall out of that.",
	"Schlieren FFGL effect"// About
);

extern "C" const char* SchlierenBuildStamp()
{
	return "schlieren " SCHLIEREN_VERSION ", built " __DATE__ " " __TIME__;
}
