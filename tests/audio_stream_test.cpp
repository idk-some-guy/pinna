// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -Wall -Wextra -pthread -I src tests/audio_stream_test.cpp src/CaStream.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaClock.cpp -o build/tests/audio_stream_test && build/tests/audio_stream_test && clang++ -std=gnu++14 -O1 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_stream_test.cpp src/CaStream.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaClock.cpp -o build/tests/audio_stream_test_tsan && build/tests/audio_stream_test_tsan
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "CaStream.h"
#include "CaMixer.h"
#include "CaDrive.h"
#include "CaClock.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static const uint64_t kNoBreak = ~0ull;

struct StatusSink
{
	std::atomic<uint32_t> underruns;
	StatusSink(void) : underruns(0) {}
};

static float
Sig(uint64_t n)
{
	return (float)(n % 64000) * 1e-5f;
}

class Ramp : public Decoder
{
public:
	Ramp(uint64_t len, uint64_t lead, uint64_t breakAt, uint32_t rate = 32000) : mLen(len), mLead(lead), mBreak(breakAt), mPos(0), mNative(0), mPad(0), mRate(rate) {}
	uint32_t rate(void) { return mRate; }
	uint32_t channels(void) { return 2; }
	uint64_t lengthFrames(void) { return mLen; }
	uint64_t leadFrames(void) { return mLead; }
	bool seekFrame(uint64_t f, bool padded)
	{
		uint64_t delay = padded ? mLead : 0;
		mPos = f < mLen ? f : mLen;
		mPad = mPos < delay ? delay - mPos : 0;
		mNative = mPos < delay ? 0 : mPos - delay;
		return true;
	}
	uint32_t read(float *out, uint32_t frames)
	{
		uint32_t n = 0;
		for(; n < frames && mPos < mLen && mPos < mBreak; n++, mPos++){
			float v = 0.0f;
			if(mPad)
				mPad--;
			else
				v = Sig(mNative++);
			out[n * 2] = v;
			out[n * 2 + 1] = -v;
		}
		return n;
	}
private:
	uint64_t mLen, mLead, mBreak, mPos, mNative, mPad;
	uint32_t mRate;
};

static Decoder *
OpenRamp(const char *path)
{
	if(strcmp(path, "missing") == 0)
		return NULL;
	if(strcmp(path, "short") == 0)
		return new Ramp(64000, 0, kNoBreak);
	if(strcmp(path, "shortlead") == 0)
		return new Ramp(64000, 529, kNoBreak);
	if(strcmp(path, "lead") == 0)
		return new Ramp(3200000, 529, kNoBreak);
	if(strcmp(path, "broken") == 0)
		return new Ramp(3200000, 0, 40000);
	if(strcmp(path, "empty") == 0)
		return new Ramp(64000, 0, 0);
	if(path[0] == 'r')
		return new Ramp(30000000, 0, kNoBreak, (uint32_t)atoi(path + 1));
	return new Ramp(3200000, 0, kNoBreak);
}

struct Rig
{
	ChunkRing ring[kStreams];
	StreamStatus status[kStreams];
	std::atomic<uint32_t> underruns;
	StreamReader reader;
	StreamWorker worker;
	Rig(uint32_t stream0Start = 0, DecoderFactory factory = OpenRamp) : ring{ { stream0Start } }, underruns(0), worker(ring, status, factory, false) {}
	void post(uint8_t op, uint32_t gen, const char *path, uint32_t ms, bool loop, uint8_t stream = 0)
	{
		StreamCommand c;
		memset(&c, 0, sizeof(c));
		c.op = op;
		c.stream = stream;
		c.loop = loop;
		c.gen = gen;
		c.startMs = ms;
		if(path)
			strcpy(c.path, path);
		worker.post(c);
	}
	void command(uint8_t op, uint32_t gen, const char *path, uint32_t ms, bool loop)
	{
		post(op, gen, path, ms, loop);
		worker.step();
	}
	void open(uint32_t gen, const char *path, uint32_t ms, bool loop) { command(CMD_OPEN, gen, path, ms, loop); }
	void seek(uint32_t gen, uint32_t ms, bool loop) { command(CMD_SEEK, gen, NULL, ms, loop); }
	std::vector<float> play(const StreamSnap &p, uint32_t frames, std::vector<float> *right = NULL, float outRate = 32000.0f, uint32_t latency = 0)
	{
		std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
		for(uint32_t done = 0; done < frames;){
			uint32_t n = frames - done < 512 ? frames - done : 512;
			worker.step();
			reader.render(p, ring[0], status[0], underruns, outRate, latency, &l[done], &r[done], n);
			done += n;
		}
		if(right)
			*right = r;
		return l;
	}
	uint32_t pos(uint32_t gen)
	{
		uint32_t frame = ~0u, rate = 0;
		if(!status[0].position(gen, &frame, &rate))
			return ~0u;
		return frame;
	}
};

static StreamSnap
Snap(uint32_t gen, bool playing, bool paused)
{
	StreamSnap s = { gen, (uint8_t)playing, (uint8_t)paused, 1.0f, 0.0f, 0.0f, 1.0f };
	return s;
}

