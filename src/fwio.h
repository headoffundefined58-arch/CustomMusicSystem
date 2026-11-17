// Mouse-wheel access for the settings/volume pages.
//
// The framework's own api is resolved by name (see menu.cpp), but the wheel is
// a *field* on ImGuiIO, not a function: igGetIO() hands out the pointer and the
// field offset has to come from the framework's own header.  Keeping that in
// one small translation unit means the heavy header is not dragged into the
// rest of the plugin, and the offset is compiler-checked against the real
// ImGuiIO instead of being a magic number.
#pragma once

namespace cms::fwio {

// ImGuiIO looked sane on this frame (layout matches the header we built with).
[[nodiscard]] bool Ready();

// Vertical mouse wheel for the current frame, 0 when it did not move.
[[nodiscard]] float Wheel();

}  // namespace cms::fwio
