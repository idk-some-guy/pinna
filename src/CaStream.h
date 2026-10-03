#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stdint.h>
#include "CaConfig.h"
#include "CaDecoder.h"
#include "CaResampler.h"

namespace ca {

enum
{
	kChunkFrames = 1024,
	kRingChunks = 64,
	kWindowFrames = 16384,
	kReaderSlice = 1024,
	kSegments = 256,
	kHistory = 256,
	kWorkerWakeMs = 20,
	kWorkerRetryMs = 2
};

inline bool
GenBefore(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

// Generations start at 1; 0 means no stream.
inline uint32_t
NextGen(uint32_t gen)
{
	return gen + 1 ? gen + 1 : 1;
}

inline uint64_t
MsToFrames(uint32_t ms, uint32_t rate)
{
	return (uint32_t)(float(ms) / 1000.0f * float(rate));
}

inline double
OalStreamStep(uint32_t rate, float outRate)
{
	float pitch = (float)rate / outRate;
	if(pitch > (float)kOalMaxPitch)
		return kOalMaxPitch;
	uint32_t fixed = (uint32_t)(pitch * (float)kOalStepOne);
	return (double)(fixed ? fixed : 1) / kOalStepOne;
}

struct StreamSnap
{
	uint32_t gen;
	uint8_t playing, paused;
	float ll, lr, rl, rr;
};

struct Chunk
{
	uint32_t gen, rate, frames;
	uint8_t eof, loop;
	uint64_t srcFrame, length;
	float pcm[kChunkFrames * 2];
};

class ChunkRing
{
public:
	ChunkRing(uint32_t start = 0) : mHead(start), mTail(start) {}
	Chunk *beginWrite(void)
	{
		uint32_t h = mHead.load(std::memory_order_relaxed);
		if(h - mTail.load(std::memory_order_acquire) >= (uint32_t)kRingChunks)
			return 0;
		return &mChunks[h % kRingChunks];
	}
	void endWrite(void) { mHead.store(mHead.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
	const Chunk *peek(void)
	{
		uint32_t t = mTail.load(std::memory_order_relaxed);
		if(t == mHead.load(std::memory_order_acquire))
			return 0;
		return &mChunks[t % kRingChunks];
	}
	void pop(void) { mTail.store(mTail.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
	uint32_t filled(void) const { return mHead.load(std::memory_order_acquire) - mTail.load(std::memory_order_acquire); }
	uint32_t mark(void) const { return mHead.load(std::memory_order_relaxed); }
	uint32_t head(void) const { return mHead.load(std::memory_order_acquire); }
	uint32_t tail(void) const { return mTail.load(std::memory_order_relaxed); }
	const Chunk *at(uint32_t i) const { return &mChunks[i % kRingChunks]; }
	uint32_t filledSince(uint32_t mark) const
	{
		uint32_t t = mTail.load(std::memory_order_acquire);
		return mHead.load(std::memory_order_relaxed) - ((int32_t)(t - mark) > 0 ? t : mark);
	}
private:
	Chunk mChunks[kRingChunks];
	std::atomic<uint32_t> mHead, mTail;
};

struct StreamStatus
{
	std::atomic<uint64_t> pos, format;
	std::atomic<uint32_t> drainedGen, failedGen, prefilledGen, postedGen;
	StreamStatus(void) : pos(0), format(0), drainedGen(0), failedGen(0), prefilledGen(0), postedGen(0) {}
	bool position(uint32_t gen, uint32_t *frame, uint32_t *rate) const
	{
		uint64_t p = pos.load(std::memory_order_acquire), f = format.load(std::memory_order_acquire);
		if((uint32_t)(p >> 32) != gen || (uint32_t)(f >> 32) != gen || (uint32_t)f == 0)
			return false;
		*frame = (uint32_t)p;
		*rate = (uint32_t)f;
		return true;
	}
};

class StreamReader
{
public:
	StreamReader(void);
	void render(const StreamSnap &p, ChunkRing &ring, StreamStatus &st, std::atomic<uint32_t> &underruns, float outRate, uint32_t latency,
		float *accL, float *accR, uint32_t frames);
private:
	struct Segment { uint64_t at, src; };
	struct Block { uint64_t outStart; double posStart, step; uint32_t advanced; };
	void reset(uint32_t gen);
	void fill(ChunkRing &ring, uint64_t need);
	float fetch(int ch, int64_t idx) const;
	void fade(ChunkRing *paused = 0);
	void fadePaused(ChunkRing &ring);
	uint64_t sourceAt(uint64_t at) const;
	double audibleAt(uint64_t out) const;
	float mWin[2][kWindowFrames];
	uint64_t mWinEnd;
	double mPos, mStep;
	uint32_t mGen, mRate;
	uint64_t mLength;
	bool mStarted, mEof, mDrained, mLoop, mAudible, mFresh;
	float mGain[4], mTarget[4], mInc[4];
	uint32_t mGainLeft;
	float mFade[2][kOalFadeFrames];
	uint32_t mFadeLeft;
	Segment mSeg[kSegments];
	uint32_t mNumSeg;
	Block mHist[kHistory];
	uint32_t mNumHist;
	uint64_t mOut;
};

enum
{
	CMD_OPEN = 1,
	CMD_SEEK,
	CMD_CLOSE
};

struct StreamCommand
{
	uint8_t op, stream, loop;
	uint32_t gen, startMs;
	char path[512];
};

// Create after the shell starts; delete before Shared.
class StreamWorker
{
public:
	StreamWorker(ChunkRing *rings, StreamStatus *status, DecoderFactory factory, bool threaded);
	~StreamWorker(void);
	void post(const StreamCommand &c);
	// Blocks the calling thread until a second of gen is decoded, its open fails, or timeoutMs passes.
	bool waitPrefilled(uint8_t stream, uint32_t gen, int timeoutMs);
	// An open or seek followed by another command for its stream in the same batch is dropped; the last one wins.
	void step(void);
	uint32_t wakes(void) const { return mWakes.load(std::memory_order_relaxed); }
private:
	struct Slot
	{
		Decoder *dec;
		uint32_t gen, rate, mark;
		bool loop, done, ending, prefilled;
		uint64_t length, lead, next, pass, written;
	};
	void run(void);
	void apply(const StreamCommand &c);
	void position(Slot &s, uint32_t ms);
	bool fill(int index);
	bool active(void) const;
	ChunkRing *mRings;
	StreamStatus *mStatus;
	DecoderFactory mFactory;
	Slot mSlot[kStreams];
	std::deque<StreamCommand> mQueue;
	std::atomic<bool> mPending, mQuit;
	std::atomic<uint32_t> mWakes;
	std::mutex mMutex;
	std::condition_variable mCv, mDoneCv;
	std::thread mThread;
	bool mStarved;
};

}
