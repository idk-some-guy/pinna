// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address -fobjc-arc -Wall -Wextra -mmacosx-version-min=14.0 -DPINNA_COREAUDIO -DCA_SHELL_OFFLINE -I src -I src/coreaudio -I tests/support tests/audio_offline/shell_check.mm src/coreaudio/CaOutput.mm src/CaMixer.cpp src/CaStream.cpp src/CaVoice.cpp src/CaResampler.cpp src/CaClock.cpp -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -framework AppKit -o build/tests/shell_check && build/tests/shell_check
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <AudioToolbox/AudioToolbox.h>
#include "CaShellOffline.h"
#include "CaDrive.h"
#include "fake_clock_time.h"

using namespace pinna;

static int failures = 0;

static const uint32_t kFarVoice = kChannels > 30 ? 30 : kChannels - 1;
static const uint32_t kMidVoice = kChannels > 12 ? 12 : kChannels - 2;

static const ShellConfig kCheckNames = { "check:", "check.audio.control" };

static bool
NamedStart(Shared *shared, const char *uid, int32_t pref = OUTPUT_AUTO, int32_t algorithm = kSpatialUseOutputType)
{
	return ShellStart(shared, uid, pref, algorithm, &kCheckNames);
}

static void
Report(bool ok, const char *name)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if(!ok)
		failures++;
}

static std::vector<int16_t>
Sine(int n, float hz, float rate)
{
	std::vector<int16_t> s(n);
	for(int i = 0; i < n; i++)
		s[i] = (int16_t)lrintf(0.5f * 32767.0f * sinf(2.0f * (float)M_PI * hz * i / rate));
	return s;
}

static VoiceSnap
Voice(const std::vector<int16_t> &s, uint32_t playId, float freq, float gainL, float gainR)
{
	VoiceSnap v = {};
	v.data = s.data();
	v.length = (uint32_t)s.size();
	v.loopStart = 0;
	v.loopEnd = (uint32_t)s.size();
	v.playId = playId;
	v.freq = freq;
	v.gainL = gainL;
	v.gainR = gainR;
	v.loop = 1;
	v.playing = 1;
	v.poolSlot = -1;
	return v;
}

static void
Publish(Shared &sh, Snapshot &mirror)
{
	mirror.serial++;
	sh.snapshot.writeBuffer() = mirror;
	sh.snapshot.publish();
}

static double
Db(const std::vector<float> &x, size_t from, size_t to)
{
	double s = 0.0;
	for(size_t i = from; i < to; i++)
		s += (double)x[i] * x[i];
	return 10.0 * log10(s / (double)(to - from) + 1e-30);
}

static float
Peak(const std::vector<float> &x)
{
	float p = 0.0f;
	for(size_t i = 0; i < x.size(); i++)
		p = fmaxf(p, fabsf(x[i]));
	return p;
}

static bool
RenderChunked(const std::vector<int16_t> &s, uint32_t chunk, uint32_t total, std::vector<float> &L, std::vector<float> &R)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[kFarVoice] = Voice(s, 1, 17000.0f, 0.1f, 0.9f);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	L.assign(total, 9.0f);
	R.assign(total, 9.0f);
	for(uint32_t done = 0; ok && done < total; done += chunk){
		if(done == total / 2){
			mirror->voice[3].freq = 25000.0f;
			mirror->voice[3].gainL = 0.3f;
			Publish(*sh, *mirror);
		}
		ShellRenderOffline(&L[done], &R[done], chunk);
	}
	ShellStop();
	delete mirror;
	delete sh;
	return ok;
}

static DeviceEntry
Device(uint32_t id, const char *uid, const char *name)
{
	DeviceEntry d;
	memset(&d, 0, sizeof(d));
	d.id = id;
	strcpy(d.uid, uid);
	strcpy(d.name, name);
	d.outputChannels = 2;
	d.alive = true;
	return d;
}

static DeviceEntry
TypedDevice(uint32_t id, const char *uid, const char *name, uint32_t transport, uint32_t terminal, uint32_t dataSource)
{
	DeviceEntry d = Device(id, uid, name);
	d.transport = transport;
	d.terminal = terminal;
	d.dataSource = dataSource;
	return d;
}

static void
Sleep(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int gBlockedPulls;
static FakeClockTime *gTime;

static void
FakeTimeOn(void)
{
	gTime = new FakeClockTime();
	ShellClockTimeForTest(gTime);
}

static void
StopOnFakeTime(void)
{
	gTime->Release();
	ShellStop();
	ShellClockTimeForTest(NULL);
	delete gTime;
	gTime = NULL;
}

static bool
Elapse(int ms)
{
	return gTime->AdvanceBy(ms / 1000.0);
}

static bool
Pull(std::vector<float> &L, std::vector<float> &R, uint32_t frames)
{
	L.assign(frames, 9.0f);
	R.assign(frames, 9.0f);
	for(uint32_t done = 0; done < frames;){
		uint32_t n = frames - done < 480 ? frames - done : 480;
		if(ShellFakeUnitPullForTest(&L[done], &R[done], n))
			done += n;
		else if(++gBlockedPulls > 1000)
			return false;
	}
	return true;
}

static void
CheckBufferEventWhileRendering(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 10);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	std::atomic<bool> run(true);
	std::atomic<int> pulls(0);
	float peak = 0.0f;
	std::thread render([&]{
		float L[480], R[480];
		while(run.load()){
			if(ShellFakeUnitPullForTest(L, R, 480)){
				pulls.fetch_add(1);
				for(int i = 0; i < 480; i++)
					peak = fmaxf(peak, fabsf(L[i]));
			}
		}
	});
	const uint32_t sizes[] = { 256, 1024, 128, 512, 2048, 1024 };
	for(int k = 0; k < 60; k++){
		ShellBufferEventForTest(sizes[k % 6]);
		Sleep(1);
	}
	while(pulls.load() < 100)
		Sleep(1);
	run.store(false);
	render.join();
	ShellInfo info = ShellGetInfo();
	uint32_t latency = sh->streamLatency.load();
	StopOnFakeTime();
	printf("MEASURED buffer-size events while the unit renders: %d pulls, buffer %u, rate %.0f, latency %u, peak %.3f\n", pulls.load(), info.buffer, info.rate, latency, peak);
	Report(started && info.buffer == 1024 && info.rate == 48000.0 && latency == 2048 && strcmp(info.device, "MacBook Pro Speakers") == 0 && peak > 0.1f,
		"a buffer-size event while the unit renders updates the buffer and latency and keeps the rate");
	delete mirror;
	delete sh;
}

static void
CheckLossAndReturn(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 10);
	uint64_t base = ShellFramesForTest();
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellInfo info = ShellGetInfo();
	bool opened = started && strcmp(info.device, "MacBook Pro Speakers") == 0 && info.rate == 48000.0;
	std::vector<float> A, R, lost, lostR, B;
	Pull(A, R, 4800);
	bool unitOwns = ShellOwnerForTest() == Drive::OWNER_UNIT && Peak(A) > 0.1f;
	uint32_t unitLatency = sh->streamLatency.load();

	list[0].alive = false;
	ShellFakeDevicesForTest(list, rates, 1, 0);
	ShellPostEventForTest(EVENT_DEVICE_GONE, 10);
	info = ShellGetInfo();
	bool clockOwns = ShellOwnerForTest() == Drive::OWNER_CLOCK && strcmp(info.device, "none") == 0;
	lost.assign(480, 9.0f);
	lostR.assign(480, 9.0f);
	bool silentUnit = !ShellFakeUnitPullForTest(&lost[0], &lostR[0], 480) && Peak(lost) == 0.0f && Peak(lostR) == 0.0f;
	uint32_t clockLatency = sh->streamLatency.load();
	uint64_t atLoss = ShellFramesForTest();
	bool elapsed = Elapse(200);
	uint64_t clockFrames = ShellFramesForTest() - atLoss;

	list[0].alive = true;
	ShellFakeDevicesForTest(list, rates, 1, 10);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 10);
	Pull(B, R, 4800);
	uint64_t E = ShellFramesForTest() - base - 4800;
	info = ShellGetInfo();
	bool back = ShellOwnerForTest() == Drive::OWNER_UNIT && strcmp(info.device, "MacBook Pro Speakers") == 0 && info.rebuilds == 2;
	uint32_t backLatency = sh->streamLatency.load();
	StopOnFakeTime();
	Report(unitLatency == 1024 && clockLatency == 0 && backLatency == 1024, "the stream latency follows the output: two buffers on a device, none while the clock drives");
	printf("MEASURED device loss and return: the clock rendered %llu frames in 200 ms of loss (simulated time); the unit resumed at frame %llu; rebuilds %u; unit pulls refused while the clock held the mixer %d\n",
		(unsigned long long)clockFrames, (unsigned long long)E, info.rebuilds, gBlockedPulls);
	Report(opened && unitOwns && clockOwns && silentUnit && back && elapsed && clockFrames >= 9600 - 960 - 512 && clockFrames <= 9600 + 512,
		"a device loss hands the mixer to the clock and the device's return hands it back to the unit");

	Shared *rs = new Shared();
	mirror->serial = 0;
	Publish(*rs, *mirror);
	std::vector<float> ref((size_t)E + 4800 + 4096), refR((size_t)E + 4800 + 4096);
	bool refOk = ShellStartOffline(rs, 48000.0);
	ShellRenderOffline(&ref[0], &refR[0], (uint32_t)ref.size());
	ShellStop();
	const size_t ramp = 600;
	double worst = 0.0;
	for(size_t i = ramp; i < B.size(); i++)
		worst = fmax(worst, fabs(B[i] - ref[E + i]));
	float rampPeak = Peak(std::vector<float>(B.begin(), B.begin() + 100)), refPeak = Peak(std::vector<float>(ref.begin() + E + 100, ref.begin() + E + 4800));
	long bestLag = 0;
	double bestErr = 1e30;
	for(long lag = -4096; lag <= 4096; lag++){
		if((long)E + lag < 0)
			continue;
		double e = 0.0;
		for(size_t i = ramp; i < ramp + 1024; i++)
			e += fabs(B[i] - ref[E + lag + i]);
		if(e < bestErr){
			bestErr = e;
			bestLag = lag;
		}
	}
	printf("MEASURED looping voice after the return against an uninterrupted render: max abs difference %.3g at the frame count, best match %ld frames from it\n", worst, bestLag);
	Report(refOk && worst <= 1e-5 && bestLag == 0, "a looping voice continues across a device loss and return from where the clock left it, once the output has ramped in");
	Report(refOk && rampPeak <= 0.2f * refPeak && refPeak > 0.1f && Peak(std::vector<float>(B.begin() + ramp, B.end())) > 0.5f * refPeak,
		"the output ramps in from silence after the device's return");
	delete rs;
	delete mirror;
	delete sh;
}

static void
CheckSilentUnit(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 10);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	std::vector<float> L, R;
	bool owned = Pull(L, R, 4800) && ShellOwnerForTest() == Drive::OWNER_UNIT && ShellGetInfo().stalls == 0;
	uint64_t f0 = ShellFramesForTest();
	bool elapsed = Elapse(300);
	uint64_t clockFrames = ShellFramesForTest() - f0;
	ShellInfo info = ShellGetInfo();
	bool stolen = ShellOwnerForTest() == Drive::OWNER_STALLED && info.stalls == 1 && info.rebuilds == 0;
	bool back = Pull(L, R, 4800) && ShellOwnerForTest() == Drive::OWNER_UNIT && Peak(L) > 0.1f;
	StopOnFakeTime();
	printf("MEASURED a unit that stops calling with no event: the clock took over (%u takeovers) and rendered %llu frames in 300 ms of simulated time\n", info.stalls, (unsigned long long)clockFrames);
	Report(started && owned && stolen && back && elapsed && clockFrames >= 130 * 48 - 512 && clockFrames <= 170 * 48 + 512, "a unit that stops calling without an event is replaced by the clock within 150 ms and takes back over when it calls again");
	delete mirror;
	delete sh;
}

static void
CheckDeviceless(void)
{
	Shared *sh = new Shared();
	ShellFakeDevicesForTest(NULL, NULL, 0, 0);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellInfo info = ShellGetInfo();
	uint64_t f0 = ShellFramesForTest();
	bool elapsed = Elapse(250);
	uint64_t f1 = ShellFramesForTest();
	double secs = 0.25;
	bool clock = started && ShellOwnerForTest() == Drive::OWNER_CLOCK && strcmp(info.device, "none") == 0;
	printf("MEASURED no device: the clock rendered %llu frames in %.3f s of simulated time (%.0f per second)\n", (unsigned long long)(f1 - f0), secs, (f1 - f0) / secs);
	Report(clock && elapsed && f1 - f0 + 512 >= 12000 && f1 - f0 <= 12000 + 512, "a start with no device succeeds and the clock drives the mixer in real time");

	DeviceEntry list[2] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers"), Device(20, "Broken", "Broken Device") };
	double rates[2] = { 44100.0, 0.0 };
	ShellFakeDevicesForTest(list, rates, 2, 20);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 20);
	info = ShellGetInfo();
	bool failed = ShellOwnerForTest() == Drive::OWNER_CLOCK && strcmp(info.device, "none") == 0;
	f0 = ShellFramesForTest();
	failed = failed && Elapse(60) && ShellFramesForTest() > f0;
	ShellFakeDevicesForTest(list, rates, 2, 10);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 10);
	std::vector<float> L, R;
	Pull(L, R, 960);
	info = ShellGetInfo();
	bool appeared = ShellOwnerForTest() == Drive::OWNER_UNIT && info.rate == 44100.0 && strcmp(info.device, "MacBook Pro Speakers") == 0;
	StopOnFakeTime();
	Report(failed, "a device that fails to start leaves the clock driving the mixer");
	Report(appeared, "a default output that appears after a deviceless start takes over from the clock");
	delete sh;
}

