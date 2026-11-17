// One volume for the whole library.
//
// Earlier builds carried a three-level model (master -> event folder -> track)
// and a page that listed every .xwm under a slider.  That is a mixer for a DAW,
// not for a game menu: 48 rows of sliders, clipped labels, and one wrongly
// dragged row quietened a single piece of music forever.  The model is now a
// single master level; the engine multiplies every voice by it.
//
// The mixer file keeps its old shape so an existing install upgrades in place:
// `m` rows are still read and written, `f` / `t` rows are parsed and dropped
// (the count is reported on the page) instead of silently re-applying levels the
// player can no longer see or change.
#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace cms {

class VolumeMix {
public:
	static constexpr float kMaxGain = 1.5f;  // 150 % head room, like the mixer

	// A level and nothing else.  The old `muted` flag is gone: silencing the
	// plugin is what the master switch is for, and a level of 0 % would read as
	// "muted" anyway - one state, one slider, no hidden second switch.
	struct Entry {
		float gain = 1.0f;

		[[nodiscard]] float Effective() const { return gain; }
		[[nodiscard]] bool  Default() const { return gain == 1.0f; }
	};

	bool Load(const std::string& a_path);
	bool Save(const std::string& a_path) const;

	[[nodiscard]] Entry Master() const;
	void              SetMaster(float a_gain);

	// ----------------------------------------------------------- audio thread --
	[[nodiscard]] float MasterEffective() const;

	[[nodiscard]] bool Dirty() const;
	void               SetDirty(bool a_dirty);

	// Rows of an old three-level file that were ignored on load (0 on a fresh
	// install).  Only used to tell the player their old per-track levels are gone.
	[[nodiscard]] std::size_t LegacyRows() const;

private:
	mutable std::mutex mtx_;
	Entry              master_{};
	mutable bool       dirty_ = false;
	mutable std::size_t legacyRows_ = 0;
};

}  // namespace cms