static float
WorstOneShot(const std::vector<float> &l, uint64_t start, uint64_t lead, int from)
{
	float worst = 0.0f;
	for(int i = from; i < (int)l.size(); i++){
		uint64_t f = start + i;
		float want = f < lead ? 0.0f : Sig(f - lead);
		worst = fmaxf(worst, fabsf(l[i] - want));
	}
	return worst;
}

static void
TestSeekAndPosition(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	CHECK(r.status[0].prefilledGen.load() == 1);
	std::vector<float> right;
	std::vector<float> l = r.play(Snap(1, true, false), 4096, &right);
	float worst = 0.0f;
	for(int i = 16; i < 4096; i++){
		worst = fmaxf(worst, fabsf(l[i] - Sig(32000 + i)));
		worst = fmaxf(worst, fabsf(right[i] + Sig(32000 + i)));
	}
	CHECK(worst < 1e-5f);
	uint32_t frame = 0, rate = 0;
	CHECK(r.status[0].position(1, &frame, &rate) && frame == 32000 + 4096 && rate == 32000);
	CHECK(!r.status[0].position(2, &frame, &rate));
}

static void
TestOneShotPadsTheLead(void)
{
	static Rig r;
	r.open(1, "lead", 1000, false);
	std::vector<float> l = r.play(Snap(1, true, false), 4096);
	CHECK(WorstOneShot(l, 32000, 529, 16) < 1e-5f);
	CHECK(r.pos(1) == 32000 + 4096);
	static Rig s;
	s.open(1, "shortlead", 0, false);
	l = s.play(Snap(1, true, false), 1024);
	CHECK(l[100] == 0.0f && l[400] == 0.0f);
	CHECK(WorstOneShot(l, 0, 529, 600) < 1e-5f);
}

static void
TestGenerationsAndPause(void)
{
	static Rig r;
	r.open(1, "long", 0, false);
	r.play(Snap(1, true, false), 2048 + 300);
	r.seek(2, 500, false);
	std::vector<float> l = r.play(Snap(2, true, false), 2048);
	CHECK(fabsf(l[100] - Sig(16000 + 100)) < 1e-5f);
	CHECK(WorstOneShot(l, 16000, 0, 80) < 1e-5f);
	CHECK(r.pos(2) == 16000 + 2048);
	uint32_t before = r.pos(2);
	l = r.play(Snap(2, true, true), 2048);
	CHECK(r.pos(2) == before);
	CHECK(l[100] == 0.0f);
	l = r.play(Snap(2, true, false), 512);
	CHECK(fabsf(l[100] - Sig(before + 100)) < 1e-5f);
	CHECK(r.pos(2) == before + 512);
	r.open(3, "short", 0, false);
	l = r.play(Snap(3, false, false), 512);
	CHECK(l[100] == 0.0f);
}

static void
TestReaderWaitsForTheSnapshot(void)
{
	static Rig r;
	r.open(1, "long", 0, false);
	r.play(Snap(1, true, false), 2048);
	r.seek(2, 500, false);
	r.status[0].postedGen.store(1);
	uint32_t before = r.underruns.load();
	std::vector<float> l = r.play(Snap(1, true, false), 64000);
	CHECK(l[63000] == 0.0f && r.underruns.load() > before);
	CHECK(r.pos(1) < 56000);
	l = r.play(Snap(2, true, false), 1024);
	CHECK(WorstOneShot(l, 16000, 0, 80) < 1e-5f);
}

static void
TestStopShape(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	r.play(Snap(1, true, false), 1000);
	std::vector<float> right;
	std::vector<float> l = r.play(Snap(1, false, false), 512, &right);
	CHECK(fabsf(l[0] - Sig(33000)) < 1e-4f);
	CHECK(fabsf(right[0] + Sig(33000)) < 1e-4f);
	CHECK(fabsf(l[32]) < 0.6f * Sig(33000) && fabsf(l[32]) > 0.4f * Sig(33000));
	bool silent = true;
	for(int i = kOalFadeFrames; i < 512; i++)
		silent = silent && l[i] == 0.0f && right[i] == 0.0f;
	CHECK(silent);
	for(int i = 1; i < kOalFadeFrames; i++)
		CHECK(fabsf(l[i]) <= fabsf(l[i - 1]) + 1e-6f);
}

static void
TestPauseFadesTheSignal(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	r.play(Snap(1, true, false), 1000);
	std::vector<float> right;
	std::vector<float> l = r.play(Snap(1, true, true), 512, &right);
	float worst = 0.0f;
	for(int k = 0; k < kOalFadeFrames; k++){
		float g = (float)(kOalFadeFrames - k) / kOalFadeFrames;
		worst = fmaxf(worst, fabsf(l[k] - g * Sig(33000 + k)));
		worst = fmaxf(worst, fabsf(right[k] + g * Sig(33000 + k)));
	}
	CHECK(worst < 1e-5f);
	bool silent = true;
	for(int i = kOalFadeFrames; i < 512; i++)
		silent = silent && l[i] == 0.0f && right[i] == 0.0f;
	CHECK(silent);
	CHECK(r.pos(1) == 33000);
	l = r.play(Snap(1, true, false), 512);
	CHECK(fabsf(l[100] - Sig(33100)) < 1e-5f);
}