static void
CheckRetry(void)
{
	Shared *sh = new Shared();
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double broken[1] = { 0.0 }, working[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, broken, 1, 10);
	bool started = NamedStart(sh, "default");
	bool silent = ShellOwnerForTest() == Drive::OWNER_CLOCK && strcmp(ShellGetInfo().device, "none") == 0;
	Sleep(300);
	ShellFakeDevicesForTest(list, working, 1, 10);
	Sleep(1000);
	ShellWaitControlForTest();
	std::vector<float> L, R;
	Pull(L, R, 960);
	ShellInfo info = ShellGetInfo();
	bool retried = ShellOwnerForTest() == Drive::OWNER_UNIT && strcmp(info.device, "MacBook Pro Speakers") == 0 && info.rebuilds == 1;
	ShellStop();
	printf("MEASURED retry: a device that refused at start and recovered with no event: owner %s, device \"%s\", rebuilds %u\n",
		retried ? "unit" : "clock", info.device, info.rebuilds);
	Report(started && silent && retried, "a failed open is retried once after about a second");

	sh->underruns.store(0);
	ShellFakeDevicesForTest(list, broken, 1, 10);
	started = NamedStart(sh, "default");
	Sleep(1300);
	ShellWaitControlForTest();
	uint32_t afterRetry = ShellGetInfo().rebuilds;
	Sleep(1300);
	ShellWaitControlForTest();
	uint32_t later = ShellGetInfo().rebuilds;
	bool stillClock = ShellOwnerForTest() == Drive::OWNER_CLOCK;
	ShellFakeDevicesForTest(list, working, 1, 10);
	ShellPostEventForTest(EVENT_DEVICES_CHANGED, 0);
	Pull(L, R, 960);
	info = ShellGetInfo();
	bool anyEvent = ShellOwnerForTest() == Drive::OWNER_UNIT && strcmp(info.device, "MacBook Pro Speakers") == 0;
	ShellStop();
	printf("MEASURED one retry only: rebuilds %u after the retry, %u 1.3 s later, %u after a device-list change\n", afterRetry, later, info.rebuilds);
	Report(started && afterRetry == 1 && later == 1 && stillClock, "a failed retry is not retried again");
	Report(anyEvent && info.rebuilds == 2, "with no current device a device-list change rebuilds while following the default");
	delete sh;
}

static void
CheckRetryGuard(void)
{
	Shared *sh = new Shared();
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double broken[1] = { 0.0 }, working[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, broken, 1, 10);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	StopOnFakeTime();
	FakeTimeOn();
	started = started && NamedStart(sh, "default") && Elapse(0);
	int armedNow = ShellPendingRetriesForTest();
	int ran = Elapse(5000) ? ShellRunDueRetriesForTest() : -1;
	uint32_t rebuilds = ShellGetInfo().rebuilds;
	StopOnFakeTime();
	printf("MEASURED retry guard across a restart: %d retries came due (the rest from stopped sessions), %d armed in the new one, rebuilds %u\n", ran, armedNow, rebuilds);
	Report(started && armedNow == 1 && ran >= 2 && rebuilds == 1, "a retry armed before a stop does nothing in the next session");

	FakeTimeOn();
	started = NamedStart(sh, "default") && Elapse(0);
	int armed = ShellPendingRetriesForTest();
	bool early = Elapse(900) && ShellRunDueRetriesForTest() == 0 && ShellGetInfo().rebuilds == 0;
	bool fired = Elapse(200) && ShellRunDueRetriesForTest() == 1 && ShellGetInfo().rebuilds == 1;
	int afterFailedRetry = ShellPendingRetriesForTest();
	bool quiet = Elapse(3000) && ShellRunDueRetriesForTest() == 0 && ShellGetInfo().rebuilds == 1;
	printf("MEASURED retry guard on fake time: %d retry armed by the failed start, %d left after the failed retry, rebuilds %u 3 s later\n", armed, afterFailedRetry, ShellGetInfo().rebuilds);
	Report(started && armed == 1 && early && fired, "a failed open arms one retry, which runs a second later on the clock's time");
	Report(afterFailedRetry == 0 && quiet, "a failed retry within the session arms no further retry");

	ShellPostEventForTest(EVENT_DEVICES_CHANGED, 0);
	int rearmed = ShellPendingRetriesForTest();
	ShellFakeDevicesForTest(list, working, 1, 10);
	bool recovered = Elapse(1100) && ShellRunDueRetriesForTest() == 1;
	ShellInfo info = ShellGetInfo();
	recovered = recovered && strcmp(info.device, "MacBook Pro Speakers") == 0 && info.rebuilds == 3;
	StopOnFakeTime();
	printf("MEASURED retry guard after a device change: %d retry armed, device \"%s\" after it, rebuilds %u\n", rearmed, info.device, info.rebuilds);
	Report(rearmed == 1 && recovered, "a device change that fails to open arms a new retry, which opens the device once it works");
	delete sh;
}

static void
CheckPin(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[2] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers"), Device(20, "A50-USB", "Astro A50 J\xc3\xb6rg") };
	double rates[2] = { 48000.0, 44100.0 };
	ShellFakeDevicesForTest(list, rates, 2, 10);
	NamedStart(sh, "default");
	uint32_t gen = ShellDeviceListGeneration();
	char name[64], uid[128];
	bool listed = ShellDeviceCount() == 2 && ShellDeviceAt(1, name, sizeof(name), uid, sizeof(uid)) && strcmp(name, "Astro A50 J\xc3\xb6rg") == 0 && strcmp(uid, "A50-USB") == 0 && !ShellDeviceAt(2, name, sizeof(name), uid, sizeof(uid));
	std::vector<float> L, R;
	bool ok = true;
	int step = 0;
	auto expect = [&](const char *device, double rate, uint32_t rebuilds){
		ShellWaitControlForTest();
		Pull(L, R, 960);
		ShellInfo i = ShellGetInfo();
		step++;
		bool good = strcmp(i.device, device) == 0 && i.rate == rate && i.rebuilds == rebuilds && Peak(L) > 0.1f;
		if(!good)
			printf("MEASURED pin step %d: device \"%s\" rate %.0f rebuilds %u peak %.3f\n", step, i.device, i.rate, i.rebuilds, Peak(L));
		ok = ok && good;
	};
	expect("MacBook Pro Speakers", 48000.0, 0);
	ShellSetDevice("A50-USB");
	expect("Astro A50 J\xc3\xb6rg", 44100.0, 1);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 10);
	expect("Astro A50 J\xc3\xb6rg", 44100.0, 1);
	ShellPostEventForTest(EVENT_RATE_CHANGED, 10);
	expect("Astro A50 J\xc3\xb6rg", 44100.0, 1);
	list[1].alive = false;
	ShellFakeDevicesForTest(list, rates, 2, 10);
	ShellPostEventForTest(EVENT_DEVICE_GONE, 20);
	expect("MacBook Pro Speakers", 48000.0, 2);
	list[1].alive = true;
	ShellFakeDevicesForTest(list, rates, 2, 10);
	ShellPostEventForTest(EVENT_DEVICES_CHANGED, 0);
	expect("Astro A50 J\xc3\xb6rg", 44100.0, 3);
	rates[1] = 96000.0;
	ShellFakeDevicesForTest(list, rates, 2, 10);
	ShellPostEventForTest(EVENT_RATE_CHANGED, 20);
	expect("Astro A50 J\xc3\xb6rg", 96000.0, 4);
	ShellPostEventForTest(EVENT_WAKE, 0);
	expect("Astro A50 J\xc3\xb6rg", 96000.0, 5);
	ShellSetDevice("default");
	expect("MacBook Pro Speakers", 48000.0, 6);
	bool regen = ShellDeviceListGeneration() != gen;
	ShellStop();
	Report(listed && regen, "the device list reaches its caller with the names as the system gives them and a generation that moves");
	Report(ok, "a pinned device is used, falls back to the default while missing and is taken back when it returns; rate and wake rebuild it");
	delete mirror;
	delete sh;
}

static void
CheckFailedPull(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	std::vector<float> L(2048), R(2048);
	ShellRenderOffline(&L[0], &R[0], 2048);
	uint32_t before = sh->underruns.load();
	ShellFailPullsForTest();
	ShellRenderOffline(&L[0], &R[0], 2048);
	uint32_t after = sh->underruns.load();
	ShellStop();
	printf("MEASURED failed pulls: underruns %u before, %u after one 2048-frame render with the spatial mixer failing\n", before, after);
	Report(ok && before == 0 && after >= 1 && Peak(L) > 0.1f, "a failed pull counts an underrun and the dry mix still plays");
	delete mirror;
	delete sh;
}

static int
CountInFile(FILE *f, const char *line)
{
	char buf[512];
	int n = 0;
	rewind(f);
	while(fgets(buf, sizeof(buf), f))
		n += strcmp(buf, line) == 0;
	return n;
}

static void
CheckSpatialFailure(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L0, R0;
	bool ok = RenderChunked(s, 512, 32768, L0, R0);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[kFarVoice] = Voice(s, 1, 17000.0f, 0.1f, 0.9f);
	Publish(*sh, *mirror);
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	ShellFailSpatialOnceForTest();
	bool started = ShellStartOffline(sh, 48000.0);
	ShellInfo info = ShellGetInfo();
	std::vector<float> L(32768, 9.0f), R(32768, 9.0f);
	for(uint32_t done = 0; started && done < L.size(); done += 512){
		if(done == L.size() / 2){
			mirror->voice[3].freq = 25000.0f;
			mirror->voice[3].gainL = 0.3f;
			Publish(*sh, *mirror);
		}
		ShellRenderOffline(&L[done], &R[done], 512);
	}
	ShellStop();
	bool again = ShellStartOffline(sh, 48000.0) && !ShellGetInfo().spatialBypassed;
	ShellStop();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	int lines = CountInFile(log, "check: spatial mixer unavailable, classic stereo bypasses it\n");
	fclose(log);
	double diff = 0.0;
	for(size_t i = 0; ok && i < L.size(); i++)
		diff = fmax(diff, fmax(fabs(L[i] - L0[i]), fabs(R[i] - R0[i])));
	printf("MEASURED spatial mixer failing once: bypassed %d, log lines %d, max abs difference %.3g, peak %.3f, next start bypassed %d\n",
		info.spatialBypassed, lines, diff, Peak(L), !again);
	Report(ok && started && info.spatialBypassed && lines == 1 && diff <= 1e-5 && Peak(L) > 0.1f && again,
		"a spatial mixer that fails leaves classic stereo, logged once and rendering as with the mixer");
	delete mirror;
	delete sh;
}

static void
CheckSpatial3DFailure(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L0, R0;
	bool ok = RenderChunked(s, 512, 32768, L0, R0);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].azimuth = -60.0f;
	mirror->voice[3].monoGain = 0.5f;
	mirror->voice[kFarVoice] = Voice(s, 1, 17000.0f, 0.1f, 0.9f);
	mirror->voice[kFarVoice].azimuth = 100.0f;
	mirror->voice[kFarVoice].elevation = 20.0f;
	mirror->voice[kFarVoice].monoGain = 0.5f;
	Publish(*sh, *mirror);
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	ShellFailSpatialOnceForTest(1);
	bool started = ShellStartOffline(sh, 48000.0);
	ShellInfo info = ShellGetInfo();
	bool echoDown = !ShellEchoReady();
	std::vector<float> L(32768, 9.0f), R(32768, 9.0f);
	for(uint32_t done = 0; started && done < L.size(); done += 512){
		if(done == L.size() / 2){
			mirror->voice[3].freq = 25000.0f;
			mirror->voice[3].gainL = 0.3f;
			Publish(*sh, *mirror);
		}
		ShellRenderOffline(&L[done], &R[done], 512);
	}
	ShellStop();
	bool again = ShellStartOffline(sh, 48000.0) && !ShellGetInfo().spatialUnavailable;
	bool echoBack = ShellEchoReady();
	ShellStop();
	bool echoStopped = !ShellEchoReady();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	int lines = CountInFile(log, "check: spatial mixer for positioned voices unavailable, spatial mode uses classic routing\n");
	fclose(log);
	bool same = ok && memcmp(L.data(), L0.data(), L.size() * sizeof(float)) == 0 && memcmp(R.data(), R0.data(), R.size() * sizeof(float)) == 0;
	printf("MEASURED spatial unit failing once: unavailable %d, classic mixer bypassed %d, log lines %d, bit-identical to classic %d, peak %.3f, next start unavailable %d\n",
		info.spatialUnavailable, info.spatialBypassed, lines, same, Peak(L), !again);
	Report(started && info.spatialUnavailable && !info.spatialBypassed && lines == 1 && same && Peak(L) > 0.1f && again && echoDown && echoBack && echoStopped,
		"spatial unit falls back to classic routing when it cannot be created");
	delete mirror;
	delete sh;
}

static float
BlockPeak(const std::vector<float> &x, uint32_t b)
{
	float p = 0.0f;
	for(uint32_t i = b * 512; i < (b + 1) * 512; i++)
		p = fmaxf(p, fabsf(x[i]));
	return p;
}

static void
CheckSpatialPullFailures(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].azimuth = -60.0f;
	mirror->voice[3].monoGain = 0.5f;
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	const uint32_t blocks = 48;
	std::vector<float> L(blocks * 512, 9.0f), R(blocks * 512, 9.0f);
	uint32_t b = 0;
	auto render = [&](uint32_t n) {
		for(uint32_t k = 0; ok && k < n && b < blocks; k++, b++)
			ShellRenderOffline(&L[b * 512], &R[b * 512], 512);
	};
	render(4);
	uint32_t before = sh->underruns.load();
	ShellFailPullsForTest(1, 1);
	render(1);
	uint32_t after = sh->underruns.load();
	bool centre = memcmp(&L[4 * 512], &R[4 * 512], 512 * sizeof(float)) == 0;
	float failedPeak = BlockPeak(L, 4);
	render(1);
	bool oneKept = !ShellGetInfo().spatialUnavailable && BlockPeak(L, 5) > 0.05f;
	printf("MEASURED one failed spatial pull: underruns %u before, %u after, block peak %.3f, left equals right %d\n", before, after, failedPeak, centre);
	Report(ok && after == before + 1 && failedPeak > 0.1f && centre && oneKept, "one failed pull of the spatial unit leaves the positioned voices sounding in that block");

	ShellFailPullsForTest(1, 7);
	render(7);
	render(1);
	ShellFailPullsForTest(1, 7);
	render(7);
	bool sevenKept = !ShellGetInfo().spatialUnavailable;
	render(1);
	ShellFailPullsForTest(1, 8);
	render(8);
	bool dropped = ShellGetInfo().spatialUnavailable;
	uint32_t from = b;
	render(blocks - b);
	float quietest = 1.0f;
	for(uint32_t k = 6; k < blocks; k++)
		quietest = fminf(quietest, fminf(BlockPeak(L, k), BlockPeak(R, k)));
	float pl = 0.0f, pr = 0.0f;
	for(uint32_t k = from + 2; k < blocks; k++){
		pl = fmaxf(pl, BlockPeak(L, k));
		pr = fmaxf(pr, BlockPeak(R, k));
	}
	ShellStop();
	bool again = ShellStartOffline(sh, 48000.0) && !ShellGetInfo().spatialUnavailable;
	ShellStop();
	printf("MEASURED spatial pulls failing: unavailable after 7 in a row %d, after 8 in a row %d, quietest block %.3f, classic left over right %.3f, next start unavailable %d\n",
		!sevenKept, dropped, quietest, pr > 0.0f ? pl / pr : 0.0f, !again);
	Report(ok && sevenKept && dropped && quietest > 0.05f && pr > 0.0f && fabsf(pl / pr - 4.0f) < 0.05f && again,
		"eight failed pulls in a row of the spatial unit end in classic routing with sound");
	delete mirror;
	delete sh;
}

