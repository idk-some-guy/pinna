#include <chrono>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include "CaConfig.h"
#include "CaClock.h"

namespace ca {

Clock::Clock(void) : mDrive(NULL), mRender(NULL), mCtx(NULL), mRate(kFallbackRate), mBlock(512), mRun(false), mStalls(0), mWakes(0) {}

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
	mRate.store(rate > 0.0 ? rate : (double)kFallbackRate, std::memory_order_release);
	mBlock.store(block > 0 ? block : 512, std::memory_order_release);
}

void
Clock::Run(void)
{
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	typedef std::chrono::steady_clock Steady;
	const Steady::duration stall = std::chrono::milliseconds(kStallMs), watch = std::chrono::milliseconds(kWatchMs);
	Steady::time_point next = Steady::now(), lastCheck = next, quiet = next;
	uint64_t lastBeat = mDrive->UnitBeats();
	bool wasUnit = false;
	while(mRun.load(std::memory_order_acquire)){
		mWakes.fetch_add(1, std::memory_order_relaxed);
		double rate = mRate.load(std::memory_order_acquire);
		uint32_t block = mBlock.load(std::memory_order_acquire);
		uint64_t beat = mDrive->UnitBeats();
		Steady::time_point now = Steady::now();
		bool unit = mDrive->Owner() == Drive::OWNER_UNIT;
		if(unit && wasUnit && beat == lastBeat){
			if(now - quiet >= stall && mDrive->Stall())
				mStalls.fetch_add(1, std::memory_order_acq_rel);
		}else
			quiet = unit && wasUnit ? lastCheck : now;
		wasUnit = unit;
		lastBeat = beat;
		lastCheck = now;
		bool rendered = mDrive->EnterClock();
		if(rendered){
			mRender(mCtx, block, rate);
			mDrive->LeaveClock(block);
		}
		if(!rendered && mDrive->Owner() == Drive::OWNER_UNIT)
			next = Steady::now() + watch;
		else{
			Steady::duration period = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(block / rate));
			next += period;
			now = Steady::now();
			if(now > next + kMaxLagTicks * period)
				next = now;
		}
		std::this_thread::sleep_until(next);
	}
}

}
