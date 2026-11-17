#include "fwio.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// the header uses std::filesystem / std::atomic itself and does not include them
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

// The SDK header mentions RE::InputEvent in an input-callback typedef.  We do
// not use that API and do not include CommonLib here, so a forward declaration
// is all it needs.
namespace RE {
class InputEvent;
}

#include "SKSEMenuFramework.h"  // ImGuiMCP::ImGuiIO, ImGuiMCP::GetIO()

namespace cms::fwio {

namespace {
bool Sane(ImGuiMCP::ImGuiIO* a_io)
{
	if (!a_io) return false;
	// Two cheap invariants that hold for every real frame.  If the framework is
	// updated to an ImGui with a different ImGuiIO layout, this fails and the
	// pages simply lose the wheel instead of reading a random field.
	const float w = a_io->DisplaySize.x;
	const float dt = a_io->DeltaTime;
	return w > 100.0f && w < 30000.0f && dt >= 0.0f && dt < 1.0f;
}
}  // namespace

bool Ready()
{
	return Sane(ImGuiMCP::GetIO());
}

float Wheel()
{
	auto* io = ImGuiMCP::GetIO();
	if (!Sane(io)) return 0.0f;

	const float w = io->MouseWheel;
	if (!std::isfinite(w) || std::fabs(w) > 5.0f) return 0.0f;
	return w;
}

}  // namespace cms::fwio