static void
TestGainsRampAndCross(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	r.play(Snap(1, true, false), 1024);
	StreamSnap half = Snap(1, true, false);
	half.ll = 0.5f;
	std::vector<float> right;
	std::vector<float> l = r.play(half, 512, &right);
	float worst = 0.0f;
	for(int k = 0; k < 512; k++){
		float g = k < kOalFadeFrames ? 1.0f - 0.5f * k / kOalFadeFrames : 0.5f;
		worst = fmaxf(worst, fabsf(l[k] - g * Sig(33024 + k)));
		worst = fmaxf(worst, fabsf(right[k] + Sig(33024 + k)));
	}
	CHECK(worst < 1e-5f);
	static Rig c;
	c.open(1, "long", 1000, false);
	StreamSnap cross = { 1, 1, 0, 0.25f, 0.5f, 0.75f, 1.0f };
	l = c.play(cross, 512, &right);
	CHECK(fabsf(l[100] + 0.5f * Sig(32100)) < 1e-5f);
	CHECK(fabsf(right[100] + 0.5f * Sig(32100)) < 1e-5f);
}

static void
TestEndAndLoop(void)
{
	static Rig a;
	a.open(1, "short", 1500, false);
	a.play(Snap(1, true, false), 32768);
	CHECK(a.status[0].drainedGen.load() == 1);
	CHECK(a.pos(1) == 64000);
	static Rig b;
	b.open(1, "shortlead", 1500, true);
	std::vector<float> l = b.play(Snap(1, true, false), 32768);
	CHECK(b.status[0].drainedGen.load() == 0);
	uint64_t native = 48000 - 529;
	float worst = 0.0f;
	for(int i = 16; i < 32768; i++){
		uint64_t src = (native + i) % 64000;
		if(src < 16 || src > 64000 - 16)
			continue;
		worst = fmaxf(worst, fabsf(l[i] - Sig(src)));
	}
	CHECK(worst < 1e-4f);
	CHECK(b.pos(1) == (48000 + 32768) % 64000);
	l = b.play(Snap(1, true, false), 65536);
	CHECK(b.status[0].drainedGen.load() == 0);
	CHECK(b.pos(1) == (48000 + 32768 + 65536) % 64000);
	CHECK(fabsf(l[60000] - Sig((native + 32768 + 60000) % 64000)) < 1e-4f);
}

static void
TestLoopFlagChangesOnAnOpenStream(void)
{
	static Rig r;
	r.open(1, "shortlead", 0, false);
	CHECK(r.status[0].prefilledGen.load() == 1);
	r.seek(2, 0, true);
	std::vector<float> l = r.play(Snap(2, true, false), 4096);
	float worst = 0.0f;
	for(int i = 16; i < 4096; i++){
		if(i > 529 - 16 && i < 529 + 16)
			continue;
		worst = fmaxf(worst, fabsf(l[i] - Sig((64000 - 529 + i) % 64000)));
	}
	CHECK(worst < 1e-4f);
	CHECK(r.pos(2) == 4096);
	r.seek(3, 128, false);
	l = r.play(Snap(3, true, false), 2048);
	CHECK(WorstOneShot(l, 4096, 529, 80) < 1e-5f);
	CHECK(r.pos(3) == 4096 + 2048);
}

static void
TestShortReadEnds(void)
{
	static Rig a;
	a.open(1, "broken", 0, false);
	a.play(Snap(1, true, false), 41000);
	CHECK(a.status[0].drainedGen.load() == 1);
	CHECK(a.pos(1) == 40000);
	static Rig b;
	b.open(1, "broken", 0, true);
	std::vector<float> l = b.play(Snap(1, true, false), 41000);
	CHECK(b.status[0].drainedGen.load() == 0);
	CHECK(fabsf(l[40500] - Sig(500)) < 1e-5f);
	CHECK(b.pos(1) == 1000);
	static Rig c;
	c.open(1, "empty", 0, true);
	c.play(Snap(1, true, false), 512);
	CHECK(c.status[0].drainedGen.load() == 1);
}

static void
TestUnderrunAndFailure(void)
{
	static Rig r;
	r.open(1, "long", 0, false);
	std::vector<float> l(512), rr(512);
	for(int i = 0; i < 200; i++)
		r.reader.render(Snap(1, true, false), r.ring[0], r.status[0], r.underruns, 32000.0f, 0, l.data(), rr.data(), 512);
	CHECK(r.underruns.load() > 0);
	r.open(2, "missing", 0, false);
	CHECK(r.status[0].failedGen.load() == 2);
	CHECK(r.worker.waitPrefilled(0, 2, 10));
	r.play(Snap(2, true, false), 512);
	CHECK(r.status[0].drainedGen.load() == 2);
	r.seek(3, 0, false);
	CHECK(r.status[0].failedGen.load() == 3);
}

static double
OalStreamStep(float outRate)
{
	return (double)(uint32_t)(32000.0f / outRate * 65536.0f) / 65536.0;
}

