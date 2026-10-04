#pragma once

#include <atomic>
#include "CaConfig.h"
#include "CaVoice.h"
#include "CaHandoff.h"
#include "CaFilter.h"
#include "CaStream.h"
#include "CaSpatial.h"

namespace ca {

struct Snapshot
{
	uint32_t serial;
	uint8_t mode;
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

struct SpatialRender
{
	int32_t algorithm;
};

class Mixer
{
public:
	explicit Mixer(Shared *shared);
	~Mixer(void);
	Mixer(const Mixer &) = delete;
	Mixer &operator=(const Mixer &) = delete;
	// With spatial set and the snapshot in spatial mode, positioned voices go to mono() and the dry bus carries the bypass voices.
	bool render(float *dryL, float *dryR, float *send, float *strL, float *strR, uint32_t frames, float outRate, const SpatialRender *spatial = NULL);
	const float *mono(int c) const { return mMono[c]; }
	Direction direction(int c) const { Direction d = { mDir[c].azimuth, mDir[c].elevation }; return d; }
	// The spatial block's chunks of SpatialChunkFrames, each with its own direction per voice.
	uint32_t chunks(void) const { return mChunks; }
	Direction direction(int c, uint32_t k) const { return k < mDirChunks[c] ? mChunkDir[c][k] : direction(c); }
private:
	void SwapRoute(int c);
	Shared *mShared;
	float *mMono[kChannels];
	float mDipGain[kMaxBlock];
	VoiceDirection mDir[kChannels];
	bool mDirLive[kChannels];
	Direction mChunkDir[kChannels][kMaxBlock / 64];
	uint32_t mDirChunks[kChannels];
	uint32_t mChunks;
	bool mRouteMono[kChannels];
	float mTail[kChannels][3][kOalFadeFrames];
	uint32_t mTailLeft[kChannels];
	VoiceState mVoice[kChannels];
	VoiceSnap mLast[kChannels];
	Biquad mShelf[kChannels];
	float mShelfHF[kChannels];
	float mShelfRate;
	float mVoiceSend[kMaxBlock];
	StreamReader mReader[kStreams];
};

}