static void
CheckFailedPullLevel(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L[2], R[2];
	for(int m = 0; m < 2; m++){
		Shared *sh = new Shared();
		Snapshot *mirror = new Snapshot();
		mirror->mode = m ? MODE_SPATIAL : MODE_CLASSIC;
		mirror->voice[3] = Voice(s, 1, 22050.0f, 0.5f * 0.595662f, 0.5f * 0.595662f);
		mirror->voice[3].monoGain = 0.5f * kSpatialCalibHeadphones;
		Publish(*sh, *mirror);
		L[m].assign(5 * 512, 9.0f);
		R[m].assign(5 * 512, 9.0f);
		bool ok = ShellStartOffline(sh, 48000.0);
		for(uint32_t b = 0; ok && b < 5; b++){
			if(m && b == 4)
				ShellFailPullsForTest(1, 1);
			ShellRenderOffline(&L[m][b * 512], &R[m][b * 512], 512);
		}
		ShellStop();
		delete mirror;
		delete sh;
	}
	double dl = Db(L[1], 4 * 512, 5 * 512) - Db(L[0], 4 * 512, 5 * 512);
	double dr = Db(R[1], 4 * 512, 5 * 512) - Db(R[0], 4 * 512, 5 * 512);
	printf("MEASURED failed spatial pull against classic, centred voice: left %+.3f dB, right %+.3f dB (classic %.1f dB)\n", dl, dr, Db(L[0], 4 * 512, 5 * 512));
	Report(Db(L[0], 4 * 512, 5 * 512) > -40.0 && fabs(dl) < 0.1 && fabs(dr) < 0.1, "a failed spatial pull plays a centred voice at classic's level within 0.1 dB");
}

typedef void (*PathFn)(double t, float *az, float *el);

static void
FastTurnPath(double t, float *az, float *el)
{
	*az = t < 0.15 ? (float)(1200.0 * t) : 180.0f;
	*el = 0.0f;
}

static void
JumpPath(double t, float *az, float *el)
{
	*az = t < 0.2 ? 0.0f : 180.0f;
	*el = 0.0f;
}

static void
CirclingPath(double t, float *az, float *el)
{
	double a = fmod(120.0 * t, 360.0);
	*az = (float)(a > 180.0 ? a - 360.0 : a);
	*el = (float)(20.0 * sin(M_PI * t));
}

static void
SteppingPath(double t, float *az, float *el)
{
	double a = fmod(40.0 * floor(t * 48000.0 / 4096.0 + 1e-9), 360.0);
	*az = (float)(a > 180.0 ? a - 360.0 : a);
	*el = 0.0f;
}

static bool
RenderPath(PathFn path, uint32_t block, uint32_t total, std::vector<float> &L, std::vector<float> &R)
{
	static std::vector<int16_t> s = Sine(44100, 441.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3] = Voice(s, 1, 22050.0f, 0.4f, 0.4f);
	mirror->voice[3].monoGain = 0.5f;
	path(0.0, &mirror->voice[3].azimuth, &mirror->voice[3].elevation);
	Publish(*sh, *mirror);
	L.assign(total, 9.0f);
	R.assign(total, 9.0f);
	bool ok = ShellStartOffline(sh, 48000.0);
	for(uint32_t done = 0; ok && done < total; done += block){
		path(done / 48000.0, &mirror->voice[3].azimuth, &mirror->voice[3].elevation);
		Publish(*sh, *mirror);
		ShellRenderOffline(&L[done], &R[done], block);
	}
	ShellStop();
	delete mirror;
	delete sh;
	return ok;
}

static int
CountDips(const std::vector<float> &L, const std::vector<float> &R, uint32_t from)
{
	std::vector<double> e;
	for(size_t i = from; i + 64 <= L.size(); i += 64){
		double s = 0.0;
		for(size_t k = i; k < i + 64; k++)
			s += (double)L[k] * L[k] + (double)R[k] * R[k];
		e.push_back(s);
	}
	std::vector<double> sorted = e;
	std::sort(sorted.begin(), sorted.end());
	double floor = sorted[sorted.size() / 2] * 0.01;
	int dips = 0;
	for(size_t w = 0; w < e.size(); w++)
		if(e[w] < floor && (w == 0 || e[w - 1] >= floor))
			dips++;
	return dips;
}

static double
WorstWindowDb(const std::vector<float> &a, const std::vector<float> &b)
{
	double worst = 0.0;
	for(size_t i = 0; i + 2400 <= a.size(); i += 2400)
		worst = fmax(worst, fabs(Db(a, i, i + 2400) - Db(b, i, i + 2400)));
	return worst;
}

static void
CheckSpatialBlockSizes(void)
{
	static const uint32_t blocks[5] = { 256, 512, 1024, 2048, 4096 };
	const uint32_t total = 8 * 4096, circling = 36 * 4096;
	bool ran = true, turnClean = true, jumpOnce = true;
	std::vector<float> L, R, refL, refR;
	ran = RenderPath(CirclingPath, 512, circling, refL, refR);
	double worst = 0.0;
	for(int b = 0; b < 5; b++){
		ran = ran && RenderPath(FastTurnPath, blocks[b], total, L, R);
		int turn = CountDips(L, R, 2048);
		ran = ran && RenderPath(JumpPath, blocks[b], total, L, R);
		int jump = CountDips(L, R, 2048);
		double wl = 0.0, wr = 0.0;
		if(blocks[b] > 512){
			ran = ran && RenderPath(CirclingPath, blocks[b], circling, L, R);
			wl = WorstWindowDb(L, refL);
			wr = WorstWindowDb(R, refR);
			if(blocks[b] >= 2048)
				worst = fmax(worst, fmax(wl, wr));
		}
		printf("MEASURED spatial unit at %u-frame device blocks: turn dips %d, jump dips %d, circling against 512 worst 50 ms window left %.2f dB right %.2f dB\n",
			blocks[b], turn, jump, wl, wr);
		turnClean = turnClean && turn == 0;
		jumpOnce = jumpOnce && jump == 1;
	}
	Report(ran && turnClean, "a 180 degree turn in 0.15 s never dips through the spatial unit at device blocks of 256 to 4096 frames");
	Report(ran && jumpOnce, "a 180 degree jump dips exactly once through the spatial unit at device blocks of 256 to 4096 frames");
	Report(ran && worst < 1.0, "a circling voice at device blocks of 2048 and 4096 frames stays within 1 dB per channel over 50 ms windows of its render at 512");

	std::vector<float> L4, R4;
	ran = RenderPath(SteppingPath, 512, total, L, R) && RenderPath(SteppingPath, 4096, total, L4, R4);
	float diff = 0.0f;
	for(size_t i = 0; i < L.size(); i++)
		diff = fmaxf(diff, fmaxf(fabsf(L[i] - L4[i]), fabsf(R[i] - R4[i])));
	printf("MEASURED voice stepping 40 degrees every 4096 frames, 4096-frame blocks against 512: max abs difference %.3g, peak %.3f\n", diff, Peak(L));
	Report(ran && Peak(L) > 0.05f && diff < 1e-5f, "a 4096-frame device block pulls the spatial unit per chunk, matching 512-frame blocks with the same target updates");
}

static void
CheckHrtfReadingArmedOnlyInSpatial(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 10);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	uint32_t atStart = ShellHrtfArmsForTest();
	ShellPostEventForTest(EVENT_RATE_CHANGED, 10);
	uint32_t classic = ShellHrtfArmsForTest(), rebuilds = ShellGetInfo().rebuilds;
	ShellSetSpatial(true);
	ShellWaitControlForTest();
	uint32_t on = ShellHrtfArmsForTest();
	ShellSetSpatial(true);
	ShellWaitControlForTest();
	uint32_t again = ShellHrtfArmsForTest();
	ShellPostEventForTest(EVENT_RATE_CHANGED, 10);
	uint32_t rebuilt = ShellHrtfArmsForTest();
	ShellSetSpatial(false);
	ShellWaitControlForTest();
	ShellPostEventForTest(EVENT_RATE_CHANGED, 10);
	uint32_t off = ShellHrtfArmsForTest(), rebuildsAll = ShellGetInfo().rebuilds;
	StopOnFakeTime();
	printf("MEASURED personalised HRTF readings armed: %u at a classic start, %u after a classic rebuild, %u when spatial starts, %u when told twice, %u after a spatial rebuild, %u after classic and a rebuild (%u rebuilds)\n",
		atStart, classic, on, again, rebuilt, off, rebuildsAll);
	Report(started && rebuilds == 1 && rebuildsAll == 3 && atStart == 0 && classic == 0 && on == 1 && again == 1 && rebuilt == 2 && off == 2,
		"the personalised HRTF reading is armed only in spatial mode, when it starts and after a rebuild");
	delete mirror;
	delete sh;
}

struct UnitSeen
{
	int32_t type, unitType, algorithm;
	float calib;
	bool unavailable;
};

static UnitSeen
SeeUnit(void)
{
	ShellWaitControlForTest();
	UnitSeen u = { ShellOutputType(), -1, -1, 0.0f, ShellGetInfo().spatialUnavailable };
	if(ShellGetInfo().outputType != u.type)
		u.type = -1;
	ShellSpatialUnitForTest(&u.unitType, &u.algorithm, &u.calib);
	return u;
}

static void
CheckOutputTypeFollowsDevice(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[3] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker),
		TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0),
		TypedDevice(30, "PG32UCDM", "PG32UCDM", kTransportDisplayPort, 0, 0) };
	double rates[3] = { 48000.0, 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 3, 20);
	FakeTimeOn();
	uint32_t base = ShellHrtfArmsForTest();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	UnitSeen a50 = SeeUnit();
	uint32_t arms = ShellHrtfArmsForTest() - base;
	ShellFakeDevicesForTest(list, rates, 3, 30);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 30);
	UnitSeen display = SeeUnit();
	uint32_t armsDisplay = ShellHrtfArmsForTest() - base;
	ShellFakeDevicesForTest(list, rates, 3, 10);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 10);
	UnitSeen mac = SeeUnit();
	uint32_t armsMac = ShellHrtfArmsForTest() - base;
	StopOnFakeTime();

	ShellFakeDevicesForTest(list, rates, 3, 30);
	FakeTimeOn();
	started = started && NamedStart(sh, "default", OUTPUT_HEADPHONES, kSpatialHRTFHQ) && Elapse(0);
	UnitSeen pinned = SeeUnit();
	StopOnFakeTime();
	printf("MEASURED output type per device: A50 %d unit %d calib %.4f, display %d unit %d, MacBook %d unit %d calib %.4f, display with the headphones setting %d unit %d algorithm %d; HRTF arms %u, %u, %u\n",
		a50.type, a50.unitType, a50.calib, display.type, display.unitType, mac.type, mac.unitType, mac.calib, pinned.type, pinned.unitType, pinned.algorithm, arms, armsDisplay, armsMac);
	Report(started && a50.type == OUTPUT_HEADPHONES && a50.unitType == OUTPUT_HEADPHONES && a50.calib == kSpatialCalibHeadphones &&
		a50.algorithm == kSpatialUseOutputType && !a50.unavailable &&
		display.type == OUTPUT_EXTERNAL && display.unitType == -1 && !display.unavailable &&
		mac.type == OUTPUT_BUILTIN && mac.unitType == OUTPUT_BUILTIN && mac.calib == kSpatialCalibBuiltInSpeakers &&
		pinned.type == OUTPUT_HEADPHONES && pinned.unitType == OUTPUT_HEADPHONES && pinned.algorithm == kSpatialHRTFHQ &&
		arms == 1 && armsDisplay == 1 && armsMac == 2,
		"the output type follows the device at start and at a device change, and sets the spatial unit, its algorithm and calib");
	delete mirror;
	delete sh;
}

static void
CheckOutputPrefWhilePlaying(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[2] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0), TypedDevice(40, "Surround", "USB Surround", kTransportUSB, 0, 0) };
	list[1].outputChannels = 6;
	double rates[2] = { 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 2, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	UnitSeen autoType = SeeUnit();
	uint32_t rebuilds0 = ShellGetInfo().rebuilds;
	ShellSetOutputPref(OUTPUT_EXTERNAL);
	UnitSeen external = SeeUnit();
	uint32_t rebuilds1 = ShellGetInfo().rebuilds;
	ShellSetOutputPref(OUTPUT_EXTERNAL);
	SeeUnit();
	uint32_t rebuilds2 = ShellGetInfo().rebuilds;
	ShellSetOutputPref(OUTPUT_BUILTIN);
	UnitSeen builtin = SeeUnit();
	ShellSetOutputPref(OUTPUT_AUTO);
	UnitSeen back = SeeUnit();
	ShellFakeDevicesForTest(list, rates, 2, 40);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 40);
	UnitSeen surround = SeeUnit();
	StopOnFakeTime();
	printf("MEASURED output pref while playing: auto %d unit %d, external %d unit %d (rebuilds %u), same again %u, built-in %d unit %d calib %.4f, auto again %d unit %d, 6 channel USB %d unit %d\n",
		autoType.type, autoType.unitType, external.type, external.unitType, rebuilds1 - rebuilds0, rebuilds2 - rebuilds1, builtin.type, builtin.unitType, builtin.calib,
		back.type, back.unitType, surround.type, surround.unitType);
	Report(started && autoType.type == OUTPUT_HEADPHONES && autoType.unitType == OUTPUT_HEADPHONES &&
		external.type == OUTPUT_EXTERNAL && external.unitType == -1 && rebuilds1 - rebuilds0 == 1 && rebuilds2 == rebuilds1 &&
		builtin.type == OUTPUT_BUILTIN && builtin.unitType == OUTPUT_BUILTIN && builtin.calib == kSpatialCalibBuiltInSpeakers &&
		back.type == OUTPUT_HEADPHONES && back.unitType == OUTPUT_HEADPHONES &&
		surround.type == OUTPUT_HEADPHONES && surround.unitType == OUTPUT_HEADPHONES,
		"an output pref set while playing rebuilds the unit once for its type on the control queue, the same pref again changes nothing, and auto returns to the device's type");
	delete mirror;
	delete sh;
}

