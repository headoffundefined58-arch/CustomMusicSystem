// Stand-alone playback probe: decodes a real .xwm and pushes it through the
// exact same sink the plugin uses, without needing the game.  Used to prove the
// RIFF/XWMA path + XAUDIO2_BUFFER_WMA submission still work after a rebuild.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

#include "audio.h"
#include "core.h"
#include "volume.h"

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: playtest <file.xwm> [seconds]\n");
		return 2;
	}
	const double secs = argc > 2 ? std::atof(argv[2]) : 4.0;

	cms::LogOpen("play_test.log");

	cms::AudioSink sink;
	std::string     err;
	if (!sink.Init(&err)) {
		std::printf("INIT FAILED: %s\n", err.c_str());
		return 1;
	}
	sink.SetMasterGain(1.0f);
	sink.SetBaseGain(0, 1.0f);

	// volume wiring: master 50 % through the same object the page writes to
	cms::VolumeMix mix;
	mix.SetMaster(0.5f);

	sink.Play(0, argv[1], 0.2f);

	// One engine tick: the director hands the mixer's master to the sink, then
	// the sink eases every voice towards it.  Mirrored here so this probe
	// exercises the same chain the plugin uses.
	auto step = [&](double a_t) {
		sink.SetMasterGain(mix.MasterEffective());
		sink.Poll(a_t);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	};

	for (double t = 0.0; t < secs; t += 0.05) step(t);

	const float gainHalf = sink.CurrentGain(0);

	// dropping the master to 0 % must take the voice to silence without killing
	// it: the track keeps running, so raising the level resumes in place
	mix.SetMaster(0.0f);
	for (double t = secs; t < secs + 1.2; t += 0.05) step(t);
	const float gainMuted = sink.CurrentGain(0);

	mix.SetMaster(0.5f);
	for (double t = secs + 1.2; t < secs + 2.2; t += 0.05) step(t);
	const float gainBack = sink.CurrentGain(0);

	const bool playing = sink.IsPlaying(0);
	const float dur = sink.CurrentDuration(0);
	const auto  path = sink.CurrentPath(0);
	std::printf("playing=%d  duration=%.1fs  pending=%d\n  %s\n", playing ? 1 : 0, dur, sink.IsPending(0) ? 1 : 0, path.c_str());

	std::printf("gain: 50%% master = %.3f | at 0%% = %.3f | back at 50%% = %.3f\n", gainHalf, gainMuted, gainBack);

	// 0.5 +/- smoothing tolerance; at 0 % it must be inaudible
	const bool ok = playing && dur > 0.5f && gainHalf > 0.45f && gainHalf < 0.55f && gainMuted < 0.01f &&
		gainBack > 0.45f;
	sink.Shutdown();
	std::printf(ok ? "PLAY OK\n" : "PLAY FAILED\n");
	return ok ? 0 : 1;
}
