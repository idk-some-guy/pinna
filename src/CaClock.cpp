#include <chrono>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include "CaConfig.h"
#include "CaClock.h"

namespace pinna {

Clock::Clock(void) : mDrive(NULL), mRender(NULL), mCtx(NULL), mTime(NULL), mRate(kFallbackRate), mBlock(512), mRun(false), mStalls(0), mWakes(0) {}

Clock::~Clock(void)
{
	Stop();
}

void
Clock::Start(Drive *drive, ClockRenderFn render, void *ctx, double rate, uint32_t block, ClockTime *time)
{
	Stop();
	mDrive = drive;
	mRender = render;
	mCtx = ctx;
	mTime = time;
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

typedef std::chrono::steady_clock Steady;

struct SteadyTime
{
	Steady::time_point Now(void) const { return Steady::now(); }
	void SleepUntil(Steady::time_point t) const { std::this_thread::sleep_until(t); }
};

struct InjectedTime
{
	ClockTime *time;
	Steady::time_point Now(void) const { return Steady::time_point(std::chrono::duration_cast<Steady::duration>(std::chrono::nanoseconds(time->Now()))); }
	void SleepUntil(Steady::time_point t) const { time->SleepUntil(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count()); }
};

void
Clock::Run(void)
{
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	if(mTime){
		InjectedTime t = { mTime };
		Loop(t);
	}else
		Loop(SteadyTime());
}

template<class Time>
void
Clock::Loop(const Time &time)
{
	const Steady::duration stall = std::chrono::milliseconds(kStallMs), watch = std::chrono::milliseconds(kWatchMs);
	Steady::time_point next = time.Now(), lastCheck = next, quiet = next;
	uint64_t lastBeat = mDrive->UnitBeats();
	bool wasUnit = false;
	while(mRun.load(std::memory_order_acquire)){
		mWakes.fetch_add(1, std::memory_order_relaxed);
		double rate = mRate.load(std::memory_order_acquire);
		uint32_t block = mBlock.load(std::memory_order_acquire);
		uint64_t beat = mDrive->UnitBeats();
		Steady::time_point now = time.Now();
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
			next = time.Now() + watch;
		else{
			Steady::duration period = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(block / rate));
			next += period;
			now = time.Now();
			if(now > next + kMaxLagTicks * period)
				next = now;
		}
		time.SleepUntil(next);
	}
}

}