static void
CheckHeadTrackingOnHeadphonesOnly(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[3] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker),
		TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0),
		TypedDevice(30, "PG32UCDM", "PG32UCDM", kTransportDisplayPort, 0, 0) };
	double rates[3] = { 48000.0, 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 3, 20);
	ShellSetHeadTracking(false);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	ShellWaitControlForTest();
	int32_t off = ShellHeadTrackingForTest();
	uint32_t rebuiltBase = ShellGetInfo().rebuilds;
	ShellSetHeadTracking(true);
	ShellWaitControlForTest();
	int32_t on = ShellHeadTrackingForTest();
	uint32_t rebuiltOn = ShellGetInfo().rebuilds - rebuiltBase;
	ShellFakeDevicesForTest(list, rates, 3, 30);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 30);
	ShellWaitControlForTest();
	int32_t display = ShellHeadTrackingForTest();
	ShellFakeDevicesForTest(list, rates, 3, 10);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 10);
	ShellWaitControlForTest();
	int32_t mac = ShellHeadTrackingForTest();
	uint32_t macBase = ShellGetInfo().rebuilds;
	ShellSetHeadTracking(true);
	ShellWaitControlForTest();
	int32_t macLive = ShellHeadTrackingForTest();
	uint32_t macRebuilds = ShellGetInfo().rebuilds - macBase;
	ShellFakeDevicesForTest(list, rates, 3, 20);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 20);
	ShellWaitControlForTest();
	int32_t rebuilt = ShellHeadTrackingForTest();
	ShellSetHeadTracking(false);
	ShellWaitControlForTest();
	int32_t switchedOff = ShellHeadTrackingForTest();
	uint32_t offBase = ShellGetInfo().rebuilds;
	ShellSetHeadTracking(false);
	ShellWaitControlForTest();
	uint32_t sameRebuilds = ShellGetInfo().rebuilds - offBase;
	StopOnFakeTime();

	ShellFakeDevicesForTest(list, rates, 3, 20);
	ShellSetHeadTracking(true);
	FakeTimeOn();
	started = started && NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	ShellWaitControlForTest();
	int32_t atStart = ShellHeadTrackingForTest();
	StopOnFakeTime();
	ShellSetHeadTracking(false);
	printf("MEASURED head tracking on the unit: off %d, switched on live %d (rebuilds %u), display %d, MacBook speakers %d (switched on there %d, rebuilds %u), back on headphones %d, switched off %d, off again rebuilds %u, on at start %d\n",
		off, on, rebuiltOn, display, mac, macLive, macRebuilds, rebuilt, switchedOff, sameRebuilds, atStart);
	Report(started && off == 0 && on == 1 && display == -1 && mac == 0 && macLive == 0 && rebuilt == 1 && switchedOff == 0 && atStart == 1,
		"head tracking is set on the spatial unit only for the headphones type, live and at a rebuild, and never for speakers");
	Report(started && rebuiltOn == 1 && macRebuilds == 0 && sameRebuilds == 0,
		"a head tracking change on headphones goes through one rebuild like the algorithm change, and changes on speakers or to the same value rebuild nothing");
	delete mirror;
	delete sh;
}

struct Surround
{
	std::vector<float> ch[8];
	double db[8];
	uint32_t n, unitChannels, unitLayout;
	int32_t type;
	bool ok;
};

static void
Measure(Surround &r, uint32_t from, uint32_t to)
{
	for(uint32_t c = 0; c < r.n; c++)
		r.db[c] = Db(r.ch[c], from, to);
}

static int
LoudestOf(const Surround &r)
{
	int best = 0;
	for(uint32_t c = 1; c < r.n; c++)
		if(r.db[c] > r.db[best])
			best = c;
	return best;
}

static double
LoudestOutside(const Surround &r, int a, int b)
{
	double m = -200.0;
	for(uint32_t c = 0; c < r.n; c++)
		if((int)c != a && (int)c != b)
			m = fmax(m, r.db[c]);
	return m;
}

static double
TotalPower(const Surround &r)
{
	double sum = 0.0;
	for(uint32_t c = 0; c < r.n; c++)
		sum += pow(10.0, r.db[c] / 10.0);
	return sum;
}

static void
PrintDb(const char *what, const Surround &r, const char *const *names)
{
	printf("MEASURED %u channel %s, unit %u channels layout %u, dB:", r.n, what, r.unitChannels, r.unitLayout);
	for(uint32_t c = 0; c < r.n; c++)
		printf(" %s %.1f", names[c], r.db[c]);
	printf("\n");
}

static Surround
RenderSurround(Snapshot *mirror, Shared *sh, int mode, bool streamFed, uint32_t n = 6, uint32_t tag = kLayoutMpeg51A, uint32_t failBlock = 0)
{
	Surround r;
	r.n = n;
	r.ok = true;
	mirror->mode = mode;
	if(streamFed){
		for(int i = 0; i < 40; i++){
			Chunk *c = sh->ring[0].beginWrite();
			if(c == NULL){
				r.ok = false;
				break;
			}
			c->gen = 1;
			c->rate = 48000;
			c->frames = kChunkFrames;
			c->eof = 0;
			c->loop = 0;
			c->srcFrame = (uint64_t)i * kChunkFrames;
			c->length = 0;
			for(int f = 0; f < kChunkFrames; f++){
				float t = (float)(i * kChunkFrames + f) / 48000.0f;
				c->pcm[f * 2] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * t);
				c->pcm[f * 2 + 1] = 0.3f * sinf(2.0f * (float)M_PI * 660.0f * t);
			}
			sh->ring[0].endWrite();
		}
		mirror->stream[0].gen = 1;
		mirror->stream[0].playing = 1;
		mirror->stream[0].ll = 1.0f;
		mirror->stream[0].rr = 1.0f;
	}
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(50, "Surround", "Surround", kTransportHDMI, 0, 0) };
	list[0].outputChannels = n;
	list[0].layoutTag = tag;
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 50);
	FakeTimeOn();
	r.ok = r.ok && NamedStart(sh, "default", OUTPUT_MULTICHANNEL) && Elapse(0);
	ShellSetSpatial(mode == MODE_SPATIAL);
	ShellWaitControlForTest();
	r.type = ShellOutputType();
	const uint32_t blocks = 40, block = 480;
	for(uint32_t c = 0; c < n; c++)
		r.ch[c].assign(blocks * block, 9.0f);
	for(uint32_t b = 0; b < blocks; b++){
		if(failBlock && b == failBlock)
			ShellFailPullsForTest(1, 3);
		float *p[8];
		for(uint32_t c = 0; c < n; c++)
			p[c] = &r.ch[c][b * block];
		for(int tries = 0; !ShellFakeUnitPullMultiForTest(p, n, block); tries++)
			if(tries > 1000){
				r.ok = false;
				break;
			}
	}
	ShellMultichannelForTest(&r.unitChannels, &r.unitLayout);
	StopOnFakeTime();
	if(failBlock)
		Measure(r, failBlock * block, (failBlock + 3) * block);
	else
		Measure(r, 12 * block, blocks * block);
	return r;
}

static Snapshot *
VoiceMirror(const std::vector<int16_t> &s, float az, float ll, float rr)
{
	Snapshot *m = new Snapshot();
	m->voice[3] = Voice(s, 1, 31000.0f, ll, rr);
	m->voice[3].azimuth = az;
	m->voice[3].monoGain = 0.5f;
	return m;
}

static std::vector<int16_t>
Noise(int n, int amp)
{
	std::vector<int16_t> v(n);
	uint32_t x = 12345u;
	for(int i = 0; i < n; i++){
		x = x * 1664525u + 1013904223u;
		v[i] = (int16_t)(((int)(x >> 16) % (2 * amp + 1)) - amp);
	}
	return v;
}

static void
CheckMultichannelLoudness(void)
{
	std::vector<int16_t> sigs[3] = { Sine(30000, 523.0f, 22050.0f), Sine(30000, 2000.0f, 22050.0f), Noise(30000, 12000) };
	const char *what[3] = { "523 Hz sine", "2 kHz sine", "white noise" };
	const char *names[6] = { "L", "R", "C", "LFE", "Ls", "Rs" };
	double worst = 0.0, exact[3];
	bool ok = true;
	for(int k = 0; k < 3; k++){
		Shared *sh = new Shared();
		Snapshot *mirror = VoiceMirror(sigs[k], 0.0f, 0.5f * 0.595662f, 0.5f * 0.595662f);
		Surround c = RenderSurround(mirror, sh, MODE_CLASSIC, false);
		delete mirror;
		delete sh;
		sh = new Shared();
		mirror = VoiceMirror(sigs[k], 0.0f, 0.5f * 0.595662f, 0.5f * 0.595662f);
		mirror->voice[3].monoGain = 0.5f * kSpatialCalibMultichannel;
		Surround p = RenderSurround(mirror, sh, MODE_SPATIAL, false);
		delete mirror;
		delete sh;
		double pc = TotalPower(c), ps = TotalPower(p), dbDiff = 10.0 * log10(ps / pc);
		exact[k] = kSpatialCalibMultichannel * sqrt(pc / ps);
		worst = fmax(worst, fabs(dbDiff));
		printf("MEASURED 5.1 loudness, %s: classic %.2f dB summed over channels, spatial %.2f dB, spatial minus classic %+.3f dB, calib that matches %.4f\n",
			what[k], 10.0 * log10(pc), 10.0 * log10(ps), dbDiff, exact[k]);
		ok = ok && c.ok && p.ok && c.db[0] > -40.0 && p.db[2] > -40.0;
		if(k == 0)
			PrintDb("front voice, spatial", p, names);
	}
	printf("MEASURED 5.1 calib per signal %.4f %.4f %.4f (noise value is the constant), worst loudness difference %.3f dB\n", exact[0], exact[1], exact[2], worst);
	Report(ok && worst < 0.1, "a front voice through the multichannel spatial unit is as loud as classic's centre pan within 0.1 dB, summed over all channels");
}

static void
CheckMultichannelOutput(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	const char *names[6] = { "L", "R", "C", "LFE", "Ls", "Rs" };
	struct Case { const char *what; float az; int want; };
	const Case cases[] = { { "azimuth -110", -110.0f, 4 }, { "azimuth +110", 110.0f, 5 }, { "azimuth 0", 0.0f, 2 } };
	double lfeWorst = -200.0;
	bool voicesOk = true;
	for(const Case &k : cases){
		Shared *sh = new Shared();
		Snapshot *mirror = VoiceMirror(s, k.az, 0.8f, 0.2f);
		Surround r = RenderSurround(mirror, sh, MODE_SPATIAL, false);
		int loud = LoudestOf(r);
		double rest = LoudestOutside(r, k.want, k.want);
		PrintDb(k.what, r, names);
		voicesOk = voicesOk && r.ok && r.unitChannels == 6 && r.unitLayout == kAudioChannelLayoutTag_MPEG_5_1_A && loud == k.want && r.db[k.want] - rest > 20.0;
		lfeWorst = fmax(lfeWorst, r.db[3]);
		delete mirror;
		delete sh;
	}
	Report(voicesOk, "a voice at azimuth -110 sounds in the left surround, at +110 in the right surround and at 0 in the centre on a 5.1 layout");

	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].routeBypass = 1;
	Surround dry = RenderSurround(mirror, sh, MODE_SPATIAL, false);
	PrintDb("bypass voice", dry, names);
	lfeWorst = fmax(lfeWorst, dry.db[3]);
	delete mirror;
	delete sh;
	Report(dry.ok && dry.db[0] > -40.0 && dry.db[1] > -40.0 && LoudestOutside(dry, 0, 1) < -90.0 && dry.db[0] - dry.db[1] > 9.0,
		"the bypass input of the spatial unit sounds only in the front left and right of a 5.1 layout");

	sh = new Shared();
	mirror = new Snapshot();
	Surround str = RenderSurround(mirror, sh, MODE_SPATIAL, true);
	PrintDb("stream", str, names);
	lfeWorst = fmax(lfeWorst, str.db[3]);
	delete mirror;
	delete sh;
	Report(str.ok && str.db[0] > -40.0 && str.db[1] > -40.0 && LoudestOutside(str, 0, 1) < -90.0, "the stream bus sounds only in the front left and right of a 5.1 layout");

	sh = new Shared();
	mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
	Surround classic = RenderSurround(mirror, sh, MODE_CLASSIC, false);
	PrintDb("classic", classic, names);
	lfeWorst = fmax(lfeWorst, classic.db[3]);
	delete mirror;
	delete sh;
	Report(classic.ok && classic.db[0] > -40.0 && classic.db[1] > -40.0 && LoudestOutside(classic, 0, 1) < -90.0 && classic.db[0] - classic.db[1] > 9.0,
		"classic on a 5.1 device stays stereo on the first two channels");

	printf("MEASURED 5.1 loudest LFE over all renders %.1f dB\n", lfeWorst);
	Report(lfeWorst < -60.0, "nothing reaches the LFE channel above -60 dB");
}

