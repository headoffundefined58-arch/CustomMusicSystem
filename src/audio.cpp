#include "audio.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core.h"
#include "volume.h"

namespace cms {

namespace {

struct Clip {
	std::string           path;
	std::vector<std::uint8_t> data;      // compressed payload, or raw PCM
	std::vector<std::uint32_t> wmaCum;   // xWMA dpds: decoded bytes per packet
	WAVEFORMATEX          fmt{};
	bool                  xwma = false;
	float                 duration = 0.0f;
	std::uint64_t         lastUse = 0;
};

std::uint16_t Rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Rd32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }

// Skyrim .xwm is RIFF/XWMA: 18 byte WAVEFORMATEX (tag 0x0161) + dpds.
// XAudio2 still decodes xWMA provided the buffer is submitted together with
// XAUDIO2_BUFFER_WMA - no decoder of our own is involved, which is both faster
// and bit-exact.
bool ParseClip(const std::string& a_path, Clip& a_out, std::string* a_err)
{
	std::ifstream in(a_path, std::ios::binary | std::ios::ate);
	if (!in) {
		if (a_err) *a_err = "cannot open " + a_path;
		return false;
	}
	const auto size = static_cast<std::size_t>(in.tellg());
	if (size < 64 || size > (256ull << 20)) {
		if (a_err) *a_err = "bad size " + a_path;
		return false;
	}
	in.seekg(0);

	std::vector<std::uint8_t> raw(size);
	if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size))) {
		if (a_err) *a_err = "short read " + a_path;
		return false;
	}

	if (std::memcmp(raw.data(), "RIFF", 4) != 0) {
		if (a_err) *a_err = "not RIFF: " + a_path;
		return false;
	}
	std::uint8_t* dataPtr = nullptr;
	std::size_t   dataLen = 0;
	std::uint8_t* dpdsPtr = nullptr;
	std::size_t   dpdsLen = 0;
	bool haveFmt = false;

	std::size_t off = 12;
	while (off + 8 <= raw.size()) {
		const std::uint8_t* id = raw.data() + off;
		const std::uint32_t csize = Rd32(raw.data() + off + 4);
		const std::size_t   body = off + 8;
		if (body + csize > raw.size()) break;

		if (std::memcmp(id, "fmt ", 4) == 0 && csize >= 16) {
			if (csize >= sizeof(WAVEFORMATEX)) {
				std::memcpy(&a_out.fmt, raw.data() + body, sizeof(WAVEFORMATEX));
			} else {
				// 16 byte PCMWAVEFORMAT: pad cbSize
				std::memcpy(&a_out.fmt, raw.data() + body, 16);
				a_out.fmt.cbSize = 0;
			}
			if (a_out.fmt.wFormatTag == 0xFFFE && csize >= 40) {
				// WAVE_FORMAT_EXTENSIBLE: real tag lives in the sub-format GUID
				std::uint16_t sub = 0;
				std::memcpy(&sub, raw.data() + body + 24, sizeof(sub));
				a_out.fmt.wFormatTag = sub;
			}
			haveFmt = true;
		} else if (std::memcmp(id, "data", 4) == 0) {
			dataPtr = raw.data() + body;
			dataLen = csize;
		} else if (std::memcmp(id, "dpds", 4) == 0) {
			dpdsPtr = raw.data() + body;
			dpdsLen = csize;
		}
		off = body + csize + (csize & 1);
	}

	if (!haveFmt || !dataPtr || dataLen == 0) {
		if (a_err) *a_err = "missing fmt/data: " + a_path;
		return false;
	}

	a_out.path = a_path;
	a_out.xwma = (a_out.fmt.wFormatTag == 0x0161 || a_out.fmt.wFormatTag == 0x0162 || a_out.fmt.wFormatTag == 0x0163);
	a_out.data.assign(dataPtr, dataPtr + dataLen);

	if (a_out.xwma) {
		if (!dpdsPtr || dpdsLen < 4) {
			if (a_err) *a_err = "xWMA without dpds: " + a_path;
			return false;
		}
		// Each dpds entry is a 32-bit cumulative decoded size; the entry count is
		// the chunk size in UINT32s (see MS XAUDIO2_BUFFER_WMA docs / FFmpeg xwma.c).
		const std::size_t packets = dpdsLen / 4;
		a_out.wmaCum.reserve(packets);
		for (std::size_t i = 0; i < packets; ++i) a_out.wmaCum.push_back(Rd32(dpdsPtr + i * 4));

		const std::uint32_t total = a_out.wmaCum.back();
		const std::uint32_t bytesPerSec = a_out.fmt.nChannels * (a_out.fmt.wBitsPerSample / 8) * a_out.fmt.nSamplesPerSec;
		a_out.duration = bytesPerSec ? static_cast<float>(total) / static_cast<float>(bytesPerSec) : 0.0f;
	} else {
		const std::uint32_t block = a_out.fmt.nBlockAlign ? a_out.fmt.nBlockAlign : 4;
		a_out.duration = a_out.fmt.nAvgBytesPerSec ? static_cast<float>(dataLen) / static_cast<float>(a_out.fmt.nAvgBytesPerSec)
		                                          : static_cast<float>(dataLen / block) / static_cast<float>(a_out.fmt.nSamplesPerSec ? a_out.fmt.nSamplesPerSec : 44100);
	}
	return true;
}

}  // namespace

