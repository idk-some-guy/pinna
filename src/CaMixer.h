#pragma once

#include <atomic>
#include "CaConfig.h"
#include "CaVoice.h"
#include "CaHandoff.h"
#include "CaFilter.h"
#include "CaStream.h"

namespace ca {

struct Snapshot
{
	uint32_t serial;
	VoiceSnap voice[kChannels];
	StreamSnap stream[kStreams];
};

struct Shared
{
	TripleBuffer<Snapshot> snapshot;
	std::atomic<uint32_t> finishedId[kChannels];
	std::atomic<int32_t> readerSlot[kChannels];
	std::atomic<uint32_t> latchedSerial;
	std::atomic<uint32_t> activeVoices;
	std::atomic<uint32_t> underruns;
	std::atomic<uint32_t> streamLatency;
	std::atomic<uint32_t> renders;
	ChunkRing ring[kStreams];
	StreamStatus status[kStreams];
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
	VoiceSnap mLast[kChannels];
	Biquad mShelf[kChannels];
	float mShelfHF[kChannels];
	float mShelfRate;
	float mVoiceSend[kMaxBlock];
	StreamReader mReader[kStreams];
};

}
