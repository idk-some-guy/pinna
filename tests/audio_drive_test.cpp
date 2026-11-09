// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_drive_test.cpp src/CaClock.cpp -o build/tests/audio_drive_test && build/tests/audio_drive_test
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <pthread/qos.h>
#include "CaDrive.h"
#include "CaFade.h"
#include "CaClock.h"
#include "support/fake_clock_time.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
TestStates(void)
{
	Drive d;
	CHECK(d.Owner() == Drive::OWNER_NONE);
	CHECK(d.EnterUnit() == Drive::SKIP);
	CHECK(!d.EnterClock());

	d.ToClock();
	CHECK(d.EnterUnit() == Drive::SKIP);
	CHECK(d.EnterClock());
	CHECK(!d.EnterClock());
	d.Arm();
	CHECK(d.EnterUnit() == Drive::BLOCKED);
	d.LeaveClock(256);
	CHECK(d.Frames() == 256);

	CHECK(d.EnterUnit() == Drive::RENDER);
	CHECK(d.Owner() == Drive::OWNER_UNIT);
	CHECK(!d.EnterClock());
	CHECK(!d.Stall());
	d.LeaveUnit(512);
	CHECK(d.Frames() == 768);
	CHECK(!d.EnterClock());

	CHECK(d.Stall());
	CHECK(d.Owner() == Drive::OWNER_STALLED);
	CHECK(d.EnterClock());
	CHECK(d.EnterUnit() == Drive::BLOCKED);
	d.LeaveClock(256);
	CHECK(d.EnterUnit() == Drive::RENDER);
	CHECK(d.Owner() == Drive::OWNER_UNIT);
	d.LeaveUnit(512);

	CHECK(d.EnterUnit() == Drive::RENDER);
	d.ToClock();
	CHECK(d.Owner() == Drive::OWNER_CLOCK);
	CHECK(d.UnitHolds());
	CHECK(!d.EnterClock());
	d.LeaveUnit(512);
	CHECK(!d.UnitHolds());
	d.WaitUnitIdle();
	CHECK(d.EnterUnit() == Drive::SKIP);
	CHECK(d.EnterClock());
	d.LeaveClock(256);

	d.Halt();
	CHECK(d.EnterUnit() == Drive::SKIP);
	CHECK(!d.EnterClock());
	CHECK(d.Frames() == 2048 + 256);
}

enum { BY_CLOCK = 1, BY_UNIT = 2 };

static const int64_t kMs = 1000000;

struct Record
{
	int who;
	int phase;
	int64_t t;
};

struct World
{
	Drive drive;
	FakeClockTime time;
	std::atomic<int> inside;
	std::atomic<int> overlaps;
	std::atomic<int> phase;
	std::atomic<bool> unitCalls;
	std::atomic<bool> unitRun;
	std::atomic<uint32_t> blocked;
	std::vector<Record> log;
	size_t count;
	World(void) : inside(0), overlaps(0), phase(0), unitCalls(true), unitRun(true), blocked(0), log(100000), count(0) {}
};

static void
RenderAs(World *w, int who)
{
	if(w->inside.fetch_add(1, std::memory_order_relaxed) != 0)
		w->overlaps.fetch_add(1, std::memory_order_relaxed);
	if(w->count < w->log.size()){
		Record &r = w->log[w->count++];
		r.who = who;
		r.phase = w->phase.load(std::memory_order_relaxed);
		r.t = w->time.Now();
	}
	std::this_thread::sleep_for(std::chrono::microseconds(300));
	w->inside.fetch_sub(1, std::memory_order_relaxed);
}

static void
ClockRender(void *ctx, uint32_t, double)
{
	RenderAs((World *)ctx, BY_CLOCK);
}

static void
UnitLoop(World *w, std::chrono::microseconds period)
{
	while(w->unitRun.load()){
		if(w->unitCalls.load()){
			int e = w->drive.EnterUnit();
			if(e == Drive::RENDER){
				RenderAs(w, BY_UNIT);
				w->drive.LeaveUnit(256);
			}else if(e == Drive::BLOCKED)
				w->blocked++;
		}
		std::this_thread::sleep_for(period);
	}
}