struct AudioSink::Impl {
	struct Voice {
		IXAudio2SourceVoice* v = nullptr;
		std::shared_ptr<Clip> clip;
		float fadeFrom = 1.0f;
		float fadeTo = 1.0f;
		double fadeT0 = 0.0;
		float fadeDur = 0.0f;
		float applied = -1.0f;
		float smoothed = 0.0f;   // final gain, eased so slider drags do not click

		Voice() = default;
		Voice(const Voice&) = delete;
		Voice& operator=(const Voice&) = delete;

		// Owning the voice here is what keeps "at most one fading-out voice per
		// slot" true.  It used to be a plain struct, so replacing an already
		// fading voice (two quick track changes) leaked the IXAudio2SourceVoice:
		// it kept playing at whatever volume it last had -> two tracks at once.
		~Voice()
		{
			if (!v) return;
			v->Stop(0);
			v->FlushSourceBuffers();
			v->DestroyVoice();
			v = nullptr;
		}
	};

	IXAudio2*                xa = nullptr;
	IXAudio2MasteringVoice*  mv = nullptr;
	std::unique_ptr<Voice>   cur[kSlots];

	// Voices that are on their way out.  A vector, not a single slot: when a
	// switch happens while a crossfade is still running (a cell border during a
	// handover, a fight during a track change) the older voice is *not* cut, it
	// keeps its own envelope and is destroyed when it reaches silence.  The old
	// "one prev pointer" design destroyed the fading voice instantly, which is
	// audible as a click and, if the new clip then failed to load, as a hole.
	std::vector<std::unique_ptr<Voice>> outgoing[kSlots];

	std::string pendingPath[kSlots];
	float       pendingFade[kSlots]{};
	bool        pending[kSlots]{};

	// slot base gain, ramped over an explicit duration so ducking follows the
	// crossfade instead of stepping in one 80 ms tick
	float  base[kSlots]{ 1.0f, 1.0f };
	float  baseTo[kSlots]{ 1.0f, 1.0f };
	float  baseFrom[kSlots]{ 1.0f, 1.0f };
	float  baseDur[kSlots]{ 0.0f, 0.0f };
	double baseT0[kSlots]{ 0.0, 0.0 };

	float master = 1.0f;
	float masterTarget = 1.0f;
	double lastPoll = 0.0;

	// loader thread
	std::thread             loader;
	std::mutex              mtx;
	std::condition_variable cv;
	std::map<std::string, std::shared_ptr<Clip>> cache;
	std::vector<std::string> queue;
	bool                    quit = false;
	std::uint64_t           useCounter = 0;
	std::string             err;
	bool                    ready = false;
	bool                    comInit = false;

	~Impl() { Shutdown(); }

