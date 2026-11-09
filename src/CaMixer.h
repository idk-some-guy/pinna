#pragma once

#include <atomic>
#include "CaConfig.h"
#include "CaVoice.h"
#include "CaHandoff.h"
#include "CaFilter.h"
#include "CaStream.h"
#include "CaSpatial.h"
#include "CaEnvironment.h"

namespace pinna {

struct EnvSnap
{
	int8_t place;
	uint32_t serial;
	float gain[NUM_ENVS], gainHF[NUM_ENVS];
};

struct EchoSnap
{
	uint32_t delayFrames[kEchoTaps];
	Direction dir[kEchoTaps];
};

struct Snapshot
{
	uint32_t serial;
	uint8_t mode;
	VoiceSnap voice[kChannels];
	StreamSnap stream[kStreams];
	EnvSnap env;
	EchoSnap echo;
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
	std::atomic<int32_t> envPlace;
	ChunkRing ring[kStreams];
	StreamStatus status[kStreams];
	Shared(void);
};

struct SpatialRender
{
	int32_t algorithm;
};

struct RouteSpan
{
	bool before, after;
	uint32_t split;
};

class EnvBank
{
public:
	void init(int place);
	bool observe(const EnvSnap &e);
	void settle(void);
	void split(const float *send, uint32_t from, uint32_t to, const EnvSnap &e, float *const *out, uint32_t frames, float rate);
	const EnvState &state(void) const { return mState; }
private:
	EnvState mState;
	uint32_t mSerial;
	bool mFresh, mFreshGain;
	float mGain[NUM_ENVS], mTarget[NUM_ENVS], mInc[NUM_ENVS], mShelfHF[NUM_ENVS];
	uint32_t mGainLeft[NUM_ENVS];
	Biquad mShelf[NUM_ENVS];
};

class Mixer
{
public:
	explicit Mixer(Shared *shared);
	~Mixer(void);
	Mixer(const Mixer &) = delete;
	Mixer &operator=(const Mixer &) = delete;
	bool render(float *dryL, float *dryR, float *send, float *strL, float *strR, uint32_t frames, float outRate, const SpatialRender *spatial = NULL);
	const float *mono(int c) const { return mMono[c]; }
	Direction direction(int c) const { Direction d = { mDir[c].azimuth, mDir[c].elevation }; return d; }
	uint32_t chunks(void) const { return mChunks; }
	Direction direction(int c, uint32_t k) const { return k < mDirChunks[c] ? mChunkDir[c][k] : direction(c); }
	RouteSpan route(void) const { return mRoute; }
	const EnvSnap &env(void) const { return mEnv; }
	const float *echo(int t) const { return mEchoLive[t] ? mEcho[t] : NULL; }
	const EchoSnap &echoSnap(void) const { return mEchoSnap; }
	uint32_t serial(void) const { return mSeenSerial; }
	const float *modeGain(void) const { return mDipping ? mModeGain : NULL; }
private:
	void SwapRoute(int c);
	float *EchoBus(int t, uint32_t frames);
	bool EchoOn(int c) const;
	void EchoTargets(int c, const VoiceSnap &v, bool started);
	void EchoFadeCapture(int c);
	void EchoReset(int c);
	void MixEchoFade(int c, uint32_t at, uint32_t n, uint32_t frames);
	void MixEcho(int c, uint32_t at, uint32_t n, uint32_t frames);
	void MoveRoute(int c, const VoiceSnap &to);
	bool Silent(void) const;
	Shared *mShared;
	DipState mDip;
	bool mRouteSpatial, mHeadSpatial;
	uint32_t mSeenSerial;
	float mModeGain[kMaxBlock];
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
	RouteSpan mRoute;
	EnvSnap mEnv;
	StreamReader mReader[kStreams];
	float *mEcho[kEchoTaps];
	bool mEchoLive[kEchoTaps];
	float mEchoIn[kMaxBlock];
	float mEchoGain[kChannels][kEchoTaps], mEchoTarget[kChannels][kEchoTaps], mEchoInc[kChannels][kEchoTaps];
	uint32_t mEchoLeft[kChannels];
	float mEchoFade[kChannels][kOalFadeFrames];
	float mEchoFadeGain[kChannels][kEchoTaps];
	uint32_t mEchoFadeLeft[kChannels];
	bool mDipping;
	EchoSnap mEchoSnap;
};

class EchoLines
{
public:
	EchoLines(void);
	~EchoLines(void);
	EchoLines(const EchoLines &) = delete;
	EchoLines &operator=(const EchoLines &) = delete;
	bool allocate(float rate);
	void release(void);
	void cut(void);
	void render(const Mixer &m, int32_t algorithm, float rate, uint32_t from, uint32_t to, uint32_t frames);
	bool live(int t) const { return mLive[t]; }
	const float *out(int t) const { return mOut[t]; }
	Direction direction(int t, uint32_t k) const { return mChunkDir[t][k < mChunks ? k : mChunks > 0 ? mChunks - 1 : 0]; }
private:
	float *mLine[kEchoTaps];
	uint32_t mLength;
	uint32_t mWrite[kEchoTaps], mFilled[kEchoTaps], mQuiet[kEchoTaps];
	EchoHead mHead[kEchoTaps];
	VoiceDirection mDir[kEchoTaps];
	bool mRestart[kEchoTaps], mLive[kEchoTaps];
	uint32_t mSerial;
	uint32_t mChunks;
	Direction mChunkDir[kEchoTaps][kMaxBlock / 64];
	float mGain[kMaxBlock];
	float mOut[kEchoTaps][kMaxBlock];
};

}