static bool
WaitFor(const std::function<bool(void)> &done)
{
	auto t0 = std::chrono::steady_clock::now();
	while(!done()){
		if(std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10))
			return false;
		std::this_thread::yield();
	}
	return true;
}

// Each simulated millisecond waits for a unit beat, so a unit thread starved by the machine never reads as a silence.
static bool
Advance(World *w, int ms, bool paced)
{
	bool ok = true;
	for(int i = 0; i < ms && ok; i++){
		uint64_t beat = w->drive.UnitBeats();
		ok = w->time.AdvanceBy(0.001);
		if(paced)
			ok = ok && WaitFor([&]{ return w->drive.UnitBeats() != beat; });
	}
	return ok;
}

static void
TestHandOver(void)
{
	World *w = new World();
	const double rate = 48000.0;
	const uint32_t block = 256;
	const double period = block / rate;
	Clock clock;
	w->drive.ToClock();
	clock.Start(&w->drive, ClockRender, w, rate, block, &w->time);
	std::thread unit(UnitLoop, w, std::chrono::microseconds((int)(period * 0.5e6)));

	w->phase = 1;
	bool ok = Advance(w, 80, false);
	w->phase = 2;
	w->drive.Arm();
	ok = ok && Advance(w, 80, true);
	w->drive.ToClock();
	w->drive.WaitUnitIdle();
	w->phase = 3;
	ok = ok && Advance(w, 80, false);
	w->drive.Arm();
	w->phase = 4;
	ok = ok && Advance(w, 80, true);
	uint32_t stallsBefore = clock.Stalls();
	w->unitCalls = false;
	w->drive.WaitUnitIdle();
	w->phase = 5;
	ok = ok && Advance(w, 250, false);
	uint32_t stallsLost = clock.Stalls();
	w->unitCalls = true;
	w->phase = 6;
	ok = ok && Advance(w, 80, true);
	w->drive.Halt();
	w->drive.WaitIdle();
	w->phase = 7;
	ok = ok && Advance(w, 30, false);
	w->time.Release();
	clock.Stop();
	w->unitRun = false;
	unit.join();

	int by[8][3] = {};
	int64_t lastT = -1, worstGap = 0;
	int worstAt = -1;
	bool clockAfterUnit[8] = {};
	bool unitSeen[8] = {};
	for(size_t i = 0; i < w->count; i++){
		const Record &r = w->log[i];
		by[r.phase][r.who]++;
		if(r.who == BY_UNIT)
			unitSeen[r.phase] = true;
		else if(unitSeen[r.phase])
			clockAfterUnit[r.phase] = true;
		if(lastT >= 0 && r.phase >= 1 && r.phase <= 6 && r.t - lastT > worstGap){
			worstGap = r.t - lastT;
			worstAt = r.phase;
		}
		lastT = r.t;
	}
	int64_t bound = (Clock::kStallMs + Clock::kWatchMs) * kMs + 2 * (int64_t)(period * 1e9);
	printf("MEASURED hand-over (simulated time): renders by phase (clock/unit): 1 %d/%d, 2 %d/%d, 3 %d/%d, 4 %d/%d, 5 %d/%d, 6 %d/%d, after halt %d/%d; stalls %u; blocked unit calls %u; longest gap %.1f ms in phase %d (bound %.1f ms)\n",
		by[1][1], by[1][2], by[2][1], by[2][2], by[3][1], by[3][2], by[4][1], by[4][2], by[5][1], by[5][2], by[6][1], by[6][2], by[7][1], by[7][2],
		clock.Stalls(), w->blocked.load(), worstGap / 1e6, worstAt, bound / 1e6);
	CHECK(ok);
	CHECK(w->overlaps.load() == 0);
	CHECK(by[1][BY_CLOCK] > 0 && by[1][BY_UNIT] == 0);
	CHECK(by[2][BY_UNIT] > 0 && !clockAfterUnit[2]);
	CHECK(by[3][BY_CLOCK] > 0 && by[3][BY_UNIT] == 0);
	CHECK(by[4][BY_UNIT] > 0 && !clockAfterUnit[4]);
	CHECK(by[5][BY_CLOCK] > 0 && stallsLost == stallsBefore + 1);
	CHECK(by[6][BY_UNIT] > 0 && !clockAfterUnit[6] && clock.Stalls() == stallsLost);
	CHECK(by[7][BY_CLOCK] == 0 && by[7][BY_UNIT] == 0);
	CHECK(worstGap <= bound);
	delete w;
}

