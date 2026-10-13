#pragma once

#include <stdint.h>
#include <atomic>

namespace ca {

class OutputFade
{
public:
	static constexpr float kSeconds = 0.012f;

	OutputFade(void) : mClosing(false), mDone(false), mQuiet(0), mTail(0), mGain(1.0f) {}

	void Close(void)
	{
		mDone.store(false, std::memory_order_relaxed);
		mQuiet.store(0, std::memory_order_relaxed);
		mClosing.store(true, std::memory_order_release);
	}

	bool Closed(void) const { return mDone.load(std::memory_order_acquire); }

	void Restart(bool silent)
	{
		mClosing.store(false, std::memory_order_relaxed);
		mDone.store(false, std::memory_order_relaxed);
		mQuiet.store(0, std::memory_order_relaxed);
		mGain = silent ? 0.0f : 1.0f;
	}

	float Gain(void) const { return mGain; }

	void SetTail(uint32_t frames) { mTail.store(frames, std::memory_order_relaxed); }
	uint32_t Tail(void) const { return mTail.load(std::memory_order_relaxed); }
	uint32_t Quiet(void) const { return mQuiet.load(std::memory_order_relaxed); }

	void Apply(float *const *channel, uint32_t count, uint32_t frames, double rate)
	{
		bool closing = mClosing.load(std::memory_order_acquire);
		if(!closing && mGain >= 1.0f)
			return;
		float step = rate > 0.0 ? 1.0f / (float)(rate * kSeconds) : 1.0f, g = mGain;
		uint32_t zeros = 0;
		for(uint32_t i = 0; i < frames; i++){
			g = closing ? (g > step ? g - step : 0.0f) : (g + step < 1.0f ? g + step : 1.0f);
			zeros += closing && g <= 0.0f;
			for(uint32_t c = 0; c < count; c++)
				channel[c][i] *= g;
		}
		mGain = g;
		if(!closing || g > 0.0f)
			return;
		uint32_t quiet = mQuiet.load(std::memory_order_relaxed) + zeros;
		mQuiet.store(quiet, std::memory_order_relaxed);
		if(quiet >= mTail.load(std::memory_order_relaxed))
			mDone.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> mClosing, mDone;
	std::atomic<uint32_t> mQuiet, mTail;
	float mGain;
};

}
