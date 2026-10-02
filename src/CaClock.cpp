#include <chrono>
#include "CaClock.h"

namespace ca {

Clock::Clock(void) : mDrive(NULL), mRender(NULL), mCtx(NULL), mRate(48000.0), mBlock(512), mRun(false), mStalls(0) {}

Clock::~Clock(void)
{
	Stop();
}

void
Clock::Start(Drive *drive, ClockRenderFn render, void *ctx, double rate, uint32_t block)
{
	Stop();
	mDrive = drive;
	mRender = render;
	mCtx = ctx;
	mStalls.store(0, std::memory_order_release);
	SetFormat(rate, block);
	mRun.store(true, std::memory_order_release);
	mThread = std::thread(&Clock::Run, this);
}

void
Clock::Stop(void)
{
	mRun.store(false, std::memory_order_release);
	if(mThread.joinable())
		mThread.join();
}

void
Clock::SetFormat(double rate, uint32_t block)
{
	mRate.store(rate > 0.0 ? rate : 48000.0, std::memory_order_release);
	mBlock.store(block > 0 ? block : 512, std::memory_order_release);
}

void
Clock::Run(void)
{
	typedef std::chrono::steady_clock Steady;
	Steady::time_point next = Steady::now();
	uint64_t lastBeat = mDrive->UnitBeats();
	int idle = 0;
	while(mRun.load(std::memory_order_acquire)){
		double rate = mRate.load(std::memory_order_acquire);
		uint32_t block = mBlock.load(std::memory_order_acquire);
		uint64_t beat = mDrive->UnitBeats();
		if(mDrive->Owner() == Drive::OWNER_UNIT && beat == lastBeat){
			if(++idle >= kStallTicks && mDrive->Stall()){
				mStalls.fetch_add(1, std::memory_order_acq_rel);
				idle = 0;
			}
		}else
			idle = 0;
		lastBeat = beat;
		if(mDrive->EnterClock()){
			mRender(mCtx, block, rate);
			mDrive->LeaveClock(block);
		}
		Steady::duration period = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(block / rate));
		next += period;
		Steady::time_point now = Steady::now();
		if(now > next + kMaxLagTicks * period)
			next = now;
		std::this_thread::sleep_until(next);
	}
}

}