struct Ticks
{
	FakeClockTime time;
	std::vector<int64_t> at;
	std::vector<uint32_t> frames;
	std::atomic<int> qos;
	Ticks(void) : qos(-1) {}
};

static void
TickRender(void *ctx, uint32_t frames, double)
{
	Ticks *t = (Ticks *)ctx;
	t->qos.store((int)qos_class_self(), std::memory_order_relaxed);
	t->at.push_back(t->time.Now());
	t->frames.push_back(frames);
}

static size_t
CountIn(const std::vector<int64_t> &at, int64_t from, int64_t to)
{
	size_t n = 0;
	for(int64_t t : at)
		n += t >= from && t < to;
	return n;
}

static void
TestClockRate(void)
{
	Ticks *t = new Ticks();
	Drive d;
	Clock clock;
	d.ToClock();
	clock.Start(&d, TickRender, t, 48000.0, 480, &t->time);
	bool ok = t->time.AdvanceTo(1000 * kMs - 1);
	size_t first = t->at.size();
	clock.SetFormat(44100.0, 882);
	ok = ok && t->time.AdvanceTo(2000 * kMs - 1);
	size_t second = t->at.size() - first;
	t->time.Release();
	clock.Stop();
	bool spaced = true;
	for(size_t i = 1; i < first + second; i++)
		spaced = spaced && t->at[i] - t->at[i - 1] == (i <= first ? 10 : 20) * kMs && t->frames[i] == (i < first ? 480u : 882u);
	printf("MEASURED clock (simulated time): %zu blocks of 480 in the first second at 48000 Hz, %zu blocks of 882 in the next at 44100 Hz; %llu frames\n", first, second, (unsigned long long)d.Frames());
	CHECK(ok && first == 100 && second == 50);
	CHECK(spaced && t->frames[0] == 480);
	CHECK(d.Frames() >= 100 * 480 + 50 * 882);
	CHECK(t->qos.load() == (int)QOS_CLASS_USER_INTERACTIVE);
	delete t;
}

static void
TestCatchUp(void)
{
	Ticks *t = new Ticks();
	Drive d;
	Clock clock;
	d.ToClock();
	const int64_t period = 10 * kMs;
	clock.Start(&d, TickRender, t, 48000.0, 480, &t->time);
	bool ok = t->time.AdvanceTo(100 * kMs - 1);
	bool exact = true;
	size_t worst = 0;
	char seen[160] = "";
	for(int k = 0; k <= 16 && ok; k++){
		size_t before = t->at.size();
		int64_t late = t->time.Deadline() + k * period + period / 2;
		ok = t->time.Late(k * period + period / 2);
		size_t burst = CountIn(t->at, late, late + 1);
		exact = exact && t->at.size() - before == burst && burst == (size_t)(k <= Clock::kMaxLagTicks ? k + 1 : 2) && t->time.Deadline() == late + (k <= Clock::kMaxLagTicks ? period / 2 : period);
		worst = burst > worst ? burst : worst;
		size_t len = strlen(seen);
		snprintf(seen + len, sizeof(seen) - len, "%s%zu", k ? " " : "", burst);
		ok = ok && t->time.AdvanceBy(0.1);
	}
	t->time.Release();
	clock.Stop();
	printf("MEASURED catch-up (simulated time): blocks rendered at once after a wake 0.5 to 16.5 periods late: %s\n", seen);
	CHECK(ok && exact);
	CHECK(worst == Clock::kMaxLagTicks + 1);
	delete t;
}

static void
UnitCall(Drive &d, uint32_t block)
{
	if(d.EnterUnit() == Drive::RENDER)
		d.LeaveUnit(block);
}

