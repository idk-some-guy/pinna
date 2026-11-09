#pragma once

#include <stdint.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "CaClock.h"

// The clock thread sleeps here until the test moves time past its deadline, so the two never run at once.
class FakeClockTime : public pinna::ClockTime
{
public:
	typedef void (*WakeHook)(void *ctx);

	FakeClockTime(void) : mNow(0), mDeadline(0), mAsleep(false), mReleased(false), mHook(NULL), mHookCtx(NULL) {}

	int64_t Now(void)
	{
		std::lock_guard<std::mutex> l(mLock);
		return mNow;
	}

	void SleepUntil(int64_t t)
	{
		std::unique_lock<std::mutex> l(mLock);
		if(mReleased){
			l.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return;
		}
		if(t <= mNow)
			return;
		mDeadline = t;
		mAsleep = true;
		mCond.notify_all();
		mCond.wait(l, [&]{ return !mAsleep || mReleased; });
		mAsleep = false;
	}

	void SetWakeHook(WakeHook hook, void *ctx)
	{
		mHook = hook;
		mHookCtx = ctx;
	}

	bool AdvanceTo(int64_t t)
	{
		std::unique_lock<std::mutex> l(mLock);
		for(;;){
			if(!mCond.wait_for(l, std::chrono::seconds(10), [&]{ return mAsleep; }))
				return false;
			if(mDeadline > t){
				if(t > mNow)
					mNow = t;
				return true;
			}
			int64_t d = mDeadline;
			if(mHook){
				l.unlock();
				mHook(mHookCtx);
				l.lock();
			}
			mNow = d;
			mAsleep = false;
			mCond.notify_all();
		}
	}

	bool AdvanceBy(double seconds) { return AdvanceTo(Now() + (int64_t)llround(seconds * 1e9)); }

	bool Late(int64_t by)
	{
		std::unique_lock<std::mutex> l(mLock);
		if(!mCond.wait_for(l, std::chrono::seconds(10), [&]{ return mAsleep; }))
			return false;
		mNow = mDeadline + by;
		mAsleep = false;
		mCond.notify_all();
		return mCond.wait_for(l, std::chrono::seconds(10), [&]{ return mAsleep && mDeadline > mNow; });
	}

	int64_t Deadline(void)
	{
		std::lock_guard<std::mutex> l(mLock);
		return mDeadline;
	}

	void Release(void)
	{
		std::lock_guard<std::mutex> l(mLock);
		mReleased = true;
		mCond.notify_all();
	}

private:
	std::mutex mLock;
	std::condition_variable mCond;
	int64_t mNow, mDeadline;
	bool mAsleep, mReleased;
	WakeHook mHook;
	void *mHookCtx;
};