static void
CheckMultichannelLayoutPolicy(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	const char *names6[6] = { "L", "R", "C", "LFE", "Ls", "Rs" };
	const char *names8[8] = { "L", "R", "C", "LFE", "Ls", "Rs", "Rls", "Rrs" };

	Shared *sh = new Shared();
	Snapshot *mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
	Surround st = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, kAudioChannelLayoutTag_Stereo);
	PrintDb("device declaring a stereo layout", st, names6);
	printf("MEASURED 6 channel device declaring stereo: output type %d, unit channels %u\n", st.type, st.unitChannels);
	delete mirror;
	delete sh;
	Report(st.ok && st.type == OUTPUT_EXTERNAL && st.unitChannels == 0 && st.db[0] > -40.0 && st.db[1] > -40.0 && LoudestOutside(st, 0, 1) < -90.0,
		"a 6 channel device that declares a stereo layout falls back to its detected output type and stays stereo on the front pair");

	const uint32_t unusedStereo[6] = { kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_Unused, kAudioChannelLabel_Unused, kAudioChannelLabel_Unused, kAudioChannelLabel_Unused };
	const uint32_t unusedQuad[6] = { kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround, kAudioChannelLabel_Unused, kAudioChannelLabel_Unused };
	const uint32_t *described[2] = { unusedStereo, unusedQuad };
	bool describedOk = true;
	for(const uint32_t *labels : described){
		sh = new Shared();
		mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
		Surround d = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, ShellLayoutTagForLabelsForTest(labels, 6));
		PrintDb("device describing unused channels", d, names6);
		describedOk = describedOk && d.ok && d.type == OUTPUT_EXTERNAL && d.unitChannels == 0 && d.db[0] > -40.0 && d.db[1] > -40.0 && LoudestOutside(d, 0, 1) < -90.0;
		delete mirror;
		delete sh;
	}
	Report(describedOk, "a 6 channel device whose speaker description is stereo or quad with unused channels falls back to its detected output type instead of 5.1");

	sh = new Shared();
	mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
	ShellFailSpatialOnceForTest(1);
	Surround rej = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, kAudioChannelLayoutTag_MPEG_5_1_A);
	PrintDb("device declaring a layout the mixer rejects", rej, names6);
	delete mirror;
	delete sh;
	Report(rej.ok && rej.type == OUTPUT_EXTERNAL && rej.unitChannels == 0 && rej.db[0] > -40.0 && rej.db[1] > -40.0 && LoudestOutside(rej, 0, 1) < -90.0,
		"a 6 channel device declaring a layout the spatial mixer rejects falls back to its detected output type on the front pair");

	struct Case { const char *what; float az; int want; };
	const Case cases[] = { { "azimuth -110", -110.0f, 4 }, { "azimuth +110", 110.0f, 5 }, { "azimuth -150", -150.0f, 6 }, { "azimuth +150", 150.0f, 7 } };
	bool ok = true;
	for(const Case &k : cases){
		sh = new Shared();
		mirror = VoiceMirror(s, k.az, 0.8f, 0.2f);
		Surround r = RenderSurround(mirror, sh, MODE_SPATIAL, false, 8, 0);
		PrintDb(k.what, r, names8);
		ok = ok && r.ok && r.type == OUTPUT_MULTICHANNEL && r.unitChannels == 8 && r.unitLayout == kAudioChannelLayoutTag_MPEG_7_1_C && LoudestOf(r) == k.want &&
			r.db[k.want] - LoudestOutside(r, k.want, k.want) > 3.0 && r.db[3] < -60.0;
		delete mirror;
		delete sh;
	}
	Report(ok, "an 8 channel device with no layout runs 7.1 with rear surrounds, a voice behind the listener sounds in the rear surrounds and nothing reaches the LFE");
}

static void
CheckMultichannelFrontPair(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	const char *names[6] = { "C", "L", "R", "Ls", "Rs", "LFE" };
	const uint32_t tag = kAudioChannelLayoutTag_MPEG_5_1_D;

	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].routeBypass = 1;
	Surround dry = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, tag);
	PrintDb("C L R Ls Rs LFE bypass voice", dry, names);
	delete mirror;
	delete sh;
	Report(dry.ok && dry.unitLayout == tag && dry.db[1] > -40.0 && dry.db[2] > -40.0 && LoudestOutside(dry, 1, 2) < -90.0 && dry.db[1] - dry.db[2] > 9.0,
		"on a C L R Ls Rs LFE device the bypass voice sounds only in channels 1 and 2, left louder");

	sh = new Shared();
	mirror = new Snapshot();
	Surround str = RenderSurround(mirror, sh, MODE_SPATIAL, true, 6, tag);
	PrintDb("C L R Ls Rs LFE stream", str, names);
	delete mirror;
	delete sh;
	Report(str.ok && str.db[1] > -40.0 && str.db[2] > -40.0 && LoudestOutside(str, 1, 2) < -90.0, "on a C L R Ls Rs LFE device the stream bus sounds only in channels 1 and 2");

	sh = new Shared();
	mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
	Surround failed = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, tag, 30);
	PrintDb("C L R Ls Rs LFE failed spatial pulls", failed, names);
	delete mirror;
	delete sh;
	Report(failed.ok && failed.db[1] > -40.0 && failed.db[2] > -40.0 && LoudestOutside(failed, 1, 2) < -90.0,
		"on a C L R Ls Rs LFE device a failed spatial pull plays the dry voice on channels 1 and 2 and leaves no prefill in the others");

	sh = new Shared();
	mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.0f, 0.0f);
	mirror->voice[3].azimuth = 0.0f;
	mirror->voice[3].monoGain = 0.0f;
	mirror->voice[3].send = 0.5f;
	mirror->voice[3].sendHF = 1.0f;
	mirror->voice[3].envSend = 0.5f;
	mirror->voice[3].envSendHF = 1.0f;
	mirror->env.place = ENV_OPEN;
	mirror->env.serial = 1;
	for(int u = 0; u < NUM_ENVS; u++)
		mirror->env.gain[u] = mirror->env.gainHF[u] = 1.0f;
	Surround rev = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, tag);
	PrintDb("C L R Ls Rs LFE reverb return", rev, names);
	delete mirror;
	delete sh;
	Report(rev.ok && rev.db[1] > -70.0 && rev.db[2] > -70.0 && LoudestOutside(rev, 1, 2) < -90.0, "on a C L R Ls Rs LFE device the reverb returns sound only in channels 1 and 2");

	sh = new Shared();
	mirror = VoiceMirror(s, -110.0f, 0.8f, 0.2f);
	Surround voices = RenderSurround(mirror, sh, MODE_SPATIAL, false, 6, tag);
	PrintDb("C L R Ls Rs LFE azimuth -110", voices, names);
	delete mirror;
	delete sh;
	Report(voices.ok && LoudestOf(voices) == 3 && voices.db[3] - LoudestOutside(voices, 3, 3) > 20.0, "on a C L R Ls Rs LFE device a voice at azimuth -110 sounds in channel 3, the left surround");
}

static void
CheckOfflineMultichannelPref(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	bool started = NamedStart(sh, "default", OUTPUT_MULTICHANNEL);
	int32_t type = ShellOutputType(), unitType = -1, algorithm = -1;
	float calib = 0.0f;
	ShellSpatialUnitForTest(&unitType, &algorithm, &calib);
	uint32_t channels = 0, tag = 0;
	ShellMultichannelForTest(&channels, &tag);
	ShellStop();
	printf("MEASURED offline start with the multichannel setting: type %d, unit %d, unit channels %u\n", type, unitType, channels);
	Report(started && type != OUTPUT_MULTICHANNEL && unitType != OUTPUT_MULTICHANNEL && channels == 0,
		"the offline start with the multichannel setting takes the same downgrade as a live two channel device and builds no multichannel unit");
	delete mirror;
	delete sh;
}

static void
CheckAlgorithmWhilePlaying(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	UnitSeen before = SeeUnit();
	uint32_t rebuilds0 = ShellGetInfo().rebuilds, arms0 = ShellHrtfArmsForTest();
	ShellSetAlgorithm(kSpatialHRTFHQ);
	UnitSeen hq = SeeUnit();
	uint32_t rebuilds1 = ShellGetInfo().rebuilds, arms1 = ShellHrtfArmsForTest();
	ShellSetAlgorithm(kSpatialHRTFHQ);
	SeeUnit();
	uint32_t rebuilds2 = ShellGetInfo().rebuilds;
	ShellSetAlgorithm(kSpatialUseOutputType);
	UnitSeen back = SeeUnit();
	StopOnFakeTime();
	printf("MEASURED spatial algorithm while playing: auto %d, then %d (rebuilds %u, HRTF arms %u), the same again rebuilds %u, back %d\n",
		before.algorithm, hq.algorithm, rebuilds1 - rebuilds0, arms1 - arms0, rebuilds2 - rebuilds1, back.algorithm);
	Report(started && before.algorithm == kSpatialUseOutputType && hq.algorithm == kSpatialHRTFHQ && hq.unitType == OUTPUT_HEADPHONES &&
		rebuilds1 - rebuilds0 == 1 && arms1 - arms0 == 1 && rebuilds2 == rebuilds1 && back.algorithm == kSpatialUseOutputType,
		"a spatial algorithm set while playing rebuilds the output once with the new algorithm on the unit, and the same algorithm again changes nothing");
	delete mirror;
	delete sh;
}

struct MonoTap
{
	uint64_t base;
	std::vector<uint64_t> at;
	std::vector<float> mono;
};

static void
TapMono(void *ctx, const Mixer &m, bool, const float *, const float *, uint32_t frames)
{
	MonoTap *t = (MonoTap *)ctx;
	t->at.push_back(ShellFramesForTest() - t->base);
	t->mono.insert(t->mono.end(), m.mono(3), m.mono(3) + frames);
}

static void
CheckRebuildKeepsSpatialRouting(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].azimuth = -60.0f;
	mirror->voice[3].monoGain = 0.5f;
	Publish(*sh, *mirror);
	DeviceEntry list[2] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0),
		TypedDevice(30, "PG32UCDM", "PG32UCDM", kTransportDisplayPort, 0, 0) };
	double rates[2] = { 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 2, 20);
	MonoTap tap;
	tap.base = ShellFramesForTest();
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellWaitControlForTest();
	std::vector<float> L, R;
	bool pulled = Pull(L, R, 4800);
	uint64_t classic0 = ShellClassicFramesForTest(), frames0 = ShellFramesForTest();
	ShellBlockTapForTest(TapMono, &tap);
	size_t from = tap.mono.size();
	ShellSetAlgorithm(kSpatialHRTFHQ);
	ShellWaitControlForTest();
	bool ran = Elapse(30);
	pulled = pulled && Pull(L, R, 4800);
	ShellPostEventForTest(EVENT_RATE_CHANGED, 20);
	ran = ran && Elapse(30);
	pulled = pulled && Pull(L, R, 4800);
	ShellBlockTapForTest(NULL, NULL);
	uint64_t clockFrames = ShellFramesForTest() - frames0 - 9600, classic = ShellClassicFramesForTest() - classic0;
	uint32_t rebuilds = ShellGetInfo().rebuilds;
	ShellFakeDevicesForTest(list, rates, 2, 30);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 30);
	ran = ran && Elapse(30);
	pulled = pulled && Pull(L, R, 4800);
	uint64_t external = ShellClassicFramesForTest() - classic0 - classic;
	StopOnFakeTime();

	Shared *rs = new Shared();
	mirror->serial = 0;
	Publish(*rs, *mirror);
	Mixer *mx = new Mixer(rs);
	SpatialRender sr = { kSpatialUseOutputType };
	std::vector<float> ref;
	float dL[480], dR[480], snd[480], sL[480], sR[480];
	uint64_t need = tap.at.empty() ? 0 : tap.at.back() + 480;
	for(uint64_t done = 0; done < need; done += 480){
		mx->render(dL, dR, snd, sL, sR, 480, 48000.0f, &sr);
		ref.insert(ref.end(), mx->mono(3), mx->mono(3) + 480);
	}
	delete mx;
	delete rs;
	double worst = tap.at.size() > from ? 0.0 : 1.0;
	float peak = 0.0f;
	for(size_t b = from; b < tap.at.size(); b++)
		for(size_t i = 0; i < 480; i++){
			worst = fmax(worst, fabs(tap.mono[b * 480 + i] - ref[tap.at[b] + i]));
			peak = fmaxf(peak, fabsf(tap.mono[b * 480 + i]));
		}
	printf("MEASURED rebuilds in spatial mode (algorithm toggle, rate change): %llu classic-routed frames, %llu frames on the clock, %u rebuilds; voice mono bus against an uninterrupted mixer: max abs difference %.3g over %zu blocks, peak %.3f; then a rebuild onto external speakers: %llu classic-routed frames\n",
		(unsigned long long)classic, (unsigned long long)clockFrames, rebuilds, worst, tap.at.size() - from, peak, (unsigned long long)external);
	Report(started && ran && pulled && rebuilds == 2 && clockFrames >= 2048 && classic == 0 && external > 0,
		"a rebuild in spatial mode keeps spatial routing while the clock drives, and only external speakers route classic");
	Report(started && ran && pulled && peak > 0.1f && worst <= 1e-5, "a positioned voice's mono bus continues across an algorithm toggle and a rate change as an uninterrupted mixer renders it");
	delete mirror;
	delete sh;
}

static void
CheckRebuildClearsFailedPulls(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	mirror->voice[3].azimuth = -60.0f;
	mirror->voice[3].monoGain = 0.5f;
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellWaitControlForTest();
	std::vector<float> L, R;
	bool pulled = Pull(L, R, 4800);
	ShellFailPullsForTest(1, 8);
	pulled = pulled && Pull(L, R, 8 * 480);
	bool failed = ShellGetInfo().spatialUnavailable;
	ShellSetAlgorithm(kSpatialHRTFHQ);
	ShellWaitControlForTest();
	bool toggled = !ShellGetInfo().spatialUnavailable;
	ShellFailPullsForTest(1, 8);
	pulled = pulled && Pull(L, R, 8 * 480);
	bool failedAgain = ShellGetInfo().spatialUnavailable;
	ShellPostEventForTest(EVENT_RATE_CHANGED, 20);
	bool rate = !ShellGetInfo().spatialUnavailable;
	uint64_t classic0 = ShellClassicFramesForTest();
	pulled = pulled && Pull(L, R, 4800);
	bool spatialAgain = ShellClassicFramesForTest() - classic0 < 4800;
	StopOnFakeTime();
	printf("MEASURED eight failed pulls then a rebuild: unavailable %d, after an algorithm toggle %d, failed again %d, after a rate change %d, spatial routing back %d\n",
		failed, !toggled, failedAgain, !rate, spatialAgain);
	Report(started && pulled && failed && toggled && failedAgain && rate && spatialAgain, "a successful rebuild after eight failed pulls brings spatial routing back");
	delete mirror;
	delete sh;
}

struct UnitTap
{
	std::vector<float> envL, envR, caveL, caveR;
};

static void
TapUnits(void *ctx, const float *pl, const float *pr, const float *cl, const float *cr, const float *, uint32_t frames)
{
	UnitTap *t = (UnitTap *)ctx;
	t->envL.insert(t->envL.end(), pl, pl + frames);
	t->envR.insert(t->envR.end(), pr, pr + frames);
	t->caveL.insert(t->caveL.end(), cl, cl + frames);
	t->caveR.insert(t->caveR.end(), cr, cr + frames);
}

static Snapshot *
SendingMirror(const std::vector<int16_t> &s, uint8_t mode)
{
	Snapshot *m = new Snapshot();
	m->mode = mode;
	m->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	m->voice[3].azimuth = -60.0f;
	m->voice[3].monoGain = 0.5f;
	m->voice[3].send = 0.3f;
	m->voice[3].sendHF = 1.0f;
	m->voice[3].envSend = 0.5f;
	m->voice[3].envSendHF = 1.0f;
	m->env.place = ENV_OPEN;
	m->env.serial = 1;
	for(int u = 0; u < NUM_ENVS; u++)
		m->env.gain[u] = m->env.gainHF[u] = 1.0f;
	return m;
}