static void
TestWatchdogTime(void)
{
	static const uint32_t blocks[] = { 64, 512, 4096 };
	for(uint32_t block : blocks){
		Ticks *t = new Ticks();
		Drive d;
		Clock clock;
		const int64_t period = (int64_t)(block / 48000.0 * 1e9);
		d.ToClock();
		clock.Start(&d, TickRender, t, 48000.0, block, &t->time);
		d.Arm();
		bool ok = true;
		int64_t now = 0;
		auto unitFor = [&](int64_t until){
			for(; now < until && ok; now += period){
				ok = t->time.AdvanceTo(now);
				UnitCall(d, block);
			}
			ok = ok && t->time.AdvanceTo(until);
		};
		unitFor(300 * kMs);
		bool owned = d.Owner() == Drive::OWNER_UNIT;
		size_t ownedRenders = t->at.size();
		uint32_t wakes0 = clock.Wakes();
		unitFor(500 * kMs);
		uint32_t ownedWakes = clock.Wakes() - wakes0;
		bool quietClock = t->at.size() == ownedRenders;
		const int64_t shortSilence = (Clock::kStallMs - Clock::kWatchMs) * kMs;
		for(int i = 0; i < 4 && ok; i++){
			int64_t last = now - period;
			ok = t->time.AdvanceTo(last + shortSilence);
			now = last + shortSilence;
			unitFor(now + 150 * kMs);
		}
		uint32_t afterShort = clock.Stalls();
		bool ownedAfterShort = d.Owner() == Drive::OWNER_UNIT && t->at.size() == ownedRenders;
		int64_t last = now - period;
		ok = ok && t->time.AdvanceTo(last + 400 * kMs);
		uint32_t afterLong = clock.Stalls();
		size_t first = ownedRenders;
		int64_t took = first < t->at.size() ? t->at[first] - last : -1;
		bool spaced = t->at.size() > first + 1;
		for(size_t i = first + 1; i < t->at.size(); i++)
			spaced = spaced && t->at[i] - t->at[i - 1] == period;
		now = last + 400 * kMs;
		size_t stalledRenders = t->at.size();
		unitFor(now + 50 * kMs);
		bool back = d.Owner() == Drive::OWNER_UNIT && t->at.size() == stalledRenders;
		d.Halt();
		t->time.Release();
		clock.Stop();
		printf("MEASURED watchdog at %u frames (simulated time): stalls after four %d ms silences %u, after a 400 ms silence %u, the clock rendering %.1f ms after the unit's last call; clock wakes in 200 ms while the unit owns %u\n",
			block, Clock::kStallMs - Clock::kWatchMs, afterShort, afterLong, took / 1e6, ownedWakes);
		CHECK(ok && owned && ownedAfterShort && quietClock);
		CHECK(afterShort == 0);
		CHECK(afterLong == 1);
		CHECK(took >= (Clock::kStallMs - Clock::kWatchMs) * kMs && took < (Clock::kStallMs + Clock::kWatchMs) * kMs);
		CHECK(spaced && back);
		CHECK(ownedWakes == 200 / Clock::kWatchMs);
		delete t;
	}
}

static float
LargestStep(const std::vector<float> &v)
{
	float m = 0.0f;
	for(size_t i = 1; i < v.size(); i++)
		m = std::max(m, std::fabs(v[i] - v[i - 1]));
	return m;
}