static void
TestRateChange(void)
{
	static Rig r;
	r.open(1, "long", 0, false);
	double s1 = OalStreamStep(48000.0f), s2 = OalStreamStep(44100.0f);
	std::vector<float> a = r.play(Snap(1, true, false), 3 * 65536, NULL, 48000.0f);
	CHECK(r.pos(1) == (uint32_t)(3 * 65536 * s1));
	std::vector<float> b = r.play(Snap(1, true, false), 4096, NULL, 44100.0f);
	CHECK(r.pos(1) == (uint32_t)(3 * 65536 * s1 + 4096 * s2));
	float worst = 0.0f;
	for(size_t i = a.size() - 1000; i + 1 < a.size(); i++)
		worst = fmaxf(worst, fabsf((a[i + 1] - a[i]) - (float)(s1 * 1e-5)));
	worst = fmaxf(worst, fabsf((b[0] - a.back()) - (float)(s1 * 1e-5)));
	for(size_t i = 0; i + 1 < 1000; i++)
		worst = fmaxf(worst, fabsf((b[i + 1] - b[i]) - (float)(s2 * 1e-5)));
	CHECK(worst < 1e-7f);
}

static void
TestAudiblePosition(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	r.play(Snap(1, true, false), 512, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000);
	r.play(Snap(1, true, false), 4096 - 512, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000 + 4096 - 1024);
	r.play(Snap(1, true, true), 512, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000 + 4096 - 512);
	r.play(Snap(1, true, true), 1024, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000 + 4096);
	r.play(Snap(1, true, false), 512, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000 + 4096);
	r.play(Snap(1, true, false), 1024, NULL, 32000.0f, 1024);
	CHECK(r.pos(1) == 32000 + 4096 + 512);
	static Rig b;
	b.open(1, "short", 0, true);
	b.play(Snap(1, true, false), 64000 + 256, NULL, 32000.0f, 1024);
	CHECK(b.pos(1) == 64000 + 256 - 1024);
	b.play(Snap(1, true, false), 1024, NULL, 32000.0f, 1024);
	CHECK(b.pos(1) == 256);
}

static void
TestGenerationWrap(void)
{
	CHECK(NextGen(0xffffffffu) == 1 && NextGen(5) == 6);
	CHECK(GenBefore(0xffffffffu, 1) && !GenBefore(1, 0xffffffffu));
	static Rig r;
	r.open(0xfffffffeu, "long", 0, false);
	r.play(Snap(0xfffffffeu, true, false), 512);
	r.open(0xffffffffu, "long", 0, false);
	r.open(NextGen(0xffffffffu), "short", 1000, false);
	CHECK(r.ring[0].filled() == kRingChunks);
	std::vector<float> l = r.play(Snap(1, true, false), 1024);
	CHECK(l[200] == 0.0f);
	CHECK(fabsf(l[512 + 200] - Sig(32000 + 200)) < 1e-5f);
	CHECK(r.pos(1) == 32000 + 512);
}

static int gOpens, gSeeks;

class CountRamp : public Ramp
{
public:
	CountRamp(void) : Ramp(3200000, 0, kNoBreak) { gOpens++; }
	bool seekFrame(uint64_t f, bool padded)
	{
		gSeeks++;
		return Ramp::seekFrame(f, padded);
	}
};

static Decoder *
OpenCount(const char *path)
{
	return strcmp(path, "missing") == 0 ? NULL : new CountRamp();
}

static void
TestSupersededCommandsDropped(void)
{
	static Rig r(0, OpenCount);
	gOpens = gSeeks = 0;
	r.post(CMD_OPEN, 1, "a", 1000, false);
	r.post(CMD_SEEK, 2, NULL, 2000, false);
	r.post(CMD_OPEN, 3, "b", 500, false);
	r.worker.step();
	CHECK(gOpens == 1 && gSeeks == 1);
	CHECK(r.status[0].prefilledGen.load() == 3);
	std::vector<float> l = r.play(Snap(3, true, false), 2048);
	CHECK(WorstOneShot(l, 16000, 0, 80) < 1e-5f);

	gOpens = gSeeks = 0;
	r.post(CMD_OPEN, 4, "a", 0, false);
	r.post(CMD_SEEK, 5, NULL, 1000, false);
	r.worker.step();
	CHECK(gOpens == 1 && gSeeks == 1);
	l = r.play(Snap(5, true, false), 2048);
	CHECK(r.status[0].prefilledGen.load() == 5);
	CHECK(WorstOneShot(l, 32000, 0, 80) < 1e-5f);

	gOpens = gSeeks = 0;
	r.post(CMD_OPEN, 6, "a", 0, false);
	r.post(CMD_CLOSE, 7, NULL, 0, false);
	r.worker.step();
	CHECK(gOpens == 0 && gSeeks == 0);
	r.post(CMD_CLOSE, 8, NULL, 0, false);
	r.post(CMD_OPEN, 9, "a", 250, false);
	if(kStreams > 1)
		r.post(CMD_OPEN, 1, "a", 0, false, 1);
	r.worker.step();
	CHECK(gOpens == (kStreams > 1 ? 2 : 1) && gSeeks == (kStreams > 1 ? 2 : 1));
	l = r.play(Snap(9, true, false), 2048);
	CHECK(WorstOneShot(l, 8000, 0, 80) < 1e-5f);
	if(kStreams > 1)
		CHECK(r.status[1].prefilledGen.load() == 1);
}

static void
TestThreadedPrefill(void)
{
	static ChunkRing ring[kStreams];
	static StreamStatus status[kStreams];
	StreamWorker w(ring, status, OpenRamp, true);
	StreamCommand c;
	memset(&c, 0, sizeof(c));
	c.op = CMD_OPEN;
	c.gen = 7;
	strcpy(c.path, "long");
	w.post(c);
	CHECK(w.waitPrefilled(0, 7, 2000));
	CHECK(ring[0].filled() * kChunkFrames >= 32000);
}

