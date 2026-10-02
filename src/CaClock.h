#pragma once

#include <stdint.h>
#include <atomic>
#include <thread>
#include "CaDrive.h"

namespace ca {

typedef void (*ClockRenderFn)(void *ctx, uint32_t frames, double rate);

class Clock
{
public:
	enum { kStallTicks = 4, kMaxLagTicks = 8 };

	Clock(void);
	~Clock(void);
	void Start(Drive *drive, ClockRenderFn render, void *ctx, double rate, uint32_t block);
	void Stop(void);
	void SetFormat(double rate, uint32_t block);
	bool Running(void) const { return mThread.joinable(); }
	uint32_t Stalls(void) const { return mStalls.load(std::memory_order_acquire); }

private:
	void Run(void);

	Drive *mDrive;
	ClockRenderFn mRender;
	void *mCtx;
	std::atomic<double> mRate;
	std::atomic<uint32_t> mBlock;
	std::atomic<bool> mRun;
	std::atomic<uint32_t> mStalls;
	std::thread mThread;
};

}
