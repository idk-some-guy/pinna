#pragma once

#include <stdint.h>
#include <atomic>
#include <thread>
#include "CaDrive.h"

namespace ca {

typedef void (*ClockRenderFn)(void *ctx, uint32_t frames, double rate);

class ClockTime
{
public:
	virtual int64_t Now(void) = 0;
	virtual void SleepUntil(int64_t ns) = 0;

protected:
	~ClockTime(void) {}
};

class Clock
{
public:
	enum { kStallMs = 150, kWatchMs = 20, kMaxLagTicks = 8 };

	Clock(void);
	~Clock(void);
	void Start(Drive *drive, ClockRenderFn render, void *ctx, double rate, uint32_t block, ClockTime *time = NULL);
	void Stop(void);
	void SetFormat(double rate, uint32_t block);
	bool Running(void) const { return mThread.joinable(); }
	uint32_t Stalls(void) const { return mStalls.load(std::memory_order_acquire); }
	uint32_t Wakes(void) const { return mWakes.load(std::memory_order_relaxed); }

private:
	void Run(void);
	template<class Time> void Loop(const Time &time);

	Drive *mDrive;
	ClockRenderFn mRender;
	void *mCtx;
	ClockTime *mTime;
	std::atomic<double> mRate;
	std::atomic<uint32_t> mBlock;
	std::atomic<bool> mRun;
	std::atomic<uint32_t> mStalls;
	std::atomic<uint32_t> mWakes;
	std::thread mThread;
};

}
