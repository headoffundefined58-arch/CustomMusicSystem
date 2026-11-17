// XAudio2 sink - the only place in the project that touches Win32 headers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace cms {

class VolumeMix;

// Two independent slots (0 = ambient, 1 = combat).  A slot holds exactly one
// *current* voice plus a list of voices that are still fading out; each one is
// stopped the moment its own envelope reaches zero.  Keeping that list (instead
// of a single "previous" pointer) is what makes an unexpected double switch
// harmless: nothing is ever cut short, so two tracks cannot be heard at full
// volume at once.  Clips are decoded on a loader thread and cached, so the
// engine tick never blocks on disk I/O.
class AudioSink {
public:
	static constexpr int kSlots = 2;

	AudioSink();
	~AudioSink();

	AudioSink(const AudioSink&) = delete;
	AudioSink& operator=(const AudioSink&) = delete;

	bool Init(std::string* a_err);
	void Shutdown();

	// Load (async) and start a clip on a slot.  If a clip is already playing it
	// keeps playing until the new one is ready, then they crossfade.  Returns
	// false only when the sink is not initialised.
	bool Play(int a_slot, const std::string& a_path, float a_fadeSec);
	void Stop(int a_slot, float a_fadeSec);
	void Prefetch(const std::string& a_path);

	void SetBaseGain(int a_slot, float a_gain, float a_seconds = 0.12f);
	void SetMasterGain(float a_gain);

	// Apply envelopes, pick up loaded clips, release finished voices.
	void Poll(double a_now);

	// True exactly once when the currently playing clip ran out.
	bool ConsumeFinished(int a_slot);

	// Playhead of the current clip.  Used to start the next track just before
	// the current one ends, so the handover is a crossfade instead of a hole.
	[[nodiscard]] float PlayheadSec(int a_slot) const;
	[[nodiscard]] float DurationSec(int a_slot) const;

	[[nodiscard]] bool        IsPlaying(int a_slot) const;
	[[nodiscard]] bool        IsPending(int a_slot) const;
	// Something is still audible on this slot: a current voice, a queued clip or
	// a voice that is fading out.
	[[nodiscard]] bool        IsActive(int a_slot) const;
	// A voice on this slot is mid-fade (usually the outgoing side of a
	// crossfade).  The director refuses to switch layers while this is true, so
	// a new track can never be launched on top of a running fade.
	[[nodiscard]] bool        IsFading(int a_slot) const;
	[[nodiscard]] std::string CurrentPath(int a_slot) const;
	[[nodiscard]] float       CurrentDuration(int a_slot) const;
	[[nodiscard]] float       CurrentGain(int a_slot) const;  // last gain sent to the voice
	[[nodiscard]] const char* LastError() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace cms