static bool
RenderUnits(uint8_t mode, uint32_t block, uint32_t total, UnitTap &tap, std::vector<float> &L, std::vector<float> &R, int place = ENV_TUNNEL, uint32_t *underruns = NULL)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, mode);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	ShellEnvTapForTest(TapUnits, &tap);
	L.assign(total, 9.0f);
	R.assign(total, 9.0f);
	for(uint32_t done = 0; ok && done < total; done += block){
		if(done == 4096 || done == 8192){
			mirror->env.place = (int8_t)place;
			mirror->env.serial++;
			Publish(*sh, *mirror);
		}
		ShellRenderOffline(&L[done], &R[done], block);
	}
	ShellEnvTapForTest(NULL, NULL);
	ShellStop();
	if(underruns)
		*underruns = sh->underruns.load();
	delete mirror;
	delete sh;
	return ok;
}

static float
SpanPeak(const std::vector<float> &x, size_t a, size_t b)
{
	float p = 0.0f;
	for(size_t i = a; i < b && i < x.size(); i++)
		p = fmaxf(p, fabsf(x[i]));
	return p;
}

static void
CheckEnvBlockSizes(void)
{
	const uint32_t total = 32 * 4096;
	UnitTap big, small;
	std::vector<float> L, R;
	bool ok = RenderUnits(MODE_SPATIAL, 4096, total, big, L, R) && RenderUnits(MODE_SPATIAL, 256, total, small, L, R);
	float diff = big.envL.size() == total && small.envL.size() == total ? 0.0f : 1.0f;
	for(size_t i = 0; diff < 1.0f && i < total; i++)
		diff = fmaxf(diff, fmaxf(fabsf(big.envL[i] - small.envL[i]), fabsf(big.envR[i] - small.envR[i])));
	printf("MEASURED reverb units through a place change, 4096-frame blocks against 256: max abs difference %.3g, wet peak %.3f\n", diff, SpanPeak(big.envL, 0, total));
	Report(ok && SpanPeak(big.envL, 0, total) > 0.01f && diff < 1e-5f, "a place change in 4096-frame blocks gives the same wet output as in 256-frame blocks");
}

static void
CheckEnvFailsToBuild(void)
{
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	UnitTap failed, again;
	std::vector<float> L, R, L2, R2;
	ShellFailSpatialOnceForTest(2 + NUM_ENVS - 1);
	uint32_t underruns = 0;
	bool ok = RenderUnits(MODE_SPATIAL, 512, 16384, failed, L, R, ENV_TUNNEL, &underruns);
	ok = RenderUnits(MODE_SPATIAL, 512, 16384, again, L2, R2) && ok;
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	int lines = CountInFile(log, "check: reverb units unavailable, spatial mode has no reverb\n");
	fclose(log);
	printf("MEASURED reverb unit failing to build: wet peak %.3g, output peak %.3f, underruns %u, log lines %d, wet peak at the next start %.3f\n",
		SpanPeak(failed.envL, 0, failed.envL.size()), Peak(L), underruns, lines, SpanPeak(again.envL, 0, again.envL.size()));
	Report(ok && underruns == 0 && failed.envL.size() == 16384 && SpanPeak(failed.envL, 0, 16384) == 0.0f && SpanPeak(failed.envR, 0, 16384) == 0.0f && Peak(L) > 0.1f && lines == 1 &&
		SpanPeak(again.envL, 0, again.envL.size()) > 0.01f, "a reverb unit that fails to build silences spatial reverb, not the voices");
}

static void
CheckEnvUnitPullFails(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, MODE_SPATIAL);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	UnitTap tap;
	ShellEnvTapForTest(TapUnits, &tap);
	std::vector<float> L(14 * 512, 9.0f), R(14 * 512, 9.0f);
	for(uint32_t b = 0; ok && b < 8; b++)
		ShellRenderOffline(&L[b * 512], &R[b * 512], 512);
	uint32_t before = sh->underruns.load();
	ShellFailPullsForTest(2 + ENV_NARROW_STREET, 1);
	ShellRenderOffline(&L[8 * 512], &R[8 * 512], 512);
	uint32_t idle = sh->underruns.load();
	ShellRenderOffline(&L[9 * 512], &R[9 * 512], 512);
	ShellFailPullsForTest(2 + ENV_OPEN, 1);
	ShellRenderOffline(&L[10 * 512], &R[10 * 512], 512);
	uint32_t sounding = sh->underruns.load();
	for(uint32_t b = 11; ok && b < 14; b++)
		ShellRenderOffline(&L[b * 512], &R[b * 512], 512);
	ShellEnvTapForTest(NULL, NULL);
	ShellStop();
	float wetBefore = SpanPeak(tap.envL, 7 * 512, 8 * 512);
	float wetIdle = SpanPeak(tap.envL, 8 * 512, 9 * 512), wetSounding = fmaxf(SpanPeak(tap.envL, 10 * 512, 11 * 512), SpanPeak(tap.envR, 10 * 512, 11 * 512));
	float wetAfter = SpanPeak(tap.envL, 11 * 512, 12 * 512);
	printf("MEASURED one failed pull of a reverb unit: underruns %u before, %u after an idle unit failed, %u after the sounding unit failed; wet peak %.3g before, %.3g with the idle unit failed, %.3g with the sounding unit failed, %.3g after\n",
		before, idle, sounding, wetBefore, wetIdle, wetSounding, wetAfter);
	Report(ok && idle == before + 1 && sounding == idle + 1 && wetBefore > 0.001f && wetIdle > 0.25f * wetBefore && wetSounding == 0.0f && wetAfter > 0.001f && BlockPeak(L, 10) > 0.1f,
		"a failed pull of a reverb unit leaves the other units' output in and silences only that unit for the block, not the voices");
	delete mirror;
	delete sh;
}

static bool
UnitHolds(int unit, int place, float *got)
{
	float v[6];
	if(!ShellEnvParamsForTest(unit, v))
		return false;
	const EnvPreset &p = kEnvPresets[place];
	const float want[6] = { 100.0f, p.gainDb, p.minDelay, p.maxDelay, p.decay0, p.decayNyquist };
	bool ok = true;
	for(int i = 0; i < 6; i++)
		ok = ok && fabsf(v[i] - want[i]) <= 1e-5f * fmaxf(1.0f, fabsf(want[i]));
	if(got)
		*got = v[4];
	return ok;
}

static bool
EachUnitHoldsItsPlace(void)
{
	bool ok = true;
	for(int u = 0; u < NUM_ENVS; u++)
		ok = UnitHolds(u, u, NULL) && ok;
	return ok;
}

static void
PublishPlace(Shared &sh, Snapshot &mirror, int place, uint32_t blocks, std::vector<float> &L, std::vector<float> &R)
{
	mirror.env.place = (int8_t)place;
	mirror.env.serial++;
	Publish(sh, mirror);
	for(uint32_t b = 0; b < blocks; b++)
		ShellRenderOffline(L.data(), R.data(), 512);
}

static void
CheckEnvPresetsReadBack(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L(512), R(512);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, MODE_SPATIAL);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	bool atSetup = EachUnitHoldsItsPlace();
	ok = ShellRebuildOfflineForTest(48000.0) && ok;
	bool rebuilt = EachUnitHoldsItsPlace();
	ShellStop();
	printf("MEASURED reverb unit parameters read back against the preset table: each unit holds its own place's preset at setup %d, after a rebuild %d\n", atSetup, rebuilt);
	Report(ok && atSetup && rebuilt, "each reverb unit holds all six parameters of its own place's preset after setup and after a rebuild");
	delete mirror;
	delete sh;
}

static void
CheckEnvNeverWrittenAfterSetup(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L(512), R(512);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, MODE_SPATIAL);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	for(int u = 0; u < NUM_ENVS; u++)
		ShellEnvPoisonForTest(u);
	bool poisoned = true;
	for(int u = 0; u < NUM_ENVS; u++)
		poisoned = !UnitHolds(u, u, NULL) && poisoned;
	const int route[] = { ENV_TUNNEL, ENV_GARAGE, ENV_NARROW_STREET, ENV_OPEN };
	for(int place : route){
		PublishPlace(*sh, *mirror, place, 1, L, R);
		PublishPlace(*sh, *mirror, place, 100, L, R);
	}
	bool untouched = true;
	for(int u = 0; u < NUM_ENVS; u++)
		untouched = !UnitHolds(u, u, NULL) && untouched;
	EnvState st = ShellEnvStateForTest();
	ShellStop();
	printf("MEASURED four place changes with every unit's parameters poisoned: still poisoned %d, confirmed place %d, fade running %d\n", untouched, st.current, st.fadeLeft != 0);
	Report(ok && poisoned && untouched && st.current == ENV_OPEN && st.fadeLeft == 0, "a place change writes no parameter of any reverb unit");
	delete mirror;
	delete sh;
}

static bool
WeightsOneHot(const EnvState &st, int place)
{
	for(int u = 0; u < NUM_ENVS; u++)
		if(st.weight[u] != (u == place ? 1.0f : 0.0f))
			return false;
	return st.fadeLeft == 0;
}

static void
CheckEnvSetUpOnConfirmedPlace(void)
{
	std::vector<float> L(512), R(512);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	bool startOk = ShellEnvStateForTest().current == ENV_OPEN && WeightsOneHot(ShellEnvStateForTest(), ENV_OPEN);
	sh->envPlace.store(ENV_TUNNEL);
	PublishPlace(*sh, *mirror, ENV_TUNNEL, 1, L, R);
	PublishPlace(*sh, *mirror, ENV_TUNNEL, 1, L, R);
	ok = ShellRebuildOfflineForTest(48000.0) && ok;
	bool confirmedOk = ShellEnvStateForTest().current == ENV_TUNNEL && WeightsOneHot(ShellEnvStateForTest(), ENV_TUNNEL);
	sh->envPlace.store(ENV_GARAGE);
	PublishPlace(*sh, *mirror, ENV_GARAGE, 1, L, R);
	ok = ShellRebuildOfflineForTest(48000.0) && ok;
	bool flickerOk = ShellEnvStateForTest().current == ENV_TUNNEL && WeightsOneHot(ShellEnvStateForTest(), ENV_TUNNEL);
	bool unitsOk = EachUnitHoldsItsPlace();
	ShellStop();
	ok = ShellStartOffline(sh, 44100.0) && ok;
	bool startedOk = ShellEnvStateForTest().current == ENV_GARAGE && WeightsOneHot(ShellEnvStateForTest(), ENV_GARAGE);
	ShellStop();
	printf("MEASURED the weight after a start and a rebuild: OPEN at start %d, TUNNEL after a rebuild with TUNNEL confirmed %d, TUNNEL after a rebuild with one GARAGE evaluation %d, GARAGE at a start with GARAGE published %d\n",
		startOk, confirmedOk, flickerOk, startedOk);
	Report(ok && startOk && confirmedOk && flickerOk && unitsOk && startedOk,
		"a start puts all the weight on the published place and a rebuild on the place the fade state confirmed, not on a single evaluation");
	delete mirror;
	delete sh;
}

static void
CheckEnvPullLimit(void)
{
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, MODE_SPATIAL);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	UnitTap tap;
	ShellEnvTapForTest(TapUnits, &tap);
	std::vector<float> L(41 * 512, 9.0f), R(41 * 512, 9.0f);
	uint32_t b = 0, start = 0, gaps = 0, before = 0, after = 0;
	for(; ok && b < 41; b++){
		if(b == 4)
			start = sh->underruns.load();
		if(b == 4 || b == 12)
			ShellFailPullsForTest(2 + ENV_NARROW_STREET, 7);
		if(b == 21){
			before = sh->underruns.load();
			gaps = before - start;
			ShellFailPullsForTest(2 + ENV_NARROW_STREET, 1000);
		}
		if(b == 33){
			after = sh->underruns.load();
			ShellFailPullsForTest(2 + ENV_NARROW_STREET, 0);
			ok = ShellRebuildOfflineForTest(48000.0) && ok;
		}
		ShellRenderOffline(&L[b * 512], &R[b * 512], 512);
		ShellService();
	}
	ShellEnvTapForTest(NULL, NULL);
	ShellStop();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	int lines = CountInFile(log, "check: reverb units stopped after repeated failed renders, spatial mode has no reverb until the output is rebuilt\n");
	fclose(log);
	float between = SpanPeak(tap.envL, 19 * 512, 21 * 512), stopped = fmaxf(SpanPeak(tap.envL, 29 * 512, 33 * 512), SpanPeak(tap.envR, 29 * 512, 33 * 512));
	float again = SpanPeak(tap.envL, 37 * 512, 41 * 512);
	printf("MEASURED a reverb unit failing its pulls: two runs of 7 with a good block between, underruns %u, wet peak after them %.3g; then every pull failing, underruns %u over 12 blocks (limit %d), wet peak %.3g, output peak %.3f, log lines %d, wet peak after a rebuild %.3g\n",
		gaps, between, after - before, 8, stopped, BlockPeak(L, 32), lines, again);
	Report(ok && gaps == 14 && between > 0.001f && after - before == 8 && stopped == 0.0f && BlockPeak(L, 32) > 0.1f && lines == 1 && again > 0.001f,
		"a reverb unit that keeps failing its pulls stops all the units after the fail limit, logs once, and the next rebuild brings them back");
	delete mirror;
	delete sh;
}

static void
CheckClassicRoutingFeedsCave(void)
{
	UnitTap classic, failed;
	std::vector<float> L, R;
	uint64_t pulls0 = ShellEnvPullsForTest();
	bool ok = RenderUnits(MODE_CLASSIC, 512, 16384, classic, L, R);
	uint64_t classicPulls = ShellEnvPullsForTest() - pulls0;
	ShellFailSpatialOnceForTest(1);
	uint64_t pulls1 = ShellEnvPullsForTest();
	ok = RenderUnits(MODE_SPATIAL, 512, 16384, failed, L, R) && ok;
	uint64_t noUnitPulls = ShellEnvPullsForTest() - pulls1;
	bool same = classic.caveL == failed.caveL && classic.caveR == failed.caveR;
	printf("MEASURED classic routing with a sending voice: classic mode CAVE peak %.3f unit wet peak %.3g, %llu unit pulls; spatial mode with no spatial unit CAVE peak %.3f unit wet peak %.3g, %llu unit pulls, CAVE identical %d\n",
		SpanPeak(classic.caveL, 0, 16384), SpanPeak(classic.envL, 0, 16384), (unsigned long long)classicPulls, SpanPeak(failed.caveL, 0, 16384), SpanPeak(failed.envL, 0, 16384), (unsigned long long)noUnitPulls, same);
	Report(ok && SpanPeak(classic.caveL, 0, 16384) > 0.001f && same && SpanPeak(classic.envL, 0, 16384) == 0.0f && SpanPeak(classic.envR, 0, 16384) == 0.0f &&
		SpanPeak(failed.envL, 0, 16384) == 0.0f && SpanPeak(failed.envR, 0, 16384) == 0.0f,
		"classic routing feeds the CAVE reverb with classic's send rule and leaves the reverb units silent, in classic mode and in spatial mode without a spatial unit");
	Report(ok && classicPulls == 0 && noUnitPulls == 0, "classic routing that never had a spatial frame pulls no reverb unit, in classic mode and in spatial mode without a spatial unit");
}