	float Env(const Voice& a_v, double a_now) const
	{
		if (a_v.fadeDur <= 0.0f) return a_v.fadeTo;
		const float t = std::clamp(static_cast<float>((a_now - a_v.fadeT0) / a_v.fadeDur), 0.0f, 1.0f);
		// equal-power curve keeps perceived loudness flat across the crossfade
		const float a = a_v.fadeFrom;
		const float b = a_v.fadeTo;
		return std::sqrt(a * a * (1.0f - t) + b * b * t);
	}

	void Destroy(std::unique_ptr<Voice>& a_v)
	{
		// ~Voice() stops, flushes and destroys the source voice; dropping the
		// pointer is all that is left to do.
		a_v.reset();
	}

	bool StartVoice(int a_slot, const std::shared_ptr<Clip>& a_clip, float a_fade, double a_now)
	{
		IXAudio2SourceVoice* v = nullptr;
		WAVEFORMATEX fmt = a_clip->fmt;
		HRESULT hr = xa->CreateSourceVoice(&v, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr);
		if (FAILED(hr) || !v) {
			err = "CreateSourceVoice failed (0x" + std::to_string(static_cast<unsigned>(hr)) + ")";
			LOG_E("audio: %s  fmt tag=0x%04x %uch %uHz", err.c_str(), fmt.wFormatTag, fmt.nChannels, fmt.nSamplesPerSec);
			return false;
		}

		XAUDIO2_BUFFER buf{};
		buf.Flags = XAUDIO2_END_OF_STREAM;
		buf.AudioBytes = static_cast<UINT32>(a_clip->data.size());
		buf.pAudioData = a_clip->data.data();

		XAUDIO2_BUFFER_WMA wma{};
		if (a_clip->xwma) {
			wma.PacketCount = static_cast<UINT32>(a_clip->wmaCum.size());
			wma.pDecodedPacketCumulativeBytes = a_clip->wmaCum.data();
			hr = v->SubmitSourceBuffer(&buf, &wma);
		} else {
			hr = v->SubmitSourceBuffer(&buf);
		}
		if (FAILED(hr)) {
			err = "SubmitSourceBuffer failed (0x" + std::to_string(static_cast<unsigned>(hr)) + ")";
			LOG_E("audio: %s  %s", err.c_str(), a_clip->path.c_str());
			v->DestroyVoice();
			return false;
		}

		v->SetVolume(0.0f);
		v->Start(0);

		// retire the outgoing voice with a matching fade
		if (cur[a_slot]) {
			auto old = std::move(cur[a_slot]);
			old->fadeFrom = old->applied < 0.0f ? 1.0f : old->applied;
			old->fadeTo = 0.0f;
			old->fadeDur = std::max(0.05f, a_fade);
			old->fadeT0 = a_now;
			outgoing[a_slot].push_back(std::move(old));
		}

		// Under normal operation the director never asks for a switch while a
		// fade is running, so this list is empty or has exactly one entry.  If a
		// third voice ever shows up (a scene transition right after a handover),
		// squeeze everything older than the new voice into a fast 200 ms release
		// so the overlap stays short without ever cutting a voice dead.
		auto& out = outgoing[a_slot];
		for (std::size_t i = 0; i + 1 < out.size(); ++i) {
			Voice& victim = *out[i];
			if (victim.fadeDur > 0.2f) {
				const float done = victim.fadeDur > 0.0f
					? std::clamp(static_cast<float>((a_now - victim.fadeT0) / victim.fadeDur), 0.0f, 1.0f)
					: 1.0f;
				victim.fadeFrom = Env(victim, a_now);
				victim.fadeTo = 0.0f;
				victim.fadeDur = std::max(0.2f * (1.0f - done), 0.05f);
				victim.fadeT0 = a_now;
			}
		}

		auto voice = std::make_unique<Voice>();
		voice->v = v;
		voice->clip = a_clip;
		voice->fadeFrom = 0.0f;
		voice->fadeTo = 1.0f;
		// never start at full amplitude: even a "no fade" request gets 50 ms of
		// ramp, which is what removes the click at the head of a track
		voice->fadeDur = std::max(0.05f, a_fade);
		voice->fadeT0 = a_now;
		cur[a_slot] = std::move(voice);
		return true;
	}

