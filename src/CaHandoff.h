#pragma once

#include <stdint.h>
#include <atomic>

namespace ca {

template<typename T>
class TripleBuffer
{
public:
	TripleBuffer(void) : mMiddle(1), mBack(2), mFront(0)
	{
		for(int i = 0; i < 3; i++)
			mBuf[i] = T();
	}
	T &writeBuffer(void) { return mBuf[mBack]; }
	void publish(void)
	{
		uint32_t prev = mMiddle.exchange(mBack | kDirty, std::memory_order_acq_rel);
		mBack = prev & kIndex;
	}
	const T &read(void)
	{
		if(mMiddle.load(std::memory_order_acquire) & kDirty){
			uint32_t prev = mMiddle.exchange(mFront, std::memory_order_acq_rel);
			mFront = prev & kIndex;
		}
		return mBuf[mFront];
	}
private:
	enum { kIndex = 3, kDirty = 4 };
	T mBuf[3];
	std::atomic<uint32_t> mMiddle;
	uint32_t mBack;
	uint32_t mFront;
};

inline int
PickPoolSlot(const int8_t *mirrorSlots, const std::atomic<int32_t> *readerSlots, const uint32_t *slotRef, uint32_t latched, int channels, int poolSize, int *cursor)
{
	for(int k = 0; k < poolSize; k++){
		int s = (*cursor + k) % poolSize;
		bool used = (int32_t)(latched - slotRef[s]) <= 0;
		for(int c = 0; c < channels && !used; c++)
			used = mirrorSlots[c] == s || readerSlots[c].load(std::memory_order_acquire) == s;
		if(!used){
			*cursor = (s + 1) % poolSize;
			return s;
		}
	}
	return -1;
}

}