static void
CheckClassicStopsPullingUnits(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L(512), R(512);
	Shared *sh = new Shared();
	Snapshot *mirror = SendingMirror(s, MODE_SPATIAL);
	Publish(*sh, *mirror);
	bool ok = ShellStartOffline(sh, 48000.0);
	UnitTap tap;
	ShellEnvTapForTest(TapUnits, &tap);
	sh->envPlace.store(ENV_TUNNEL);
	PublishPlace(*sh, *mirror, ENV_TUNNEL, 1, L, R);
	PublishPlace(*sh, *mirror, ENV_TUNNEL, 120, L, R);
	mirror->mode = MODE_CLASSIC;
	mirror->voice[3].playing = 0;
	Publish(*sh, *mirror);
	size_t switchAt = tap.envL.size();
	const uint32_t blocks = 400;
	uint32_t pulled = 0, lastPulled = 0, firstClassic = 0, pulledAfterStop = 0;
	bool seenClassic = false, stopped = false;
	for(uint32_t b = 0; ok && b < blocks; b++){
		uint64_t p0 = ShellEnvPullsForTest(), c0 = ShellClassicFramesForTest();
		ShellRenderOffline(L.data(), R.data(), 512);
		bool pulledNow = ShellEnvPullsForTest() != p0;
		bool classic = ShellClassicFramesForTest() - c0 == 512;
		if(classic && !seenClassic){
			seenClassic = true;
			firstClassic = b;
		}
		if(classic && pulledNow){
			pulled++;
			lastPulled = b;
		}
		if(classic && !pulledNow)
			stopped = true;
		if(stopped && pulledNow)
			pulledAfterStop++;
	}
	size_t lastBlockAt = switchAt + (size_t)lastPulled * 512;
	float tailAtSwitch = SpanPeak(tap.envL, switchAt, switchAt + 4096);
	float tailAtEnd = fmaxf(SpanPeak(tap.envL, lastBlockAt, lastBlockAt + 512), SpanPeak(tap.envR, lastBlockAt, lastBlockAt + 512));
	uint32_t want = (EnvLongestDecayFrames(48000.0f) + 511) / 512;
	PublishPlace(*sh, *mirror, ENV_GARAGE, 1, L, R);
	PublishPlace(*sh, *mirror, ENV_GARAGE, 1, L, R);
	bool settled = ShellEnvStateForTest().current == ENV_GARAGE && WeightsOneHot(ShellEnvStateForTest(), ENV_GARAGE);
	mirror->mode = MODE_SPATIAL;
	mirror->voice[3].playing = 1;
	Publish(*sh, *mirror);
	bool resumed = false, early = false, firstOneHot = false;
	for(uint32_t b = 0; ok && b < 12; b++){
		uint64_t p0 = ShellEnvPullsForTest(), c0 = ShellClassicFramesForTest();
		ShellRenderOffline(L.data(), R.data(), 512);
		bool hasSpatial = ShellClassicFramesForTest() - c0 < 512;
		bool pulledNow = ShellEnvPullsForTest() != p0;
		if(hasSpatial && !resumed)
			firstOneHot = WeightsOneHot(ShellEnvStateForTest(), ENV_GARAGE);
		if(hasSpatial && !resumed)
			resumed = pulledNow;
		early = early || (!hasSpatial && pulledNow);
	}
	ShellEnvTapForTest(NULL, NULL);
	ShellStop();
	printf("MEASURED a tail across a switch to classic: %u fully classic blocks pulled (wanted %u) after the first at block %u, last pulled at block %u, pulls after the stop %u; tail peak %.3g at the switch, %.3g in the last pulled block (%.1f dB)\n",
		pulled, want, firstClassic, lastPulled, pulledAfterStop, tailAtSwitch, tailAtEnd, 20.0 * log10((tailAtEnd + 1e-30) / tailAtSwitch));
	Report(ok && seenClassic && pulled == want && pulledAfterStop == 0 && tailAtSwitch > 0.001f && tailAtEnd < 3e-4f * tailAtSwitch,
		"the reverb units are pulled for the longest decay after the last spatial frame, so a tail rings out across a switch to classic, and not after that");
	Report(ok && resumed && !early, "the reverb units are pulled again from the first block with a spatial frame, and not in a block without one");
	Report(ok && settled && firstOneHot, "a place confirmed while the reverb units are not pulled puts all the weight on it at once, with no fade left for the next spatial block");
	delete mirror;
	delete sh;
}

static void
CheckDeviceLine(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[4] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker),
		TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0),
		TypedDevice(30, "PG32UCDM", "PG32UCDM", kTransportDisplayPort, 0, 0),
		TypedDevice(40, "Interface", "Interface", kTransportUSB, 0, 0) };
	list[3].outputChannels = 6;
	double rates[4] = { 48000.0, 48000.0, 48000.0, 48000.0 };
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	ShellFakeDevicesForTest(list, rates, 4, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellWaitControlForTest();
	const uint32_t next[3] = { 10, 30, 40 };
	for(uint32_t id : next){
		ShellFakeDevicesForTest(list, rates, 4, id);
		ShellPostEventForTest(EVENT_DEFAULT_CHANGED, id);
		ShellWaitControlForTest();
	}
	StopOnFakeTime();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	int a50 = CountInFile(log, "check: device \"A50 Main\" transport usb, terminals hdph, data source none, channels 2, output headphones\n");
	int mac = CountInFile(log, "check: device \"MacBook Pro Speakers\" transport bltn, terminals 0x301, data source ispk, channels 2, output builtin\n");
	int display = CountInFile(log, "check: device \"PG32UCDM\" transport dprt, terminals 0, data source none, channels 2, output external\n");
	int multi = CountInFile(log, "check: device \"Interface\" transport usb, terminals 0, data source none, channels 6, output headphones\n");
	int all = 0;
	char buf[512];
	rewind(log);
	while(fgets(buf, sizeof(buf), log))
		all += strncmp(buf, "check: device ", 14) == 0;
	fclose(log);
	printf("MEASURED device lines: A50 %d, MacBook %d, display %d, interface %d, all %d\n", a50, mac, display, multi, all);
	Report(started && a50 == 1 && mac == 1 && display == 1 && multi == 1 && all == 4,
		"each opened device logs one line with its transport, terminals, data source, channels and the output type the rule resolved");
	delete mirror;
	delete sh;
}

struct NamesRun
{
	bool started;
	int pinna, named;
	char label[64];
};