struct Gate
{
	std::mutex lock;
	std::condition_variable cv;
	bool entered, open;
	std::atomic<int> reads, deleted;
	Gate(void) : entered(false), open(false), reads(0), deleted(0) {}
};

static Gate *gGate;

class BlockingDecoder : public Ramp
{
public:
	BlockingDecoder(void) : Ramp(3200000, 0, kNoBreak) {}
	~BlockingDecoder(void) { gGate->deleted++; }
	uint32_t read(float *out, uint32_t frames)
	{
		if(gGate->reads++ == 0){
			std::unique_lock<std::mutex> l(gGate->lock);
			gGate->entered = true;
			gGate->cv.notify_all();
			gGate->cv.wait(l, [] { return gGate->open; });
		}
		return Ramp::read(out, frames);
	}
};

static Decoder *
OpenBlocking(const char *)
{
	return new BlockingDecoder();
}

static void
TestTerminateInMidDecode(void)
{
	static ChunkRing ring[kStreams];
	static StreamStatus status[kStreams];
	Gate gate;
	gGate = &gate;
	StreamWorker *w = new StreamWorker(ring, status, OpenBlocking, true);
	StreamCommand c;
	memset(&c, 0, sizeof(c));
	c.op = CMD_OPEN;
	c.gen = 4;
	strcpy(c.path, "blocking");
	w->post(c);
	bool entered;
	{
		std::unique_lock<std::mutex> l(gate.lock);
		entered = gate.cv.wait_for(l, std::chrono::seconds(5), [&] { return gate.entered; });
	}
	CHECK(entered);
	int readsAtDelete = gate.reads.load();
	std::atomic<bool> gone(false);
	std::thread terminator([&] {
		delete w;
		gone.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	bool waited = !gone.load(std::memory_order_acquire) && gate.deleted.load() == 0;
	{
		std::lock_guard<std::mutex> l(gate.lock);
		gate.open = true;
	}
	gate.cv.notify_all();
	terminator.join();
	printf("MEASURED terminate in mid-decode: decoder reads %d, decoders deleted %d, ring chunks %u\n", gate.reads.load(), gate.deleted.load(), ring[0].filled());
	CHECK(waited);
	CHECK(gone.load() && gate.deleted.load() == 1);
	CHECK(gate.reads.load() - readsAtDelete <= 1);
	bool ours = true;
	for(uint32_t i = ring[0].tail(); i != ring[0].head(); i++)
		ours = ours && ring[0].at(i)->gen == 4;
	CHECK(ring[0].filled() >= 1 && ours);
	gGate = NULL;
}

static void
TestStarvedWorkerRetriesSoon(void)
{
	static ChunkRing ring[kStreams];
	static StreamStatus status[kStreams];
	StreamWorker w(ring, status, OpenRamp, true);
	StreamCommand c;
	memset(&c, 0, sizeof(c));
	c.op = CMD_OPEN;
	strcpy(c.path, "long");
	c.gen = 1;
	w.post(c);
	CHECK(w.waitPrefilled(0, 1, 2000));
	c.gen = 2;
	w.post(c);
	for(int i = 0; i < 2000 && ring[0].filled() < (uint32_t)kRingChunks; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(ring[0].filled() == kRingChunks);
	c.gen = 3;
	w.post(c);
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	std::atomic<bool> go(true);
	std::thread renderer([&] {
		static StreamReader reader;
		static StatusSink sink;
		float l[64], r[64];
		while(go.load(std::memory_order_acquire)){
			reader.render(Snap(3, false, false), ring[0], status[0], sink.underruns, 32000.0f, 0, l, r, 64);
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	auto t0 = std::chrono::steady_clock::now();
	bool ok = w.waitPrefilled(0, 3, 2000);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	go.store(false, std::memory_order_release);
	renderer.join();
	CHECK(ok);
	CHECK(ms < kWorkerWakeMs * 0.6);
	printf("starved worker: prefilled %.1f ms after the renderer started dropping stale chunks\n", ms);
}

static void
TestPostWithoutPublish(void)
{
	static Rig r;
	r.open(1, "long", 0, false);
	r.play(Snap(1, true, false), 4096);
	r.play(Snap(1, true, true), 512);
	r.open(2, "long", 1000, false);
	std::vector<float> l = r.play(Snap(1, true, true), 2048);
	CHECK(r.status[0].prefilledGen.load() == 2);
	std::vector<float> right;
	l = r.play(Snap(1, true, false), 2048, &right);
	bool silent = true;
	for(int i = 0; i < 2048; i++)
		silent = silent && l[i] == 0.0f && right[i] == 0.0f;
	CHECK(silent);
	CHECK(r.pos(1) == 4096);
	l = r.play(Snap(2, true, false), 2048);
	CHECK(WorstOneShot(l, 32000, 0, 16) < 1e-5f);
	CHECK(r.pos(2) == 32000 + 2048);
	static Rig s;
	s.open(1, "long", 0, false);
	s.play(Snap(1, true, false), 4096);
	l = s.play(Snap(1, false, false), 512, &right);
	CHECK(l[kOalFadeFrames] == 0.0f && l[0] != 0.0f);
	s.open(2, "long", 1000, false);
	l = s.play(Snap(1, false, false), 2048);
	CHECK(s.status[0].prefilledGen.load() == 2);
	l = s.play(Snap(2, true, false), 2048);
	CHECK(WorstOneShot(l, 32000, 0, 16) < 1e-5f);
	CHECK(s.pos(2) == 32000 + 2048);
}

static void
TestPostWithoutPublishThreaded(void)
{
	static ChunkRing ring[kStreams];
	static StreamStatus status[kStreams];
	StreamWorker w(ring, status, OpenRamp, true);
	StreamCommand c;
	memset(&c, 0, sizeof(c));
	c.op = CMD_OPEN;
	c.gen = 1;
	strcpy(c.path, "long");
	w.post(c);
	CHECK(w.waitPrefilled(0, 1, 2000));
	for(int i = 0; i < 2000 && ring[0].filled() < (uint32_t)(32000 * 3 / 2 / kChunkFrames); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	std::atomic<bool> go(true);
	std::thread renderer([&] {
		static StreamReader reader;
		static StatusSink sink;
		float l[512], r[512];
		while(go.load(std::memory_order_acquire)){
			reader.render(Snap(1, true, true), ring[0], status[0], sink.underruns, 32000.0f, 0, l, r, 512);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	c.gen = 2;
	c.startMs = 1000;
	auto t0 = std::chrono::steady_clock::now();
	w.post(c);
	bool ok = w.waitPrefilled(0, 2, 2000);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	go.store(false, std::memory_order_release);
	renderer.join();
	CHECK(ok);
	CHECK(ms < 100.0);
	printf("post without publish: prefilled %.1f ms after the post\n", ms);
}

static void
TestWorkerSleepsAtHighRates(void)
{
	static ChunkRing ring[kStreams];
	static StreamStatus status[kStreams];
	StreamWorker w(ring, status, OpenRamp, true);
	const char *paths[2] = { "r44100", "r48000" };
	const int used = kStreams < 2 ? kStreams : 2;
	for(int s = 0; s < used; s++){
		StreamCommand c;
		memset(&c, 0, sizeof(c));
		c.op = CMD_OPEN;
		c.stream = (uint8_t)s;
		c.gen = 1;
		strcpy(c.path, paths[s]);
		w.post(c);
		CHECK(w.waitPrefilled((uint8_t)s, 1, 2000));
	}
	const uint32_t bound = 1000 / kWorkerWakeMs + 10;
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	uint32_t w0 = w.wakes();
	std::this_thread::sleep_for(std::chrono::seconds(1));
	uint32_t paused = w.wakes() - w0;
	std::atomic<bool> go(true);
	static StatusSink sink;
	std::thread renderer([&] {
		static StreamReader reader[2];
		static float l[1024], r[1024];
		auto next = std::chrono::steady_clock::now();
		while(go.load(std::memory_order_acquire)){
			for(int s = 0; s < used; s++){
				memset(l, 0, sizeof(l));
				memset(r, 0, sizeof(r));
				reader[s].render(Snap(1, true, false), ring[s], status[s], sink.underruns, 48000.0f, 0, l, r, 1024);
			}
			next += std::chrono::microseconds(1024 * 1000000 / 48000);
			std::this_thread::sleep_until(next);
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	uint32_t w1 = w.wakes();
	std::this_thread::sleep_for(std::chrono::seconds(1));
	uint32_t playing = w.wakes() - w1;
	go.store(false, std::memory_order_release);
	renderer.join();
	CHECK(paused <= bound);
	CHECK(playing <= bound);
	CHECK(sink.underruns.load() == 0);
	printf("worker at 44100 and 48000 Hz: %u wake-ups in a second paused, %u playing (bound %u)\n", paused, playing, bound);
}

static void
TestResumeRampsIn(void)
{
	static Rig r;
	r.open(1, "long", 1000, false);
	r.play(Snap(1, true, false), 1024);
	r.play(Snap(1, true, true), 512);
	std::vector<float> l = r.play(Snap(1, true, false), 512);
	float worst = 0.0f;
	for(int k = 0; k < 512; k++){
		float g = k < kOalFadeFrames ? (float)k / kOalFadeFrames : 1.0f;
		worst = fmaxf(worst, fabsf(l[k] - g * Sig(33024 + k)));
	}
	CHECK(l[0] == 0.0f);
	CHECK(worst < 1e-5f);
	static Rig u;
	u.open(1, "long", 0, false);
	std::vector<float> a(512), b(512);
	uint32_t before = u.underruns.load();
	for(int i = 0; i < 200; i++)
		u.reader.render(Snap(1, true, false), u.ring[0], u.status[0], u.underruns, 32000.0f, 0, a.data(), b.data(), 512);
	CHECK(u.underruns.load() > before);
	uint32_t p = u.pos(1);
	l = u.play(Snap(1, true, false), 512);
	worst = 0.0f;
	for(int k = 0; k < 512; k++){
		float g = k < kOalFadeFrames ? (float)k / kOalFadeFrames : 1.0f;
		worst = fmaxf(worst, fabsf(l[k] - g * Sig(p + k)));
	}
	CHECK(l[0] == 0.0f);
	CHECK(worst < 1e-5f);
}

static void
TestRingIndexWrap(void)
{
	static ChunkRing ring(0xfffffff0u);
	for(int i = 0; i < 8; i++){
		CHECK(ring.beginWrite() != 0);
		ring.endWrite();
	}
	uint32_t mark = ring.mark();
	for(int i = 0; i < 16; i++){
		CHECK(ring.beginWrite() != 0);
		ring.endWrite();
	}
	CHECK(ring.mark() == 8u);
	CHECK(ring.filled() == 24 && ring.filledSince(mark) == 16);
	for(int i = 0; i < 4; i++)
		ring.pop();
	CHECK(ring.filledSince(mark) == 16);
	for(int i = 0; i < 14; i++)
		ring.pop();
	CHECK(ring.filled() == 6 && ring.filledSince(mark) == 6);
	for(int i = 0; i < 58; i++){
		CHECK(ring.beginWrite() != 0);
		ring.endWrite();
	}
	CHECK(ring.filled() == (uint32_t)kRingChunks && ring.beginWrite() == 0);
	static Rig r(0xffffffffu - 20);
	r.open(1, "long", 0, false);
	std::vector<float> l = r.play(Snap(1, true, false), 48000);
	CHECK(WorstOneShot(l, 0, 0, 16) < 1e-5f);
	CHECK(r.pos(1) == 48000);
	CHECK(r.underruns.load() == 0);
	CHECK(r.ring[0].mark() < 0x1000);
}

class Tagged : public Decoder
{
public:
	Tagged(uint64_t len, uint32_t tag) : mLen(len), mPos(0), mTag(tag), mCalls(0) {}
	uint32_t rate(void) { return 32000; }
	uint32_t channels(void) { return 2; }
	uint64_t lengthFrames(void) { return mLen; }
	uint64_t leadFrames(void) { return 0; }
	bool seekFrame(uint64_t f, bool)
	{
		mPos = f < mLen ? f : mLen;
		if(f)
			mTag = (uint32_t)((f + 16) / 32 / 64);
		return true;
	}
	uint32_t read(float *out, uint32_t frames)
	{
		if(++mCalls % 5 == 0)
			std::this_thread::sleep_for(std::chrono::microseconds(300));
		uint32_t n = 0;
		for(; n < frames && mPos < mLen; n++, mPos++){
			out[n * 2] = 0.01f;
			out[n * 2 + 1] = 0.01f * mTag;
		}
		return n;
	}
private:
	uint64_t mLen, mPos;
	uint32_t mTag;
	uint64_t mCalls;
};

static Decoder *
OpenTagged(const char *path)
{
	if(path[0] == 't')
		return new Tagged(8000, (uint32_t)atoi(path + 1));
	return new Tagged(3200000, 0);
}

struct Live
{
	Shared *sh;
	Mixer *mx;
	float dryL[kMaxBlock], dryR[kMaxBlock], send[kMaxBlock], strL[kMaxBlock], strR[kMaxBlock];
	uint32_t lastSerial;
	bool havePrev;
	uint32_t checked, superseded;
	std::atomic<uint32_t> violations, clockBlocks, unitBlocks;
};

static void
LiveRender(Live *v, uint32_t frames, double rate)
{
	uint32_t posted = v->sh->status[0].postedGen.load(std::memory_order_acquire);
	v->mx->render(v->dryL, v->dryR, v->send, v->strL, v->strR, frames, (float)rate);
	uint32_t serial = v->sh->latchedSerial.load(std::memory_order_acquire);
	float tag = (float)((serial & 0xff) + 1);
	uint32_t from = v->havePrev && serial == v->lastSerial ? 0 : kOalFadeFrames;
	if(posted == v->sh->status[0].postedGen.load(std::memory_order_acquire) && (posted & 0xff) != (serial & 0xff)){
		v->superseded++;
		for(uint32_t n = kOalFadeFrames; n < frames; n++)
			if(v->strL[n] != 0.0f || v->strR[n] != 0.0f)
				v->violations.fetch_add(1, std::memory_order_relaxed);
	}
	for(uint32_t n = from; n < frames; n++)
		if(fabsf(v->strL[n]) > 1e-6f){
			v->checked++;
			if(fabsf(v->strR[n] / v->strL[n] - tag) > 0.25f)
				v->violations.fetch_add(1, std::memory_order_relaxed);
		}
	v->lastSerial = serial;
	v->havePrev = true;
}

static void
LiveClock(void *ctx, uint32_t frames, double rate)
{
	Live *v = (Live *)ctx;
	LiveRender(v, frames, rate);
	v->clockBlocks.fetch_add(1, std::memory_order_relaxed);
}

struct LiveGame
{
	Shared *sh;
	StreamWorker *worker;
	Snapshot *mirror;
	uint32_t gen, changes, unpublished;
	void publish(void)
	{
		mirror->serial = ++changes << 8 | (gen & 0xff);
		sh->snapshot.writeBuffer() = *mirror;
		sh->snapshot.publish();
	}
	void post(uint8_t op, const char *path, uint32_t ms, bool loop)
	{
		gen = NextGen(gen);
		StreamCommand c;
		memset(&c, 0, sizeof(c));
		c.op = op;
		c.loop = loop;
		c.gen = gen;
		c.startMs = ((gen & 0xff) + 1) * 64 + ms % 64;
		if(path && path[0] == 't'){
			c.startMs = 0;
			snprintf(c.path, sizeof(c.path), "t%u", (gen & 0xff) + 1);
		}else if(path)
			strcpy(c.path, path);
		worker->post(c);
		mirror->stream[0].gen = gen;
	}
	void run(double seconds, uint32_t *seed)
	{
		auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
		while(std::chrono::steady_clock::now() < end){
			*seed = *seed * 1664525u + 1013904223u;
			uint32_t pick = (*seed >> 8) % 10;
			bool later = pick <= 8 && ((*seed >> 24) & 3) == 0;
			StreamSnap &s = mirror->stream[0];
			if(pick < 3){
				char path[8];
				snprintf(path, sizeof(path), "c%u", 1 + (*seed >> 16) % 9);
				post(CMD_OPEN, path, (*seed >> 4) % 20000, (pick & 1) != 0);
				s.playing = 1;
				s.paused = 0;
			}else if(pick < 6){
				post(CMD_SEEK, NULL, (*seed >> 4) % 20000, (pick & 1) != 0);
				s.playing = 1;
			}else if(pick < 8)
				s.paused = !s.paused;
			else if(pick == 8){
				post(CMD_CLOSE, NULL, 0, false);
				s.playing = 0;
			}else
				s.ll = s.ll == 1.0f ? 0.5f : 1.0f, s.rr = s.ll;
			if(later && (pick < 6 || pick == 8))
				unpublished++;
			else
				publish();
			std::this_thread::sleep_for(std::chrono::microseconds(500 + (*seed >> 20) % 2500));
		}
	}
};

static void
TestRenderersAndWorkerRace(void)
{
	Live *v = new Live();
	v->sh = new Shared();
	v->mx = new Mixer(v->sh);
	v->lastSerial = 0;
	v->havePrev = false;
	v->checked = 0;
	v->superseded = 0;
	v->violations.store(0);
	v->clockBlocks.store(0);
	v->unitBlocks.store(0);
	StreamWorker *worker = new StreamWorker(v->sh->ring, v->sh->status, OpenTagged, true);
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	mirror->stream[0].ll = mirror->stream[0].rr = 1.0f;
	LiveGame g = { v->sh, worker, mirror, 0xfffffff0u, 0, 0 };
	const double rate = 256000.0;
	Drive drive;
	Clock clock;
	drive.ToClock();
	clock.Start(&drive, LiveClock, v, rate, 512);
	uint32_t seed = 12345;
	g.run(0.5, &seed);
	std::atomic<bool> unitRun(true);
	std::thread unit([&] {
		while(unitRun.load(std::memory_order_acquire)){
			if(drive.EnterUnit() == Drive::RENDER){
				LiveRender(v, 512, rate);
				v->unitBlocks.fetch_add(1, std::memory_order_relaxed);
				drive.LeaveUnit(512);
			}
			std::this_thread::sleep_for(std::chrono::microseconds(2000));
		}
	});
	drive.Arm();
	g.run(0.5, &seed);
	drive.ToClock();
	unitRun.store(false, std::memory_order_release);
	unit.join();
	g.post(CMD_OPEN, "t", 0, false);
	mirror->stream[0].playing = 1;
	mirror->stream[0].paused = 0;
	g.publish();
	uint32_t gen = g.gen;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while(v->sh->status[0].drainedGen.load(std::memory_order_acquire) != gen && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	drive.Halt();
	clock.Stop();
	drive.WaitIdle();
	uint32_t frame = 0, r = 0;
	CHECK(v->sh->status[0].drainedGen.load() == gen);
	CHECK(v->sh->status[0].position(gen, &frame, &r) && frame == 8000 && r == 32000);
	CHECK(GenBefore(0xfffffff0u, gen) && gen < 0x1000);
	CHECK(v->violations.load() == 0);
	CHECK(v->checked > 10000);
	CHECK(v->clockBlocks.load() > 50 && v->unitBlocks.load() > 50);
	CHECK(g.unpublished > 20 && v->superseded > 20);
	printf("race: gens to %u, %u posts left unpublished, %u blocks rendered behind a post, %u stream frames checked, renders clock %u unit %u, underruns %u\n", gen,
		g.unpublished, v->superseded, v->checked, v->clockBlocks.load(), v->unitBlocks.load(), v->sh->underruns.load());
	delete worker;
	delete mirror;
	delete v->mx;
	delete v->sh;
	delete v;
}

int
main(void)
{
	InitResampler();
	TestSeekAndPosition();
	TestOneShotPadsTheLead();
	TestGenerationsAndPause();
	TestReaderWaitsForTheSnapshot();
	TestStopShape();
	TestPauseFadesTheSignal();
	TestGainsRampAndCross();
	TestEndAndLoop();
	TestLoopFlagChangesOnAnOpenStream();
	TestShortReadEnds();
	TestUnderrunAndFailure();
	TestRateChange();
	TestAudiblePosition();
	TestGenerationWrap();
	TestSupersededCommandsDropped();
	TestThreadedPrefill();
	TestTerminateInMidDecode();
	TestStarvedWorkerRetriesSoon();
	TestPostWithoutPublish();
	TestPostWithoutPublishThreaded();
	TestWorkerSleepsAtHighRates();
	TestResumeRampsIn();
	TestRingIndexWrap();
	TestRenderersAndWorkerRace();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all stream tests passed\n");
	return 0;
}