static void
TestFade(void)
{
	const double rate = 48000.0;
	const float step = 1.0f / (float)(rate * OutputFade::kSeconds);
	{
		OutputFade f;
		std::vector<float> a(300, 0.5f), b(300, -0.25f);
		float *ch[2] = { a.data(), b.data() };
		std::vector<float> a0 = a, b0 = b;
		f.Apply(ch, 2, 300, rate);
		CHECK(a == a0 && b == b0);
		CHECK(!f.Closed() && f.Gain() == 1.0f);
	}
	{
		OutputFade f;
		std::vector<float> a(700, 1.0f), b(700, -1.0f);
		float *ch[2] = { a.data(), b.data() };
		f.Close();
		CHECK(!f.Closed());
		f.Apply(ch, 2, 100, rate);
		CHECK(!f.Closed() && f.Gain() < 1.0f && f.Gain() > 0.5f);
		float *rest[2] = { a.data() + 100, b.data() + 100 };
		f.Apply(rest, 2, 600, rate);
		CHECK(f.Closed() && f.Gain() == 0.0f);
		CHECK(a[0] < 1.0f && a[0] > 1.0f - 2.0f * step && a[699] == 0.0f && b[699] == 0.0f);
		for(int i = 1; i < 700; i++)
			CHECK(a[i] <= a[i - 1] && b[i] == -a[i]);
		CHECK(LargestStep(a) <= step * 1.001f);
		f.Restart(true);
		CHECK(!f.Closed() && f.Gain() == 0.0f);
		std::vector<float> c(1024, 1.0f), d(1024, 1.0f);
		float *up[2] = { c.data(), d.data() };
		f.Apply(up, 2, 1024, rate);
		CHECK(c[0] > 0.0f && c[0] <= step * 1.001f && c[1023] == 1.0f && f.Gain() == 1.0f && !f.Closed());
		CHECK(LargestStep(c) <= step * 1.001f);
		f.Restart(false);
		CHECK(f.Gain() == 1.0f);
	}
	{
		OutputFade f;
		std::vector<float> a(200, 1.0f), b(200, 1.0f);
		float *ch[2] = { a.data(), b.data() };
		f.Close();
		f.Apply(ch, 2, 200, rate);
		float g = f.Gain();
		f.Restart(true);
		f.Apply(ch, 2, 0, rate);
		CHECK(g < 1.0f && g > 0.0f && f.Gain() == 0.0f);
		f.Close();
		f.Apply(ch, 2, 4, 0.0);
		CHECK(f.Closed());
	}
	{
		OutputFade f;
		std::vector<float> a(2000, 1.0f), b(2000, 1.0f);
		float *ch[2] = { a.data(), b.data() };
		f.SetTail(512);
		f.Close();
		f.Apply(ch, 2, 676, rate);
		uint32_t ramp = 0;
		while(ramp < 676 && a[ramp] != 0.0f)
			ramp++;
		uint32_t zeros = 676 - ramp;
		CHECK(ramp > 500 && zeros > 50 && f.Gain() == 0.0f && !f.Closed() && f.Quiet() == zeros);
		float *more[2] = { a.data() + 676, b.data() + 676 };
		f.Apply(more, 2, 511 - zeros, rate);
		CHECK(!f.Closed() && f.Quiet() == 511);
		f.Apply(more, 2, 1, rate);
		CHECK(f.Closed() && f.Quiet() == 512);
		f.Restart(false);
		CHECK(!f.Closed() && f.Quiet() == 0);
	}
	{
		OutputFade f;
		std::atomic<bool> stop(false);
		std::atomic<uint32_t> blocks(0);
		std::vector<float> seen;
		float peak = 0.0f;
		std::thread render([&] {
			std::vector<float> a(256), b(256);
			float *ch[2] = { a.data(), b.data() };
			while(!stop.load()){
				std::fill(a.begin(), a.end(), 1.0f);
				std::fill(b.begin(), b.end(), 1.0f);
				f.Apply(ch, 2, 256, rate);
				seen.insert(seen.end(), a.begin(), a.end());
				blocks.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		});
		while(blocks.load() < 5)
			std::this_thread::yield();
		f.Close();
		auto t0 = std::chrono::steady_clock::now();
		while(!f.Closed() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		bool landed = f.Closed();
		uint32_t at = blocks.load();
		while(blocks.load() < at + 3)
			std::this_thread::yield();
		stop.store(true);
		render.join();
		for(float v : seen)
			peak = std::max(peak, v);
		CHECK(landed);
		CHECK(peak == 1.0f && seen.back() == 0.0f);
		CHECK(LargestStep(seen) <= step * 1.001f);
	}
	printf("MEASURED fade: %.0f frames at %.0f Hz, largest sample step %.5f\n", rate * OutputFade::kSeconds, rate, step);
}

int
main(void)
{
	TestStates();
	TestFade();
	TestHandOver();
	TestClockRate();
	TestCatchUp();
	TestWatchdogTime();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all drive tests passed\n");
	return 0;
}
