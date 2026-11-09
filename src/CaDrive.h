#pragma once

#include <stdint.h>
#include <atomic>
#include <thread>

namespace pinna {

class Drive
{
public:
	enum { OWNER_NONE = 0, OWNER_CLOCK, OWNER_PENDING, OWNER_UNIT, OWNER_STALLED };
	enum { SKIP = 0, RENDER, BLOCKED };

	Drive(void) : mWord(OWNER_NONE), mFrames(0), mUnitBeats(0) {}

	int Owner(void) const { return mWord.load(std::memory_order_acquire) & kOwnerMask; }
	uint64_t Frames(void) const { return mFrames.load(std::memory_order_acquire); }
	uint64_t UnitBeats(void) const { return mUnitBeats.load(std::memory_order_acquire); }
	bool UnitHolds(void) const { return (mWord.load(std::memory_order_acquire) & kHolderMask) == HOLD_UNIT; }

	int EnterUnit(void)
	{
		uint32_t w = mWord.load(std::memory_order_acquire);
		for(;;){
			uint32_t o = w & kOwnerMask;
			if(o != OWNER_PENDING && o != OWNER_UNIT && o != OWNER_STALLED)
				return SKIP;
			if(w & kHolderMask)
				return BLOCKED;
			if(mWord.compare_exchange_weak(w, OWNER_UNIT | HOLD_UNIT, std::memory_order_acq_rel, std::memory_order_acquire))
				return RENDER;
		}
	}

	void LeaveUnit(uint32_t frames)
	{
		mFrames.fetch_add(frames, std::memory_order_relaxed);
		mUnitBeats.fetch_add(1, std::memory_order_relaxed);
		mWord.fetch_and(~(uint32_t)kHolderMask, std::memory_order_release);
	}

	bool EnterClock(void)
	{
		uint32_t w = mWord.load(std::memory_order_acquire);
		for(;;){
			uint32_t o = w & kOwnerMask;
			if(o != OWNER_CLOCK && o != OWNER_PENDING && o != OWNER_STALLED)
				return false;
			if(w & kHolderMask)
				return false;
			if(mWord.compare_exchange_weak(w, o | HOLD_CLOCK, std::memory_order_acq_rel, std::memory_order_acquire))
				return true;
		}
	}

	void LeaveClock(uint32_t frames)
	{
		mFrames.fetch_add(frames, std::memory_order_relaxed);
		mWord.fetch_and(~(uint32_t)kHolderMask, std::memory_order_release);
	}

	void ToClock(void) { SetOwner(OWNER_CLOCK); }
	void Arm(void) { SetOwner(OWNER_PENDING); }
	void Halt(void) { SetOwner(OWNER_NONE); }

	bool Stall(void)
	{
		uint32_t w = OWNER_UNIT;
		return mWord.compare_exchange_strong(w, OWNER_STALLED, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	void WaitUnitIdle(void)
	{
		while(UnitHolds())
			std::this_thread::yield();
	}

	void WaitIdle(void)
	{
		while(mWord.load(std::memory_order_acquire) & kHolderMask)
			std::this_thread::yield();
	}

private:
	enum { kOwnerMask = 0xff, kHolderMask = 0xff00, HOLD_CLOCK = 0x100, HOLD_UNIT = 0x200 };

	void SetOwner(uint32_t o)
	{
		uint32_t w = mWord.load(std::memory_order_acquire);
		while(!mWord.compare_exchange_weak(w, (w & kHolderMask) | o, std::memory_order_acq_rel, std::memory_order_acquire))
			;
	}

	std::atomic<uint32_t> mWord;
	std::atomic<uint64_t> mFrames;
	std::atomic<uint64_t> mUnitBeats;
};

}