	void LoaderMain()
	{
		for (;;) {
			std::string path;
			{
				std::unique_lock lock(mtx);
				cv.wait(lock, [this] { return quit || !queue.empty(); });
				if (quit) return;
				path = std::move(queue.front());
				queue.erase(queue.begin());
			}

			auto clip = std::make_shared<Clip>();
			std::string err;
			if (ParseClip(path, *clip, &err)) {
			LOG_D("clip: %s  %.1fs  %uHz %uch  %s  %.1f MB", path.c_str(), clip->duration,
				clip->fmt.nSamplesPerSec, clip->fmt.nChannels, clip->xwma ? "xwma" : "pcm",
				static_cast<double>(clip->data.size()) / (1024.0 * 1024.0));
			} else {
				LOG_W("clip failed: %s", err.c_str());
				clip.reset();
			}

			std::lock_guard lock(mtx);
			if (clip) {
				clip->lastUse = ++useCounter;
				cache[path] = clip;
				// keep the cache bounded; shared_ptr keeps anything still playing alive
				while (cache.size() > 8) {
					auto victim = cache.begin();
					for (auto it = cache.begin(); it != cache.end(); ++it) {
						if (it->second.use_count() == 1 && it->second->lastUse < victim->second->lastUse) victim = it;
					}
					if (victim->second.use_count() > 1) break;
					cache.erase(victim);
				}
			} else {
				cache[path] = nullptr;  // cache negatives so we do not retry every tick
			}
		}
	}

	void Shutdown()
	{
		{
			std::lock_guard lock(mtx);
			quit = true;
		}
		cv.notify_all();
		if (loader.joinable()) loader.join();

		for (int s = 0; s < kSlots; ++s) {
			Destroy(cur[s]);
			outgoing[s].clear();
		}
		if (mv) { mv->DestroyVoice(); mv = nullptr; }
		if (xa) { xa->Release(); xa = nullptr; }
		cache.clear();
		if (comInit) { CoUninitialize(); comInit = false; }
	}
};

AudioSink::AudioSink() : impl_(std::make_unique<Impl>()) {}
AudioSink::~AudioSink() = default;

bool AudioSink::Init(std::string* a_err)
{
	auto& I = *impl_;
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr)) I.comInit = true;
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		I.err = "CoInitializeEx failed";
		if (a_err) *a_err = I.err;
		return false;
	}

	hr = XAudio2Create(&I.xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr) || !I.xa) {
		I.err = "XAudio2Create failed";
		if (a_err) *a_err = I.err;
		I.Shutdown();  // release a partial COM init; there is no device to keep
		return false;
	}
	hr = I.xa->CreateMasteringVoice(&I.mv);
	if (FAILED(hr) || !I.mv) {
		I.err = "CreateMasteringVoice failed (no audio device?)";
		if (a_err) *a_err = I.err;
		I.Shutdown();  // release IXAudio2 + COM instead of leaking them
		return false;
	}

	I.ready = true;
	I.loader = std::thread([&I] { I.LoaderMain(); });
	LOG_I("audio: XAudio2 ready");
	return true;
}

void AudioSink::Shutdown()
{
	if (impl_) impl_->Shutdown();
}

bool AudioSink::Play(int a_slot, const std::string& a_path, float a_fadeSec)
{
	auto& I = *impl_;
	if (!I.ready || a_slot < 0 || a_slot >= kSlots) return false;

	std::shared_ptr<Clip> clip;
	{
		std::lock_guard lock(I.mtx);
		auto it = I.cache.find(a_path);
		if (it != I.cache.end()) {
			clip = it->second;
			if (clip) clip->lastUse = ++I.useCounter;
		} else if (std::find(I.queue.begin(), I.queue.end(), a_path) == I.queue.end()) {
			I.queue.push_back(a_path);
			I.cv.notify_one();
		}
	}

	if (clip) {
		I.pending[a_slot] = false;
		return I.StartVoice(a_slot, clip, a_fadeSec, I.lastPoll);
	}

	I.pending[a_slot] = true;
	I.pendingPath[a_slot] = a_path;
	I.pendingFade[a_slot] = a_fadeSec;
	// A queued clip keeps whatever is currently playing alive: the crossfade
	// starts when the loader hands the clip over, not now.
	return true;
}

