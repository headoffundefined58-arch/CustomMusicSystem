#include "volume.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace cms {

namespace {

constexpr const char* kHeader =
	"; CustomMusicSystem volume\n"
	"; Written by the in-game page (SKSE Menu Framework -> Custom Music System -> Volume).\n"
	"; Format: m TAB master TAB <gain 0..1.5> TAB <unused, always 0>\n"
	"; Older f/t rows (per folder / per track) are no longer used and are dropped on save.\n";

std::vector<std::string> SplitTab(const std::string& a_line)
{
	std::vector<std::string> out;
	std::size_t              start = 0;
	for (;;) {
		const auto tab = a_line.find('\t', start);
		if (tab == std::string::npos) {
			out.push_back(a_line.substr(start));
			break;
		}
		out.push_back(a_line.substr(start, tab - start));
		start = tab + 1;
	}
	return out;
}

float ClampGain(float a_v) { return std::clamp(a_v, 0.0f, VolumeMix::kMaxGain); }

}  // namespace

VolumeMix::Entry VolumeMix::Master() const
{
	std::lock_guard lock(mtx_);
	return master_;
}

void VolumeMix::SetMaster(float a_gain)
{
	std::lock_guard lock(mtx_);

	const float gain = ClampGain(a_gain);
	if (master_.gain == gain) return;

	master_.gain = gain;
	dirty_ = true;
}

float VolumeMix::MasterEffective() const
{
	std::lock_guard lock(mtx_);
	return master_.Effective();
}

bool VolumeMix::Dirty() const
{
	std::lock_guard lock(mtx_);
	return dirty_;
}

void VolumeMix::SetDirty(bool a_dirty)
{
	std::lock_guard lock(mtx_);
	dirty_ = a_dirty;
}

std::size_t VolumeMix::LegacyRows() const
{
	std::lock_guard lock(mtx_);
	return legacyRows_;
}

bool VolumeMix::Load(const std::string& a_path)
{
	std::ifstream in(a_path, std::ios::binary);
	if (!in) return false;

	std::lock_guard lock(mtx_);

	std::size_t ignored = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;

		const auto cols = SplitTab(line);
		if (cols.size() < 4) continue;

		const std::string& kind = cols[0];
		if (kind != "m") {
			// f = event folder, t = single track: the unified model has no place
			// for them, and keeping them would mute music the page cannot show.
			++ignored;
			continue;
		}

		// cols[3] used to be a per-install mute flag; it is ignored rather than
		// restored, so an old file can never start up silent with no widget to
		// undo it.
		master_.gain = ClampGain(static_cast<float>(std::atof(cols[2].c_str())));
	}

	legacyRows_ = ignored;
	dirty_ = false;
	return true;
}

bool VolumeMix::Save(const std::string& a_path) const
{
	std::lock_guard lock(mtx_);

	std::FILE* f = nullptr;
	if (fopen_s(&f, a_path.c_str(), "wb") != 0 || !f) return false;

	std::fputs(kHeader, f);
	std::fprintf(f, "m\tmaster\t%.4f\t0\n", master_.gain);

	std::fclose(f);
	dirty_ = false;
	legacyRows_ = 0;
	return true;
}

}  // namespace cms
