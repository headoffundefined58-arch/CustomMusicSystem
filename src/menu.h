// In-game pages, hosted by SKSE Menu Framework (F1 -> mod control panel).
//
// The framework is a sibling DLL: it exports its menu api plus the whole cimgui
// surface, so everything here is resolved with GetProcAddress and there is no
// link-time dependency.  Without the framework the plugin simply has no page -
// the audio engine does not care, the mixer file keeps working either way.
#pragma once

#include <string>

namespace cms {

struct Config;
struct Library;
class VolumeMix;
class Director;

namespace menu {

// Game thread, once, at kDataLoaded (after the first library scan).
bool Register(Config& a_cfg, const Library& a_lib, VolumeMix& a_mix, Director& a_dir, std::string a_mixerPath,
	std::string a_iniPath);
void Unregister();

// Republishes the track list the page draws from.  Called after every library
// scan (which happens on the engine thread) so the UI never walks the live
// Library while a rescan touches its vectors.
void PublishLibrary(const Library& a_lib);

[[nodiscard]] bool Available();

}  // namespace menu
}  // namespace cms