void AudioSink::Prefetch(const std::string& a_path)
{
	auto& I = *impl_;
	if (!I.ready) return;
	std::lock_guard lock(I.mtx);
	if (I.cache.find(a_path) != I.cache.end()) return;
	if (std::find(I.queue.begin(), I.queue.end(), a_path) != I.queue.end()) return;
	I.queue.push_back(a_path);
	I.cv.notify_one();
}

void AudioSink::Stop(int a_slot, float a_fadeSec)
{
	auto& I = *impl_;
	if (!I.ready || a_slot < 0 || a_slot >= kSlots) return;
	I.pending[a_slot] = false;
	if (I.cur[a_slot]) {
		auto old = std::move(I.cur[a_slot]);
		old->fadeFrom = old->applied < 0.0f ? 1.0f : old->applied;
		old->fadeTo = 0.0f;
		old->fadeDur = std::max(0.02f, a_fadeSec);
		old->fadeT0 = I.lastPoll;
		I.outgoing[a_slot].push_back(std::move(old));
	}
}

void AudioSink::SetBaseGain(int a_slot, float a_gain, float a_seconds)
{
	if (a_slot < 0 || a_slot >= kSlots) return;
	auto& I = *impl_;
	const float target = std::clamp(a_gain, 0.0f, 4.0f);
	if (target == I.baseTo[a_slot] && I.baseDur[a_slot] > 0.0f) return;  // same ramp, do not restart it
	I.baseFrom[a_slot] = I.base[a_slot];
	I.baseTo[a_slot] = target;
	I.baseDur[a_slot] = std::max(0.0f, a_seconds);
	I.baseT0[a_slot] = I.lastPoll;
	if (I.baseDur[a_slot] <= 0.0f) I.base[a_slot] = target;
}

void AudioSink::SetMasterGain(float a_gain) { impl_->masterTarget = std::clamp(a_gain, 0.0f, 4.0f); }

void AudioSink::Poll(double a_now)
{
	auto& I = *impl_;
	if (!I.ready) return;

	const float dt = I.lastPoll > 0.0 ? static_cast<float>(std::clamp(a_now - I.lastPoll, 0.0, 0.5)) : 0.05f;
	I.lastPoll = a_now;

	// smooth parameter changes so dragging the volume slider does not zipper
	const float tau = 0.12f;
	const float k = tau > 0.0f ? std::min(1.0f, dt / tau) : 1.0f;
	I.master += (I.masterTarget - I.master) * k;

	// slot base gain: explicit equal-power ramp, so a combat duck takes exactly
	// as long as the combat fade-in instead of snapping in one tick
	for (int s = 0; s < kSlots; ++s) {
		if (I.baseDur[s] > 0.0f) {
			const float t = std::clamp(static_cast<float>((a_now - I.baseT0[s]) / I.baseDur[s]), 0.0f, 1.0f);
			const float from = I.baseFrom[s];
			const float to = I.baseTo[s];
			I.base[s] = std::sqrt(from * from * (1.0f - t) + to * to * t);
			if (t >= 1.0f) I.baseDur[s] = 0.0f;
		} else {
			I.base[s] = I.baseTo[s];
		}
	}

	// pick up clips that finished loading
	for (int s = 0; s < kSlots; ++s) {
		if (!I.pending[s]) continue;
		std::shared_ptr<Clip> clip;
		bool                  failed = false;
		{
			std::lock_guard lock(I.mtx);
			auto it = I.cache.find(I.pendingPath[s]);
			if (it != I.cache.end()) {
				clip = it->second;
				if (clip) clip->lastUse = ++I.useCounter;
				else failed = true;
			}
		}
		if (failed) {
			I.pending[s] = false;
			LOG_D("audio: clip unavailable, staying on previous track: %s", I.pendingPath[s].c_str());
		} else if (clip) {
			I.pending[s] = false;
			I.StartVoice(s, clip, I.pendingFade[s], a_now);
		}
	}

	// Gain for one voice = envelope x slot base x master.  The result is eased
	// towards its target so dragging the volume slider never produces a step in
	// the waveform.
	const float kGain = std::min(1.0f, dt / 0.08f);

	auto applyGain = [&](std::unique_ptr<Impl::Voice>& a_voice, float a_base, bool a_retirable) {
		if (!a_voice) return;

		const float target = I.Env(*a_voice, a_now) * a_base * I.master;

		a_voice->smoothed += (target - a_voice->smoothed) * kGain;

		if (a_retirable && target <= 0.003f && a_voice->smoothed <= 0.003f) {
			I.Destroy(a_voice);
			return;
		}
		if (std::fabs(a_voice->smoothed - a_voice->applied) > 0.002f) {
			a_voice->v->SetVolume(a_voice->smoothed);
			a_voice->applied = a_voice->smoothed;
		}
	};

	for (int s = 0; s < kSlots; ++s) {
		applyGain(I.cur[s], I.base[s], false);

		auto& out = I.outgoing[s];
		for (std::size_t i = 0; i < out.size();) {
			applyGain(out[i], I.base[s], true);
			// applyGain destroys the voice when it reached silence
			if (!out[i]) out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
			else ++i;
		}
	}
}