static NamesRun
RunWithNames(const ShellConfig *config, bool freshQueue)
{
	NamesRun r = {};
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { Device(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers") };
	double rates[1] = { 48000.0 };
	FILE *log = tmpfile();
	fflush(stdout);
	int saved = dup(fileno(stdout));
	dup2(fileno(log), fileno(stdout));
	ShellFakeDevicesForTest(list, rates, 1, 10);
	if(freshQueue)
		ShellDropControlForTest();
	FakeTimeOn();
	r.started = ShellStart(sh, "default", OUTPUT_AUTO, kSpatialUseOutputType, config) && Elapse(0);
	ShellWaitControlForTest();
	ShellQueueLabelForTest(r.label, sizeof(r.label));
	StopOnFakeTime();
	fflush(stdout);
	dup2(saved, fileno(stdout));
	close(saved);
	char buf[512];
	rewind(log);
	while(fgets(buf, sizeof(buf), log)){
		r.pinna += strncmp(buf, "pinna: ", 7) == 0;
		r.named += strncmp(buf, "check: ", 7) == 0;
	}
	fclose(log);
	delete mirror;
	delete sh;
	return r;
}

static void
CheckShellNames(void)
{
	NamesRun none = RunWithNames(NULL, true);
	NamesRun named = RunWithNames(&kCheckNames, true);
	static const ShellConfig half = { NULL, "other.queue" };
	NamesRun partial = RunWithNames(&half, true);
	NamesRun kept = RunWithNames(&kCheckNames, false);
	printf("MEASURED shell names: no config %d pinna lines %d named lines label %s, names %d and %d label %s, half config %d and %d label %s, names on a queue that exists %d and %d label %s\n",
		none.pinna, none.named, none.label, named.pinna, named.named, named.label, partial.pinna, partial.named, partial.label, kept.pinna, kept.named, kept.label);
	Report(none.started && none.pinna > 0 && none.named == 0 && strcmp(none.label, "pinna.audio.control") == 0 &&
		named.started && named.named > 0 && named.pinna == 0 && strcmp(named.label, "check.audio.control") == 0 &&
		partial.started && partial.pinna > 0 && partial.named == 0 && strcmp(partial.label, "other.queue") == 0,
		"with no start-time names the log lines carry pinna: and the queue is pinna.audio.control, with names they carry the prefix and the queue the label, and a null field takes its default");
	Report(kept.started && kept.named > 0 && kept.pinna == 0 && strcmp(kept.label, "other.queue") == 0,
		"a start with another queue label keeps the control queue that exists, and its log prefix still applies");
}

static double
SecondDiffPeak(const std::vector<float> &x, size_t a, size_t b)
{
	double m = 0.0;
	for(size_t n = a > 0 ? a : 1; n < b && n + 1 < x.size(); n++)
		m = fmax(m, fabs((double)x[n + 1] - 2.0 * x[n] + x[n - 1]));
	return m;
}

struct FadeRun
{
	bool ok;
	double ratio;
	float before, after;
	size_t silent;
	uint32_t rebuilds;
	int32_t type;
	uint32_t quiet, tail;
};

static FadeRun
RunFadeAcrossRebuild(int change)
{
	FadeRun r = {};
	std::vector<int16_t> s = Sine(22050, 440.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 22050.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellSetSpatial(true);
	ShellWaitControlForTest();
	std::atomic<bool> run(true);
	std::atomic<int> pulls(0);
	std::vector<float> line;
	std::thread render([&]{
		float L[480], R[480];
		while(run.load()){
			bool got = ShellFakeUnitPullForTest(L, R, 480);
			line.insert(line.end(), L, L + 480);
			if(got){
				pulls.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}else
				std::this_thread::yield();
		}
	});
	while(pulls.load() < 100)
		Sleep(1);
	uint32_t rebuilds0 = ShellGetInfo().rebuilds;
	if(change == 0)
		ShellSetOutputPref(OUTPUT_BUILTIN);
	else
		ShellSetOutputConfig(kSpatialHRTFHQ, OUTPUT_BUILTIN, 1);
	ShellWaitControlForTest();
	int resumed = pulls.load();
	while(pulls.load() < resumed + 100)
		Sleep(1);
	run.store(false);
	render.join();
	r.rebuilds = ShellGetInfo().rebuilds - rebuilds0;
	r.type = ShellOutputType();
	ShellFadeAtStopForTest(&r.quiet, &r.tail);
	StopOnFakeTime();
	size_t from = 480 * 20, steady = 480 * 90;
	double ref = SecondDiffPeak(line, from, steady);
	r.ratio = ref > 0.0 ? SecondDiffPeak(line, from, line.size()) / ref : INFINITY;
	r.before = Peak(std::vector<float>(line.begin() + from, line.begin() + steady));
	r.after = Peak(std::vector<float>(line.end() - 480 * 40, line.end()));
	size_t run0 = 0;
	for(size_t i = from; i < line.size(); i++){
		run0 = line[i] == 0.0f ? run0 + 1 : 0;
		r.silent = std::max(r.silent, run0);
	}
	r.ok = started && line.size() > steady;
	delete mirror;
	delete sh;
	return r;
}

static void
CheckOutputFadesAcrossRebuild(void)
{
	FadeRun pref = RunFadeAcrossRebuild(0), config = RunFadeAcrossRebuild(1);
	printf("MEASURED a rebuild while the unit renders a 440 Hz tone: output type change ClickRatio %.3f (rebuilds %u, peak %.3f before and %.3f after, longest silence %zu frames); algorithm, output type and head tracking in one call ClickRatio %.3f (rebuilds %u)\n",
		pref.ratio, pref.rebuilds, pref.before, pref.after, pref.silent, config.ratio, config.rebuilds);
	Report(pref.ok && pref.ratio < 2.0 && pref.before > 0.1f && pref.after > 0.1f && pref.silent > 0 && pref.rebuilds == 1 && pref.type == OUTPUT_BUILTIN,
		"an output type change while the unit renders fades the output out before the stop and back in after the restart, with no click");
	Report(config.ok && config.ratio < 2.0 && config.before > 0.1f && config.after > 0.1f && config.silent > 0 && config.rebuilds == 1 && config.type == OUTPUT_BUILTIN,
		"an algorithm, output type and head tracking change in one call rebuilds once and fades the same way");
	printf("MEASURED fade tail: the stop came after %u and %u silent frames against a tail of %u and %u\n", pref.quiet, config.quiet, pref.tail, config.tail);
	Report(pref.tail >= 512 && pref.quiet >= pref.tail && config.tail >= 512 && config.quiet >= config.tail,
		"the unit is stopped only after the output latency's worth of silent frames has been rendered past the fade, so the device has played the ramp");
}

static void
CheckRestoreDefaultsRebuildsOnce(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[2] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker),
		TypedDevice(20, "A50-USB", "A50 USB", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[2] = { 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 2, 10);
	FakeTimeOn();
	bool started = NamedStart(sh, "A50-USB", OUTPUT_HEADPHONES, kSpatialHRTFHQ) && Elapse(0);
	ShellWaitControlForTest();
	ShellInfo before = ShellGetInfo();
	uint32_t base = before.rebuilds;
	ShellSetOutputConfig(kSpatialUseOutputType, OUTPUT_AUTO, 0, "default");
	ShellWaitControlForTest();
	ShellInfo after = ShellGetInfo();
	uint32_t one = after.rebuilds - base;
	ShellSetOutputConfig(kSpatialUseOutputType, OUTPUT_AUTO, 0, "default");
	ShellSetOutputConfig(kSpatialUseOutputType, OUTPUT_AUTO, 0, "");
	ShellWaitControlForTest();
	uint32_t again = ShellGetInfo().rebuilds - base - one;
	StopOnFakeTime();
	printf("MEASURED restore defaults with a pinned device: device \"%s\" to \"%s\", one call made %u rebuilds, the same again %u\n", before.device, after.device, one, again);
	Report(started && strcmp(before.device, "A50 USB") == 0 && strcmp(after.device, "MacBook Pro Speakers") == 0 && one == 1 && again == 0,
		"one config call that unpins the device and changes the output type rebuilds once, and the same call again rebuilds nothing");
	delete mirror;
	delete sh;
}

static void
CheckTypeUnknownAfterEveryDeviceLost(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-USB", "A50 USB", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	ShellWaitControlForTest();
	int32_t open = ShellOutputType();
	list[0].alive = false;
	ShellFakeDevicesForTest(list, rates, 1, 0);
	ShellPostEventForTest(EVENT_DEVICE_GONE, 20);
	int32_t lost = ShellOutputType(), shown = ShellGetInfo().outputType;
	StopOnFakeTime();
	printf("MEASURED output type: %d with the device open, %d and %d once every device is gone\n", open, lost, shown);
	Report(started && open == OUTPUT_HEADPHONES && lost == OUTPUT_AUTO && shown == OUTPUT_AUTO,
		"once every device is gone the output type reads as unknown again, so the speaker entry shows AUTO without a stale type");
	delete mirror;
	delete sh;
}

static void
CheckConfigCoalescesAndPinsOutputType(void)
{
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	Publish(*sh, *mirror);
	DeviceEntry list[2] = { TypedDevice(10, "BuiltInSpeakerDevice", "MacBook Pro Speakers", kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker),
		TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[2] = { 48000.0, 48000.0 };
	ShellFakeDevicesForTest(list, rates, 2, 10);
	ShellSetHeadTracking(false);
	FakeTimeOn();
	bool started = NamedStart(sh, "default", OUTPUT_AUTO, kSpatialHRTFHQ) && Elapse(0);
	ShellSetSpatial(true);
	UnitSeen macStart = SeeUnit();
	ShellSetAlgorithm(kSpatialUseOutputType);
	SeeUnit();
	ShellSetAlgorithm(kSpatialHRTFHQ);
	UnitSeen macLive = SeeUnit();
	ShellFakeDevicesForTest(list, rates, 2, 20);
	ShellPostEventForTest(EVENT_DEFAULT_CHANGED, 20);
	UnitSeen headphones = SeeUnit();
	uint32_t base = ShellGetInfo().rebuilds;
	ShellSetOutputConfig(kSpatialUseOutputType, OUTPUT_BUILTIN, 1);
	UnitSeen builtin = SeeUnit();
	uint32_t one = ShellGetInfo().rebuilds - base;
	ShellSetOutputConfig(kSpatialUseOutputType, OUTPUT_BUILTIN, 1);
	SeeUnit();
	uint32_t again = ShellGetInfo().rebuilds - base - one;
	ShellSetOutputConfig(kKeepConfig, kKeepConfig, kKeepConfig);
	SeeUnit();
	uint32_t keep = ShellGetInfo().rebuilds - base - one - again;
	ShellSetOutputConfig(kSpatialHRTFHQ, OUTPUT_AUTO, 0);
	UnitSeen back = SeeUnit();
	uint32_t both = ShellGetInfo().rebuilds - base - one - again - keep;
	StopOnFakeTime();
	ShellSetHeadTracking(false);
	printf("MEASURED output config: HRTFHQ on the MacBook speakers reads back algorithm %d at start and %d after a live set (type %d, unit type %d, calib %.4f), on headphones %d; one call changing three things made %u rebuilds (type %d unit %d), the same again %u, keep all %u, algorithm and output back %u (algorithm %d)\n",
		macStart.algorithm, macLive.algorithm, macLive.type, macLive.unitType, macLive.calib, headphones.algorithm, one, builtin.type, builtin.unitType, again, keep, both, back.algorithm);
	Report(started && macStart.type == OUTPUT_BUILTIN && macStart.unitType == OUTPUT_BUILTIN && macStart.algorithm == kSpatialUseOutputType &&
		macLive.unitType == OUTPUT_BUILTIN && macLive.algorithm == kSpatialUseOutputType && macLive.calib == kSpatialCalibBuiltInSpeakers && headphones.algorithm == kSpatialHRTFHQ,
		"the HRTFHQ algorithm pref gives headphones the high quality binaural and leaves the MacBook speakers on the output type virtualisation, at start and when set live");
	Report(started && one == 1 && builtin.type == OUTPUT_BUILTIN && builtin.algorithm == kSpatialUseOutputType && again == 0 && keep == 0 && both == 1 &&
		back.type == OUTPUT_HEADPHONES && back.algorithm == kSpatialHRTFHQ,
		"a change to the algorithm, the output type and head tracking in one call rebuilds once, the same values again or none at all rebuild nothing");
	delete mirror;
	delete sh;
}

static void
CheckFeedCallbacksStayInBlock(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	struct Case { uint32_t at, frames, capacity; };
	const Case cases[] = { { 3584, 1024, 1024 }, { 0, 1024, 300 }, { 4000, 512, 512 }, { 0, 512, 512 } };
	bool tailZero = true, canary = true, head = true;
	for(const Case &c : cases){
		const uint32_t guard = 64;
		float *m = new float[c.capacity + guard], *l = new float[c.capacity + guard], *r = new float[c.capacity + guard];
		std::fill(m, m + c.capacity + guard, 5.0f);
		std::fill(l, l + c.capacity + guard, 5.0f);
		std::fill(r, r + c.capacity + guard, 5.0f);
		ShellFeedForTest(c.at, c.frames, c.capacity, m, l, r);
		uint32_t room = 4096 - c.at, filled = std::min(std::min(c.frames, c.capacity), room), written = std::min(c.frames, c.capacity);
		for(float *buf : { m, l, r }){
			for(uint32_t i = 0; i < filled; i++)
				head = head && buf[i] != 5.0f;
			for(uint32_t i = filled; i < written; i++)
				tailZero = tailZero && buf[i] == 0.0f;
			for(uint32_t i = written; i < c.capacity + guard; i++)
				canary = canary && buf[i] == 5.0f;
		}
		delete[] m;
		delete[] l;
		delete[] r;
	}
	StopOnFakeTime();
	printf("MEASURED feed callbacks at chunk offsets near the end of the block and into short buffers: written part overwritten %d, rest of a pull past the block zeroed %d, nothing written beyond the buffer %d\n", head, tailZero, canary);
	Report(started && head && tailZero && canary, "the spatial unit's input callbacks copy no more than the block holds or the buffer takes, and zero what a pull asks beyond it");
	delete mirror;
	delete sh;
}

static void
CheckOutputRenderWithOneBuffer(void)
{
	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	mirror->voice[3] = Voice(s, 1, 31000.0f, 0.8f, 0.2f);
	Publish(*sh, *mirror);
	DeviceEntry list[1] = { TypedDevice(20, "A50-Main", "A50 Main", kTransportUSB, kTerminalHeadphones, 0) };
	double rates[1] = { 48000.0 };
	ShellFakeDevicesForTest(list, rates, 1, 20);
	FakeTimeOn();
	bool started = NamedStart(sh, "default") && Elapse(0);
	std::vector<float> one(512, 9.0f), two(512, 9.0f);
	float *single[1] = { one.data() };
	bool pulled = ShellFakeUnitPullMultiForTest(single, 1, 512);
	bool silent = Peak(one) == 0.0f;
	float *stereo[2] = { one.data(), two.data() };
	std::fill(one.begin(), one.end(), 9.0f);
	bool normal = ShellFakeUnitPullMultiForTest(stereo, 2, 512);
	StopOnFakeTime();
	printf("MEASURED a pull with one output buffer: rendered %d, buffer silent %d; the next pull with two buffers rendered %d\n", pulled, silent, normal);
	Report(started && !pulled && silent && normal, "a pull with fewer than two output buffers returns silence without rendering, and the next two-buffer pull renders");
	delete mirror;
	delete sh;
}

int
main(void)
{
	InitResampler();
	gShellOffline = true;
	SetLogPrefix(kCheckNames.logPrefix);

	Report(ShellOutputType() == OUTPUT_AUTO && ShellGetInfo().outputType == OUTPUT_AUTO, "before any output opens the output type reads as unknown, so the speaker entry shows AUTO alone");
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	bool started = ShellStartOffline(sh, 48000.0);
	ShellInfo info = ShellGetInfo();
	printf("MEASURED offline shell: rate %.0f buffer %u device \"%s\"%s\n", info.rate, info.buffer, info.device, info.spatialBypassed ? ", spatial mixer bypassed" : "");
	Report(started && info.rate == 48000.0 && info.buffer == 512 && strcmp(info.device, "offline") == 0 && !info.spatialBypassed,
		"offline shell starts with the spatial mixer and no device");

	std::vector<float> L(48000, 9.0f), R(48000, 9.0f);
	ShellRenderOffline(&L[0], &R[0], 48000);
	Report(Peak(L) == 0.0f && Peak(R) == 0.0f, "offline shell renders silence with no voices");

	float inL[512], inR[512], outL[512], outR[512];
	double worst = 0.0;
	bool pulled = true;
	for(int b = 0; b < 20 && pulled; b++){
		for(int i = 0; i < 512; i++){
			int n = b * 512 + i;
			inL[i] = 0.3f * sinf(0.013f * n) + (n == 3000 ? 0.5f : 0.0f);
			inR[i] = 0.2f * sinf(0.029f * n + 1.0f);
		}
		pulled = ShellPassThroughTest(inL, inR, outL, outR, 512);
		for(int i = 0; i < 512; i++)
			worst = fmax(worst, fmax(fabs(outL[i] - inL[i]), fabs(outR[i] - inR[i])));
	}
	printf("MEASURED classic bus through the spatial mixer: max abs error %.3g\n", worst);
	Report(pulled && worst <= 1e-5, "classic bus passes the spatial mixer unchanged and on time");

	std::vector<int16_t> tone = Sine(22050, 440.0f, 22050.0f);
	mirror->voice[kMidVoice] = Voice(tone, 1, 22050.0f, 1.0f, 0.0f);
	Publish(*sh, *mirror);
	ShellRenderOffline(&L[0], &R[0], 9600);
	double leftL = Db(L, 0, 9600), leftR = Db(R, 0, 9600);
	mirror->voice[kMidVoice] = Voice(tone, 2, 22050.0f, 0.0f, 1.0f);
	Publish(*sh, *mirror);
	ShellRenderOffline(&L[0], &R[0], 9600);
	double rightL = Db(L, 480, 9600), rightR = Db(R, 480, 9600);
	printf("MEASURED left-only voice: left %.1f dB right %.1f dB; right-only voice: left %.1f dB right %.1f dB\n", leftL, leftR, rightL, rightR);
	Report(leftL > -15.0 && leftL - leftR >= 90.0 && rightR > -15.0 && rightR - rightL >= 90.0,
		"a voice's left gain reaches the left output and its right gain the right");

	ShellStop();
	Report(!ShellPassThroughTest(inL, inR, outL, outR, 512), "teardown disposes the spatial mixer");
	delete mirror;
	delete sh;

	std::vector<int16_t> s = Sine(30000, 523.0f, 22050.0f);
	std::vector<float> L0, R0;
	bool ok = RenderChunked(s, 4096, 32768, L0, R0);
	double drift = 0.0;
	for(uint32_t chunk = 256; ok && chunk < 4096; chunk *= 2){
		std::vector<float> Lc, Rc;
		ok = RenderChunked(s, chunk, 32768, Lc, Rc);
		for(size_t i = 0; i < Lc.size(); i++)
			drift = fmax(drift, fmax(fabs(Lc[i] - L0[i]), fabs(Rc[i] - R0[i])));
	}
	printf("MEASURED block sizes 256 to 4096: max abs difference %.3g, level %.1f dB\n", drift, Db(L0, 0, L0.size()));
	Report(ok && drift <= 1e-6 && Peak(L0) > 0.1f, "block sizes from 256 to 4096 frames render the same output");

	bool cycles = true;
	for(int i = 0; i < 50 && cycles; i++){
		Shared *c = new Shared();
		cycles = ShellStartOffline(c, i & 1 ? 44100.0 : 48000.0) && ShellPassThroughTest(inL, inR, outL, outR, 512);
		ShellStop();
		cycles = cycles && !ShellPassThroughTest(inL, inR, outL, outR, 512);
		delete c;
	}
	Report(cycles, "fifty start and stop cycles at 44100 and 48000 leave no unit behind");

	CheckFailedPull();
	CheckSpatialFailure();
	CheckSpatial3DFailure();
	CheckSpatialPullFailures();
	CheckFailedPullLevel();
	CheckSpatialBlockSizes();
	CheckEnvBlockSizes();
	CheckEnvFailsToBuild();
	CheckEnvUnitPullFails();
	CheckEnvSetUpOnConfirmedPlace();
	CheckEnvPresetsReadBack();
	CheckEnvNeverWrittenAfterSetup();
	CheckEnvPullLimit();
	CheckClassicRoutingFeedsCave();
	CheckClassicStopsPullingUnits();
	CheckOfflineMultichannelPref();
	gShellOffline = false;
	CheckDeviceless();
	CheckLossAndReturn();
	CheckBufferEventWhileRendering();
	CheckSilentUnit();
	CheckRetry();
	CheckRetryGuard();
	CheckPin();
	CheckHrtfReadingArmedOnlyInSpatial();
	CheckOutputTypeFollowsDevice();
	CheckOutputPrefWhilePlaying();
	CheckHeadTrackingOnHeadphonesOnly();
	CheckMultichannelOutput();
	CheckMultichannelLoudness();
	CheckMultichannelLayoutPolicy();
	CheckMultichannelFrontPair();
	CheckDeviceLine();
	CheckShellNames();
	SetLogPrefix(kCheckNames.logPrefix);
	CheckAlgorithmWhilePlaying();
	CheckRebuildKeepsSpatialRouting();
	CheckRebuildClearsFailedPulls();
	CheckOutputFadesAcrossRebuild();
	CheckConfigCoalescesAndPinsOutputType();
	CheckRestoreDefaultsRebuildsOnce();
	CheckTypeUnknownAfterEveryDeviceLost();
	CheckFeedCallbacksStayInBlock();
	CheckOutputRenderWithOneBuffer();
	gShellOffline = true;

	if(failures)
		return 1;
	printf("all shell checks passed\n");
	return 0;
}
