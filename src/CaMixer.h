#pragma once

#include <atomic>
#include "CaConfig.h"
#include "CaVoice.h"
#include "CaHandoff.h"
#include "CaFilter.h"

namespace ca {

struct StreamSnap
{
	uint32_t gen;
	uint8_t playing, paused;
	float ll, lr, rl, rr;
};

struct Snapshot
{
	uint32_t serial;
	VoiceSnap voice[kChannels];
	StreamSnap stream[kStreams];
	float reverbLevel;
};

struct Shared
{
	TripleBuffer<Snapshot> snapshot;
	std::atomic<uint32_t> finishedId[kChannels];
	std::atomic<int32_t> readerSlot[kChannels];
	std::atomic<uint32_t> latchedSerial;
	std::atomic<uint32_t> activeVoices;
	std::atomic<uint32_t> underruns;
	Shared(void);
};

class Mixer
{
public:
	explicit Mixer(Shared *shared);
	void render(float *dryL, float *dryR, float *send, float *strL, float *strR, uint32_t frames, float outRate);
private:
	Shared *mShared;
	VoiceState mVoice[kChannels];
	Biquad mShelf;
	float mShelfLevel, mShelfRate;
};

}