bool AudioSink::ConsumeFinished(int a_slot)
{
	auto& I = *impl_;
	if (!I.ready || a_slot < 0 || a_slot >= kSlots) return false;
	if (!I.cur[a_slot] || I.pending[a_slot]) return false;

	XAUDIO2_VOICE_STATE st{};
	I.cur[a_slot]->v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
	if (st.BuffersQueued == 0 && st.SamplesPlayed > 0) {
		I.Destroy(I.cur[a_slot]);
		return true;
	}
	return false;
}

bool AudioSink::IsPlaying(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots) return false;
	return impl_->cur[a_slot] != nullptr;
}

bool AudioSink::IsPending(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots) return false;
	return impl_->pending[a_slot];
}

bool AudioSink::IsActive(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots) return false;
	return impl_->cur[a_slot] != nullptr || impl_->pending[a_slot] || !impl_->outgoing[a_slot].empty();
}

bool AudioSink::IsFading(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots) return false;
	return !impl_->outgoing[a_slot].empty();
}

std::string AudioSink::CurrentPath(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots || !impl_->cur[a_slot] || !impl_->cur[a_slot]->clip) return {};
	return impl_->cur[a_slot]->clip->path;
}

float AudioSink::CurrentDuration(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots || !impl_->cur[a_slot] || !impl_->cur[a_slot]->clip) return 0.0f;
	return impl_->cur[a_slot]->clip->duration;
}

float AudioSink::PlayheadSec(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots) return 0.0f;
	auto& I = *impl_;
	if (!I.cur[a_slot] || !I.cur[a_slot]->v || !I.cur[a_slot]->clip) return 0.0f;

	XAUDIO2_VOICE_STATE st{};
	I.cur[a_slot]->v->GetState(&st, 0);
	const auto rate = I.cur[a_slot]->clip->fmt.nSamplesPerSec;
	return rate ? static_cast<float>(st.SamplesPlayed) / static_cast<float>(rate) : 0.0f;
}

float AudioSink::DurationSec(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots || !impl_->cur[a_slot] || !impl_->cur[a_slot]->clip) return 0.0f;
	return impl_->cur[a_slot]->clip->duration;
}

float AudioSink::CurrentGain(int a_slot) const
{
	if (a_slot < 0 || a_slot >= kSlots || !impl_->cur[a_slot]) return 0.0f;
	return impl_->cur[a_slot]->applied < 0.0f ? impl_->cur[a_slot]->smoothed : impl_->cur[a_slot]->applied;
}

const char* AudioSink::LastError() const { return impl_->err.c_str(); }

}  // namespace cms
