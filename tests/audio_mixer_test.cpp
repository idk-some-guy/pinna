// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -I src tests/audio_mixer_test.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaStream.cpp -o build/tests/audio_mixer_test && build/tests/audio_mixer_test
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include "CaMixer.h"
#include "CaStats.h"
#include "CaFilter.h"
#include "CaSpatial.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static const float kRate = 48000.0f;

struct Bus
{
	std::vector<float> dryL, dryR, send, strL, strR;
	explicit Bus(uint32_t n) : dryL(n, 9.0f), dryR(n, 9.0f), send(n, 9.0f), strL(n, 9.0f), strR(n, 9.0f) {}
};

static std::vector<int16_t>
Sine(int n)
{
	std::vector<int16_t> s(n);
	for(int i = 0; i < n; i++)
		s[i] = (int16_t)lrintf(0.5f * 32767.0f * sinf(2.0f * 3.14159265f * 440.0f * i / 22050.0f));
	return s;
}

static VoiceSnap
Voice(const std::vector<int16_t> &s, uint32_t playId, bool loop, int8_t slot)
{
	VoiceSnap v = {};
	v.data = s.data();
	v.length = (uint32_t)s.size();
	v.loopStart = 0;
	v.loopEnd = (uint32_t)s.size();
	v.playId = playId;
	v.freq = 22050.0f;
	v.gainL = 1.0f;
	v.gainR = 0.5f;
	v.send = v.envSend = 0.25f;
	v.sendHF = v.envSendHF = 1.0f;
	v.loop = loop;
	v.playing = 1;
	v.poolSlot = slot;
	return v;
}

static void
Publish(Shared &sh, Snapshot &mirror)
{
	mirror.serial++;
	sh.snapshot.writeBuffer() = mirror;
	sh.snapshot.publish();
}

static void
Render(Mixer &m, Bus &b, uint32_t at, uint32_t n)
{
	m.render(&b.dryL[at], &b.dryR[at], &b.send[at], &b.strL[at], &b.strR[at], n, kRate);
}

static float
Peak(const std::vector<float> &x, uint32_t from, uint32_t to)
{
	float p = 0.0f;
	for(uint32_t i = from; i < to; i++)
		p = fmaxf(p, fabsf(x[i]));
	return p;
}

static void
TestSilence(void)
{
	static Shared sh;
	Mixer m(&sh);
	Bus b(512);
	Render(m, b, 0, 512);
	CHECK(Peak(b.dryL, 0, 512) == 0.0f && Peak(b.dryR, 0, 512) == 0.0f && Peak(b.send, 0, 512) == 0.0f);
	CHECK(Peak(b.strL, 0, 512) == 0.0f && Peak(b.strR, 0, 512) == 0.0f);
	CHECK(sh.activeVoices.load() == 0);
	for(int c = 0; c < kChannels; c++)
		CHECK(sh.readerSlot[c].load() == -1 && sh.finishedId[c].load() == 0);
}

static void
TestPlayingVoice(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	mirror.voice[5] = Voice(s, 1, true, 17);
	mirror.voice[kChannels - 1] = Voice(s, 1, true, -1);
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(512);
	Render(m, b, 0, 512);
	CHECK(sh.activeVoices.load() == 2);
	CHECK(sh.readerSlot[5].load() == 17);
	CHECK(sh.readerSlot[kChannels - 1].load() == -1);
	float l = Peak(b.dryL, 0, 512), r = Peak(b.dryR, 0, 512), snd = Peak(b.send, 0, 512);
	CHECK(l > 0.4f && fabsf(r / l - 0.5f) < 1e-4f && fabsf(snd / l - 0.25f) < 1e-4f);
	CHECK(Peak(b.strL, 0, 512) == 0.0f && Peak(b.strR, 0, 512) == 0.0f);
}

static bool
RenderSpatial(Mixer &m, Bus &b, uint32_t at, uint32_t n, int32_t algorithm)
{
	SpatialRender sp = { algorithm };
	return m.render(&b.dryL[at], &b.dryR[at], &b.send[at], &b.strL[at], &b.strR[at], n, kRate, &sp);
}

static VoiceSnap
Positioned(const std::vector<int16_t> &s, uint32_t playId, float az, float el, float monoGain)
{
	VoiceSnap v = Voice(s, playId, true, -1);
	v.azimuth = az;
	v.elevation = el;
	v.monoGain = monoGain;
	v.routeBypass = 0;
	return v;
}

static void
TestSpatialRouting(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	mirror.mode = MODE_SPATIAL;
	mirror.voice[5] = Positioned(s, 1, 30.0f, 10.0f, 0.6f);
	mirror.voice[7] = Voice(s, 1, true, -1);
	mirror.voice[7].routeBypass = 1;
	Publish(sh, mirror);
	Mixer m(&sh);
	CHECK(m.mono(0) != NULL && m.mono(5) != m.mono(6));
	Bus b(1024);
	CHECK(RenderSpatial(m, b, 0, 512, kSpatialUseOutputType));

	static Shared ref;
	static Snapshot classic;
	classic.voice[7] = Voice(s, 1, true, -1);
	Publish(ref, classic);
	Mixer rm(&ref);
	Bus rb(512);
	Render(rm, rb, 0, 512);
	CHECK(memcmp(b.dryL.data(), rb.dryL.data(), 512 * sizeof(float)) == 0);
	CHECK(memcmp(b.dryR.data(), rb.dryR.data(), 512 * sizeof(float)) == 0);

	VoiceSnap mono = Voice(s, 1, true, -1);
	mono.gainL = 0.6f;
	mono.gainR = 0.0f;
	VoiceState st = {};
	StartVoice(st, mono);
	std::vector<float> l(512, 0.0f), x(512, 0.0f);
	RenderVoice(st, mono, kRate, l.data(), NULL, x.data(), 512);
	CHECK(memcmp(m.mono(5), l.data(), 512 * sizeof(float)) == 0);
	CHECK(Peak(l, 0, 512) > 0.25f);
	bool silent = true;
	for(int c = 0; c < kChannels; c++)
		for(int i = 0; c != 5 && i < 512; i++)
			silent = silent && m.mono(c)[i] == 0.0f;
	CHECK(silent);
	CHECK(Peak(b.send, 0, 512) > 0.2f);
	Direction d = m.direction(5);
	CHECK(d.azimuth == 30.0f && d.elevation == 10.0f);

	mirror.voice[5].playing = 0;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(m.mono(5)[0] != 0.0f && m.mono(5)[64] == 0.0f);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(Peak(std::vector<float>(m.mono(5), m.mono(5) + 512), 0, 512) == 0.0f);
	mirror.voice[5] = Positioned(s, 2, 30.0f, 10.0f, 0.6f);
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);

	CHECK(!m.render(&b.dryL[512], &b.dryR[512], &b.send[512], &b.strL[512], &b.strR[512], 512, kRate));
	CHECK(Peak(b.dryL, 512, 1024) > 0.4f);
}

static void
TestSpatialClassicMode(void)
{
	static Shared sh, ref;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	mirror.voice[5] = Positioned(s, 1, 30.0f, 10.0f, 0.6f);
	Publish(sh, mirror);
	Publish(ref, mirror);
	Mixer m(&sh), rm(&ref);
	Bus b(512), rb(512);
	CHECK(!RenderSpatial(m, b, 0, 512, kSpatialUseOutputType));
	Render(rm, rb, 0, 512);
	CHECK(memcmp(b.dryL.data(), rb.dryL.data(), 512 * sizeof(float)) == 0 && Peak(b.dryL, 0, 512) > 0.4f);
	CHECK(memcmp(b.send.data(), rb.send.data(), 512 * sizeof(float)) == 0);
}

static float
MinMono(const Mixer &m, int c)
{
	float lo = 1e9f;
	for(int i = 0; i < 512; i++)
		lo = fminf(lo, fabsf(m.mono(c)[i]));
	return lo;
}

static void
TestJumpElapsedFromPublish(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	for(int i = 0; i < (int)s.size(); i++)
		s[i] = 16000;
	mirror.mode = MODE_SPATIAL;
	mirror.voice[6] = Positioned(s, 1, 0.0f, 0.0f, 0.5f);
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(512);
	for(int k = 0; k < 3; k++)
		RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	mirror.voice[6].azimuth = 80.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(MinMono(m, 6) > 0.1f);
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	mirror.voice[6].azimuth = 0.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(MinMono(m, 6) == 0.0f);
}

static void
TestSpatialDirection(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	mirror.mode = MODE_SPATIAL;
	mirror.voice[6] = Positioned(s, 1, 0.0f, 0.0f, 0.5f);
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(512);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	mirror.voice[6].azimuth = 30.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(fabsf(m.direction(6).azimuth - 9.6f) < 1e-3f);
	RenderSpatial(m, b, 0, 512, kSpatialHRTFHQ);
	CHECK(fabsf(m.direction(6).azimuth - 29.6f) < 1e-3f);

	mirror.voice[6] = Positioned(s, 2, -120.0f, 5.0f, 0.5f);
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(m.direction(6).azimuth == -120.0f && m.direction(6).elevation == 5.0f);

	mirror.voice[6].azimuth = 60.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(m.direction(6).azimuth == -120.0f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 512), 0, 128) > 0.1f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 512), 256, 512) == 0.0f);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(m.direction(6).azimuth == 60.0f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 512), 0, 512) == 0.0f);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	RenderSpatial(m, b, 0, 512, kSpatialUseOutputType);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 512), 256, 512) > 0.2f);
}

static void
TestSpatialChunkedDirections(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(22050);
	mirror.mode = MODE_SPATIAL;
	mirror.voice[6] = Positioned(s, 1, 0.0f, 0.0f, 0.5f);
	mirror.voice[4] = Voice(s, 1, true, -1);
	mirror.voice[4].routeBypass = 1;
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(4096);
	RenderSpatial(m, b, 0, 4096, kSpatialUseOutputType);
	CHECK(m.chunks() == 8);
	mirror.voice[6].azimuth = 60.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 4096, kSpatialUseOutputType);
	CHECK(m.chunks() == 8);
	for(uint32_t k = 0; k < 6; k++)
		CHECK(fabsf(m.direction(6, k).azimuth - 9.6f * (k + 1)) < 1e-3f);
	CHECK(m.direction(6, 6).azimuth == 60.0f && m.direction(6, 7).azimuth == 60.0f);
	CHECK(m.direction(6).azimuth == 60.0f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 4096), 3584, 4096) > 0.2f);
	CHECK(m.direction(4, 3).azimuth == m.direction(4).azimuth);

	mirror.voice[6].azimuth = -120.0f;
	Publish(sh, mirror);
	RenderSpatial(m, b, 0, 3000, kSpatialUseOutputType);
	CHECK(m.chunks() == 6);
	CHECK(m.direction(6, 0).azimuth == 60.0f);
	CHECK(m.direction(6, 1).azimuth == -120.0f && m.direction(6, 5).azimuth == -120.0f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 3000), 0, 128) > 0.1f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 3000), 256, 1536) == 0.0f);
	CHECK(Peak(std::vector<float>(m.mono(6), m.mono(6) + 3000), 2600, 3000) > 0.2f);
}

static std::vector<int16_t> &Ramp(void);

struct SpatialScene
{
	Shared *sh;
	Snapshot *mirror;
	Mixer *m;
	Bus b;
	SpatialScene(void) : sh(new Shared()), mirror(new Snapshot()), m(NULL), b(2048)
	{
		memset(mirror, 0, sizeof(*mirror));
		mirror->mode = MODE_SPATIAL;
	}
	~SpatialScene(void)
	{
		delete m;
		delete mirror;
		delete sh;
	}
	void publish(void)
	{
		Publish(*sh, *mirror);
		if(m == NULL)
			m = new Mixer(sh);
	}
	std::vector<float> render(uint32_t at, uint32_t n)
	{
		RenderSpatial(*m, b, at, n, kSpatialUseOutputType);
		return std::vector<float>(m->mono(6), m->mono(6) + n);
	}
};

static VoiceSnap
TwoD(const std::vector<int16_t> &s, uint32_t playId)
{
	VoiceSnap v = Voice(s, playId, true, -1);
	v.routeBypass = 1;
	return v;
}

static double
Worst(const std::vector<float> &a, uint32_t aFrom, const std::vector<float> &b, uint32_t bFrom, uint32_t n)
{
	double worst = 0.0;
	for(uint32_t i = 0; i < n; i++)
		worst = fmax(worst, fabs(a[aFrom + i] - b[bFrom + i]));
	return worst;
}

static void
TestReusedChannelPositionedThen2D(void)
{
	std::vector<int16_t> &s = Ramp();
	SpatialScene t, tail, fresh;
	t.mirror->voice[6] = Positioned(s, 1, 30.0f, 0.0f, 0.6f);
	tail.mirror->voice[6] = t.mirror->voice[6];
	t.publish();
	tail.publish();
	t.render(0, 512);
	tail.render(0, 512);
	t.mirror->voice[6] = TwoD(s, 2);
	t.publish();
	std::vector<float> mono = t.render(512, 512);
	tail.mirror->voice[6].playing = 0;
	tail.publish();
	std::vector<float> monoTail = tail.render(512, 512);
	fresh.mirror->voice[6] = TwoD(s, 2);
	fresh.publish();
	fresh.render(0, 512);
	CHECK(Peak(monoTail, 0, 64) > 0.05f && Peak(monoTail, 64, 512) == 0.0f);
	CHECK(Worst(mono, 0, monoTail, 0, 512) <= 1e-6);
	CHECK(Peak(fresh.b.dryL, 0, 512) > 0.4f);
	CHECK(Worst(t.b.dryL, 512, fresh.b.dryL, 0, 512) <= 1e-6);
	CHECK(Worst(t.b.dryR, 512, fresh.b.dryR, 0, 512) <= 1e-6);
}

static void
TestReusedChannel2DThenPositioned(void)
{
	std::vector<int16_t> &s = Ramp();
	SpatialScene t, tail, fresh;
	t.mirror->voice[6] = TwoD(s, 1);
	tail.mirror->voice[6] = t.mirror->voice[6];
	t.publish();
	tail.publish();
	t.render(0, 512);
	tail.render(0, 512);
	t.mirror->voice[6] = Positioned(s, 2, -40.0f, 0.0f, 0.6f);
	t.publish();
	std::vector<float> mono = t.render(512, 512);
	tail.mirror->voice[6].playing = 0;
	tail.publish();
	tail.render(512, 512);
	fresh.mirror->voice[6] = Positioned(s, 2, -40.0f, 0.0f, 0.6f);
	fresh.publish();
	std::vector<float> monoFresh = fresh.render(0, 512);
	CHECK(Peak(tail.b.dryR, 512, 576) > 0.02f && Peak(tail.b.dryL, 576, 1024) == 0.0f);
	CHECK(Worst(t.b.dryL, 512, tail.b.dryL, 512, 512) <= 1e-6);
	CHECK(Worst(t.b.dryR, 512, tail.b.dryR, 512, 512) <= 1e-6);
	CHECK(Peak(monoFresh, 0, 512) > 0.2f);
	CHECK(Worst(mono, 0, monoFresh, 0, 512) <= 1e-6);
}

static void
TestRouteFlipOnPlayingVoice(void)
{
	std::vector<int16_t> &s = Ramp();
	SpatialScene t, tail, always2D, alwaysMono;
	t.mirror->voice[6] = Positioned(s, 1, 30.0f, 0.0f, 0.6f);
	tail.mirror->voice[6] = t.mirror->voice[6];
	alwaysMono.mirror->voice[6] = t.mirror->voice[6];
	always2D.mirror->voice[6] = t.mirror->voice[6];
	always2D.mirror->voice[6].routeBypass = 1;
	t.publish();
	tail.publish();
	always2D.publish();
	alwaysMono.publish();
	SpatialScene *all[4] = { &t, &tail, &always2D, &alwaysMono };
	for(SpatialScene *k : all){
		k->render(0, 480);
		k->mirror->voice[6].monoGain = 0.3f;
		k->publish();
		k->render(480, 32);
	}
	t.mirror->voice[6].routeBypass = 1;
	t.publish();
	std::vector<float> mono = t.render(512, 512);
	tail.mirror->voice[6].playing = 0;
	tail.publish();
	std::vector<float> monoTail = tail.render(512, 512);
	always2D.render(512, 512);
	CHECK(Peak(monoTail, 0, 64) > 0.02f && Peak(monoTail, 64, 512) == 0.0f);
	CHECK(Worst(mono, 0, monoTail, 0, 512) <= 1e-6);
	CHECK(Peak(always2D.b.dryL, 512, 1024) > 0.4f);
	CHECK(Worst(t.b.dryL, 512, always2D.b.dryL, 512, 512) <= 1e-6);
	CHECK(Worst(t.b.dryR, 512, always2D.b.dryR, 512, 512) <= 1e-6);
	CHECK(Worst(t.b.send, 0, always2D.b.send, 0, 1024) <= 1e-6);

	t.mirror->voice[6].routeBypass = 0;
	t.publish();
	mono = t.render(1024, 512);
	always2D.mirror->voice[6].playing = 0;
	always2D.publish();
	always2D.render(1024, 512);
	alwaysMono.render(512, 512);
	std::vector<float> monoRef = alwaysMono.render(1024, 512);
	CHECK(Peak(always2D.b.dryR, 1024, 1088) > 0.02f && Peak(always2D.b.dryL, 1088, 1536) == 0.0f);
	CHECK(Worst(t.b.dryL, 1024, always2D.b.dryL, 1024, 512) <= 1e-6);
	CHECK(Worst(t.b.dryR, 1024, always2D.b.dryR, 1024, 512) <= 1e-6);
	CHECK(Peak(monoRef, 0, 512) > 0.1f);
	CHECK(Worst(mono, 0, monoRef, 0, 512) <= 1e-6);
	CHECK(t.m->direction(6).azimuth == 30.0f);
}

static void
TestRouteFlipWithStop(void)
{
	std::vector<int16_t> &s = Ramp();
	SpatialScene t, tail;
	t.mirror->voice[6] = Positioned(s, 1, 30.0f, 0.0f, 0.6f);
	tail.mirror->voice[6] = t.mirror->voice[6];
	t.publish();
	tail.publish();
	t.render(0, 512);
	tail.render(0, 512);
	t.mirror->voice[6].routeBypass = 1;
	t.mirror->voice[6].playing = 0;
	t.publish();
	std::vector<float> mono = t.render(512, 512);
	tail.mirror->voice[6].playing = 0;
	tail.publish();
	std::vector<float> monoTail = tail.render(512, 512);
	CHECK(Peak(monoTail, 0, 64) > 0.05f && Worst(mono, 0, monoTail, 0, 512) <= 1e-6);
	CHECK(Peak(t.b.dryL, 512, 1024) == 0.0f && Peak(t.b.dryR, 512, 1024) == 0.0f);
	CHECK(Peak(tail.b.send, 512, 576) > 0.01f && Worst(t.b.send, 512, tail.b.send, 512, 512) <= 1e-6);
}

static std::vector<float>
VoiceStop(const std::vector<int16_t> &s, float gain, uint32_t stopAt)
{
	VoiceSnap sn = Voice(s, 1, true, -1);
	sn.gainL = gain;
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(stopAt + 128), r(stopAt + 128), x(stopAt + 128);
	RenderVoice(st, sn, kRate, l.data(), r.data(), x.data(), stopAt);
	sn.playing = 0;
	RenderVoice(st, sn, kRate, &l[stopAt], &r[stopAt], &x[stopAt], 128);
	return std::vector<float>(l.begin() + stopAt, l.end());
}

static double
StopError(const std::vector<float> &x, uint32_t at, const std::vector<float> &stop)
{
	double worst = 0.0;
	for(uint32_t k = 0; k < 128; k++)
		worst = fmax(worst, fabs(x[at + k] - stop[k]));
	return worst;
}

static std::vector<int16_t> &
Ramp(void)
{
	static std::vector<int16_t> s;
	if(s.empty()){
		s.resize(30000);
		for(int i = 0; i < 30000; i++)
			s[i] = (int16_t)lrintf(6000.0f * sinf(2.0f * 3.14159265f * 523.0f * i / 22050.0f) + 9000.0f);
	}
	return s;
}

static void
TestStopRampsHeldValue(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Ramp(), poison(s.size(), -32768);
	mirror.voice[2] = Voice(s, 7, true, 30);
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(4096);
	Render(m, b, 0, 1024);
	mirror.voice[2].playing = 0;
	Publish(sh, mirror);
	Render(m, b, 1024, 16);
	CHECK(sh.readerSlot[2].load() == -1);
	CHECK(sh.activeVoices.load() == 0);
	CHECK(sh.finishedId[2].load() == 0);
	s.assign(poison.begin(), poison.end());
	Render(m, b, 1040, 1024);
	std::vector<float> stop = VoiceStop(Ramp(), 1.0f, 1024);
	CHECK(Peak(stop, 0, 64) > 0.05f && Peak(stop, 64, 128) == 0.0f);
	CHECK(StopError(b.dryL, 1024, stop) <= 1e-6);
	CHECK(Peak(b.dryL, 1088, 2064) == 0.0f && Peak(b.dryR, 1088, 2064) == 0.0f);
}

struct Scene
{
	Shared *sh;
	Snapshot *mirror;
	Mixer *m;
	Bus b;
	Scene(void) : sh(new Shared()), mirror(new Snapshot()), m(NULL), b(4096)
	{
		memset(mirror, 0, sizeof(*mirror));
	}
	~Scene(void)
	{
		delete m;
		delete mirror;
		delete sh;
	}
	void publish(void)
	{
		Publish(*sh, *mirror);
		if(m == NULL)
			m = new Mixer(sh);
	}
	void render(uint32_t at, uint32_t n)
	{
		Render(*m, b, at, n);
	}
};

static void
CheckRestart(uint32_t stopAt, uint32_t restartAt, float hf)
{
	std::vector<int16_t> &s = Ramp();
	std::vector<int16_t> old = s;
	Scene both, oldOnly, newOnly;
	both.mirror->voice[6] = Voice(old, 3, true, 11);
	oldOnly.mirror->voice[6] = Voice(s, 3, true, 11);
	both.mirror->voice[6].sendHF = oldOnly.mirror->voice[6].sendHF = hf;
	both.publish();
	oldOnly.publish();
	newOnly.publish();
	both.render(0, stopAt);
	oldOnly.render(0, stopAt);
	newOnly.render(0, restartAt);
	if(stopAt < restartAt){
		both.mirror->voice[6].playing = 0;
		both.publish();
		both.render(stopAt, restartAt - stopAt);
	}
	oldOnly.mirror->voice[6].playing = 0;
	oldOnly.publish();
	oldOnly.render(stopAt, 2048 - stopAt);
	both.mirror->voice[6] = Voice(s, 4, true, 12);
	both.mirror->voice[6].gainL = 0.7f;
	both.mirror->voice[6].sendHF = hf;
	both.publish();
	newOnly.mirror->voice[6] = both.mirror->voice[6];
	newOnly.publish();
	both.render(restartAt, 16);
	CHECK(both.sh->readerSlot[6].load() == 12);
	CHECK(both.sh->activeVoices.load() == 1);
	std::fill(old.begin(), old.end(), (int16_t)-32768);
	both.render(restartAt + 16, 2048 - restartAt - 16);
	newOnly.render(restartAt, 2048 - restartAt);
	CHECK(both.sh->finishedId[6].load() == 0);
	double worst = 0.0;
	for(uint32_t i = restartAt; i < 2048; i++){
		worst = fmax(worst, fabs(both.b.dryL[i] - oldOnly.b.dryL[i] - newOnly.b.dryL[i]));
		worst = fmax(worst, fabs(both.b.dryR[i] - oldOnly.b.dryR[i] - newOnly.b.dryR[i]));
		worst = fmax(worst, fabs(both.b.send[i] - oldOnly.b.send[i] - newOnly.b.send[i]));
	}
	CHECK(worst <= 1e-6);
	std::vector<float> stop = VoiceStop(s, 1.0f, stopAt);
	CHECK(Peak(stop, 0, 64) > 0.05f);
	CHECK(StopError(oldOnly.b.dryL, stopAt, stop) <= 1e-6);
	CHECK(Peak(oldOnly.b.dryL, stopAt + 64, 2048) == 0.0f);
}

static void
TestRestartFadesOldVoice(void)
{
	CheckRestart(512, 544, 1.0f);
	CheckRestart(512, 512, 1.0f);
	CheckRestart(512, 544, 0.3f);
	CheckRestart(512, 512, 0.3f);
}

static void
TestOneShotEnds(void)
{
	static Shared sh;
	static Snapshot mirror;
	std::vector<int16_t> s = Sine(1000);
	mirror.voice[0] = Voice(s, 21, false, 40);
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(8192);
	Render(m, b, 0, 1024);
	CHECK(sh.finishedId[0].load() == 0 && sh.readerSlot[0].load() == 40);
	Render(m, b, 1024, 2048);
	CHECK(sh.finishedId[0].load() == 21);
	CHECK(sh.readerSlot[0].load() == -1);
	Render(m, b, 3072, 512);
	CHECK(sh.activeVoices.load() == 0);
	CHECK(Peak(b.dryL, 3072, 3584) == 0.0f);
	uint32_t end = 0;
	for(double pos = 0.0, step = 22050.0f / kRate; pos < 1000.0; pos += step)
		end++;
	float last = s[999] / 32768.0f;
	CHECK(fabsf(last) > 0.01f);
	CHECK(fabsf(b.dryL[end + 63] - last / 64.0f) <= 1e-6f);
	CHECK(Peak(b.dryL, end + 64, 3072) == 0.0f);
}

static void
TestEmptyVoiceFinishes(void)
{
	static Shared sh;
	static Snapshot mirror;
	static std::vector<int16_t> s = Sine(1000);
	mirror.voice[3] = Voice(s, 31, false, 12);
	mirror.voice[3].length = 0;
	mirror.voice[4] = Voice(s, 32, true, 13);
	mirror.voice[4].data = NULL;
	mirror.voice[5] = Voice(s, 33, false, 14);
	mirror.voice[5].playing = 0;
	Publish(sh, mirror);
	Mixer m(&sh);
	Bus b(512);
	Render(m, b, 0, 512);
	for(int c = 3; c <= 5; c++){
		CHECK(sh.finishedId[c].load() == (uint32_t)(28 + c));
		CHECK(sh.readerSlot[c].load() == -1);
	}
	CHECK(sh.activeVoices.load() == 0);
	CHECK(Peak(b.dryL, 0, 512) == 0.0f);
}

static void
RenderChunked(uint32_t chunk, std::vector<float> &L, std::vector<float> &R, std::vector<float> &S)
{
	static std::vector<int16_t> s = Sine(30000);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	mirror->voice[1] = Voice(s, 1, true, -1);
	mirror->voice[1].freq = 31000.0f;
	mirror->voice[6] = Voice(s, 1, false, 5);
	mirror->voice[6].freq = 17000.0f;
	Publish(*sh, *mirror);
	Mixer *m = new Mixer(sh);
	const uint32_t total = 16384;
	Bus b(total);
	for(uint32_t done = 0; done < total; done += chunk){
		if(done == total / 2){
			mirror->voice[1].freq = 25000.0f;
			mirror->voice[1].gainL = 0.3f;
			Publish(*sh, *mirror);
		}
		Render(*m, b, done, chunk);
	}
	L = b.dryL;
	R = b.dryR;
	S = b.send;
	delete m;
	delete mirror;
	delete sh;
}

static void
TestBlockSizes(void)
{
	std::vector<float> L0, R0, S0;
	RenderChunked(4096, L0, R0, S0);
	for(uint32_t chunk = 256; chunk < 4096; chunk *= 2){
		std::vector<float> L, R, S;
		RenderChunked(chunk, L, R, S);
		double worst = 0.0;
		for(size_t i = 0; i < L.size(); i++)
			worst = fmax(worst, fmax(fabs(L[i] - L0[i]), fmax(fabs(R[i] - R0[i]), fabs(S[i] - S0[i]))));
		CHECK(worst <= 1e-6);
	}
}

static std::vector<int16_t> &
TwoTones(void)
{
	static std::vector<int16_t> s;
	if(s.empty()){
		s.resize(30000);
		for(int i = 0; i < 30000; i++)
			s[i] = (int16_t)lrintf(16000.0f * (sinf(2.0f * 3.14159265f * 300.0f * i / 22050.0f) + sinf(2.0f * 3.14159265f * 7000.0f * i / 22050.0f)));
	}
	return s;
}

struct SendCase
{
	float hfA, hfB;
	bool voiceB;
	uint32_t switchAt;
	bool restartA;
};

static std::vector<float>
RenderSend(const SendCase &k)
{
	std::vector<int16_t> &s = TwoTones();
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	mirror->voice[3] = Voice(s, 1, true, -1);
	mirror->voice[3].send = 1.0f;
	mirror->voice[3].sendHF = k.hfA;
	if(k.voiceB){
		mirror->voice[7] = Voice(s, 1, true, -1);
		mirror->voice[7].send = 1.0f;
		mirror->voice[7].sendHF = k.hfB;
	}
	Publish(*sh, *mirror);
	Mixer *m = new Mixer(sh);
	const uint32_t total = 8192;
	Bus b(total);
	uint32_t fade = kOalFadeFrames;
	for(uint32_t done = 0; done < total;){
		uint32_t n = k.restartA && done < k.switchAt - fade && done + 512 > k.switchAt - fade ? k.switchAt - fade - done : 512;
		if(k.restartA && done == k.switchAt - fade){
			mirror->voice[3].playing = 0;
			Publish(*sh, *mirror);
			n = fade;
		}
		if(done == k.switchAt){
			if(k.restartA){
				mirror->voice[3].playId++;
				mirror->voice[3].playing = 1;
			}else
				mirror->voice[3].sendHF = k.hfB;
			Publish(*sh, *mirror);
		}
		Render(*m, b, done, n);
		done += n;
	}
	std::vector<float> out = b.send;
	delete m;
	delete mirror;
	delete sh;
	return out;
}

static std::vector<float>
Shelved(const std::vector<float> &x, float hf, uint32_t from, uint32_t to)
{
	std::vector<float> y(x.begin() + from, x.begin() + to);
	Biquad bq = {};
	HighShelf(bq, 5000.0f, kRate, hf);
	RunBiquad(bq, y.data(), (uint32_t)y.size());
	return y;
}

static double
WorstDiff(const std::vector<float> &a, uint32_t aFrom, const std::vector<float> &b, uint32_t n)
{
	double worst = 0.0;
	for(uint32_t i = 0; i < n; i++)
		worst = fmax(worst, fabs(a[aFrom + i] - b[i]));
	return worst;
}

static void
TestSendShelf(void)
{
	std::vector<float> flat = RenderSend({ 1.0f, 1.0f, false, 99999, false });
	std::vector<float> shelved = RenderSend({ 0.3f, 0.3f, false, 99999, false });
	CHECK(Peak(flat, 0, 8192) > 0.1f && WorstDiff(shelved, 0, Shelved(flat, 0.3f, 0, 8192), 8192) <= 1e-6);
	CHECK(Peak(shelved, 4096, 8192) < 0.8f * Peak(flat, 4096, 8192));

	std::vector<float> switched = RenderSend({ 0.3f, 0.6f, false, 4096, false });
	std::vector<float> ref = flat;
	Biquad bq = {};
	HighShelf(bq, 5000.0f, kRate, 0.3f);
	RunBiquad(bq, ref.data(), 4096);
	HighShelf(bq, 5000.0f, kRate, 0.6f);
	RunBiquad(bq, ref.data() + 4096, 4096);
	CHECK(WorstDiff(switched, 0, ref, 8192) <= 1e-6);
}

static void
TestSendShelfPerVoice(void)
{
	std::vector<float> flat = RenderSend({ 1.0f, 1.0f, false, 99999, false });
	std::vector<float> both = RenderSend({ 0.3f, 0.9f, true, 99999, false });
	std::vector<float> a = Shelved(flat, 0.3f, 0, 8192), b = Shelved(flat, 0.9f, 0, 8192);
	for(size_t i = 0; i < a.size(); i++)
		a[i] += b[i];
	CHECK(WorstDiff(both, 0, a, 8192) <= 2e-6);

	std::vector<float> restarted = RenderSend({ 0.3f, 0.3f, false, 4096, true });
	CHECK(WorstDiff(restarted, 4096, Shelved(flat, 0.3f, 0, 4096), 4096) <= 1e-6);
}

static void
RenderAt(Mixer &m, Bus &b, uint32_t at, uint32_t n, float rate)
{
	for(uint32_t done = 0; done < n; done += 512)
		m.render(&b.dryL[at + done], &b.dryR[at + done], &b.send[at + done], &b.strL[at + done], &b.strR[at + done], 512, rate);
}

static void
TestShelfAfterRateChange(void)
{
	const float newRate = 44100.0f;
	std::vector<int16_t> &s = TwoTones();
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	mirror->voice[3] = Voice(s, 1, true, -1);
	mirror->voice[3].send = 1.0f;
	mirror->voice[3].sendHF = 0.3f;
	Publish(*sh, *mirror);
	Mixer *m = new Mixer(sh);
	Bus b(16384);
	RenderAt(*m, b, 0, 2048, kRate);
	mirror->voice[3].playing = 0;
	Publish(*sh, *mirror);
	RenderAt(*m, b, 2048, 1024, kRate);
	RenderAt(*m, b, 3072, 2048, newRate);
	mirror->voice[3].playId = 2;
	mirror->voice[3].playing = 1;
	Publish(*sh, *mirror);
	RenderAt(*m, b, 8192, 8192, newRate);

	Shared *fsh = new Shared();
	Snapshot *fmirror = new Snapshot();
	memset(fmirror, 0, sizeof(*fmirror));
	fmirror->voice[3] = Voice(s, 1, true, -1);
	fmirror->voice[3].send = 1.0f;
	Publish(*fsh, *fmirror);
	Mixer *fm = new Mixer(fsh);
	Bus fb(8192);
	RenderAt(*fm, fb, 0, 8192, newRate);
	std::vector<float> ref = fb.send;
	Biquad bq = {};
	HighShelf(bq, 5000.0f, newRate, 0.3f);
	RunBiquad(bq, ref.data(), 8192);
	double worst = WorstDiff(b.send, 8192, ref, 8192);
	printf("MEASURED send after a rate change while idle: max abs difference %.3g against the 44.1 kHz shelf\n", worst);
	CHECK(Peak(ref, 0, 8192) > 0.1f && worst <= 1e-6);
	delete fm;
	delete fmirror;
	delete fsh;
	delete m;
	delete mirror;
	delete sh;
}

static bool
AllFinite(const std::vector<float> &x)
{
	for(float v : x)
		if(!std::isfinite(v))
			return false;
	return true;
}

static void
TestSendFiniteAt8k(void)
{
	const float rate = 8000.0f;
	std::vector<int16_t> s = Sine(22050);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	const float hf[] = { 0.0f, 0.25f, 0.5f, 1.0f };
	for(int c = 0; c < 4; c++){
		mirror->voice[c] = Voice(s, 1, true, -1);
		mirror->voice[c].send = 1.0f;
		mirror->voice[c].sendHF = hf[c];
	}
	Publish(*sh, *mirror);
	Mixer *m = new Mixer(sh);
	Bus b(512);
	bool finite = true;
	float peak = 0.0f;
	for(uint32_t done = 0; done < 10 * (uint32_t)rate; done += 512){
		m->render(&b.dryL[0], &b.dryR[0], &b.send[0], &b.strL[0], &b.strR[0], 512, rate);
		finite = finite && AllFinite(b.send) && AllFinite(b.dryL);
		peak = fmaxf(peak, Peak(b.send, 0, 512));
	}
	printf("MEASURED send at 8 kHz over ten seconds: finite %d, peak %.3f\n", finite, peak);
	CHECK(finite && peak > 0.1f && peak < 4.0f);
	delete m;
	delete mirror;
	delete sh;
}

static void
TestNonFiniteSendDropped(void)
{
	std::vector<int16_t> s = Sine(22050);
	Shared *sh = new Shared();
	Snapshot *mirror = new Snapshot();
	memset(mirror, 0, sizeof(*mirror));
	mirror->voice[0] = Voice(s, 1, true, -1);
	mirror->voice[0].send = 0.5f;
	mirror->voice[0].sendHF = 0.5f;
	mirror->voice[1] = Voice(s, 1, true, -1);
	mirror->voice[1].send = INFINITY;
	mirror->voice[1].sendHF = 0.5f;
	Publish(*sh, *mirror);
	Mixer *m = new Mixer(sh);
	Bus b(512);
	bool finite = true;
	for(int k = 0; k < 8; k++){
		Render(*m, b, 0, 512);
		finite = finite && AllFinite(b.send);
	}
	float peak = Peak(b.send, 0, 512);
	mirror->voice[1].send = 0.5f;
	Publish(*sh, *mirror);
	bool recovered = true;
	for(int k = 0; k < 8; k++){
		Render(*m, b, 0, 512);
		recovered = recovered && AllFinite(b.send);
	}
	float after = Peak(b.send, 0, 512);
	printf("MEASURED non-finite send: finite %d, peak %.3f, after recovery finite %d, peak %.3f\n", finite, peak, recovered, after);
	CHECK(finite && peak > 0.05f);
	CHECK(recovered && after > 1.5f * peak);
	delete m;
	delete mirror;
	delete sh;
}

static void
TestRendersCounted(void)
{
	static Shared sh;
	Mixer m(&sh);
	Bus b(512);
	CHECK(sh.renders.load() == 0);
	Render(m, b, 0, 256);
	Render(m, b, 256, 256);
	CHECK(sh.renders.load() == 2);
}

static void
TestStats(void)
{
	StatsInfo s = { 3, 43, 1, 2, 4, 48000, 512, "classic", "stereo", "MacBook Pro Speakers", NULL };
	char line[256];
	SetLogPrefix(NULL);
	FormatStats(line, sizeof(line), s);
	CHECK(strncmp(line, "pinna: stats voices 3/43 ", 25) == 0);
	SetLogPrefix("check:");
	int n = FormatStats(line, sizeof(line), s);
	CHECK(n == (int)strlen(line));
	CHECK(strcmp(line, "check: stats voices 3/43 streams 1 underruns 2 rebuilds 4 mode classic output stereo rate 48000 buffer 512 device \"MacBook Pro Speakers\"\n") == 0);
	char small[16];
	FormatStats(small, sizeof(small), s);
	CHECK(strlen(small) == 15);
	StatsInfo sp = { 3, 43, 1, 2, 4, 48000, 512, "spatial", "headphones", "A50 Main", "auto" };
	FormatStats(line, sizeof(line), sp);
	CHECK(strcmp(line, "check: stats voices 3/43 streams 1 underruns 2 rebuilds 4 mode spatial auto output headphones rate 48000 buffer 512 device \"A50 Main\"\n") == 0);
	sp.detail = "";
	FormatStats(line, sizeof(line), sp);
	CHECK(strstr(line, " mode spatial output headphones ") != NULL);
	SetLogPrefix(NULL);
}

static void
TestLogPrefix(void)
{
	SetLogPrefix(NULL);
	CHECK(strcmp(LogPrefix(), "pinna:") == 0 && strcmp(LogPrefix(), kDefaultLogPrefix) == 0);
	CHECK(!SetLogPrefix(NULL) && !SetLogPrefix(""));
	CHECK(SetLogPrefix("check:") && strcmp(LogPrefix(), "check:") == 0);
	CHECK(!SetLogPrefix("check:") && strcmp(LogPrefix(), "check:") == 0);
	CHECK(SetLogPrefix("other:") && strcmp(LogPrefix(), "other:") == 0);
	CHECK(SetLogPrefix(NULL) && strcmp(LogPrefix(), "pinna:") == 0);
	char longPrefix[100];
	memset(longPrefix, 'p', sizeof(longPrefix) - 1);
	longPrefix[sizeof(longPrefix) - 1] = '\0';
	CHECK(SetLogPrefix(longPrefix) && strlen(LogPrefix()) == 63);
	CHECK(!SetLogPrefix(longPrefix));
	SetLogPrefix(NULL);
}

struct ModeRun
{
	Shared *sh;
	Snapshot *mirror;
	Mixer *m;
	Bus b;
	std::vector<float> mono;
	std::vector<uint8_t> spatial;
	ModeRun(uint8_t mode, uint32_t total) : sh(new Shared()), mirror(new Snapshot()), m(NULL), b(total), mono(total, 0.0f), spatial(total / 512, 0)
	{
		memset(mirror, 0, sizeof(*mirror));
		mirror->mode = mode;
	}
	~ModeRun(void)
	{
		delete m;
		delete mirror;
		delete sh;
	}
	void publish(void)
	{
		Publish(*sh, *mirror);
		if(m == NULL)
			m = new Mixer(sh);
	}
	void block(uint32_t k)
	{
		spatial[k] = RenderSpatial(*m, b, k * 512, 512, kSpatialUseOutputType);
		if(spatial[k])
			memcpy(&mono[k * 512], m->mono(5), 512 * sizeof(float));
	}
};

static std::vector<float>
DipGains(uint32_t n)
{
	DipState d = { kDipIdle, 0 };
	ModeDipStart(d);
	std::vector<float> g(n);
	ModeDipFill(d, g.data(), n);
	return g;
}

static void
ModeVoices(ModeRun &r, const std::vector<int16_t> &s, float az)
{
	r.mirror->voice[5] = Positioned(s, 1, az, 0.0f, 0.6f);
	r.mirror->voice[7] = Voice(s, 1, true, -1);
	r.mirror->voice[7].routeBypass = 1;
	r.mirror->voice[7].gainL = 0.3f;
	r.mirror->voice[7].gainR = 0.2f;
}

static double
ScaledError(const std::vector<float> &x, uint32_t at, const std::vector<float> &ref, const std::vector<float> &g, uint32_t from, uint32_t n)
{
	double worst = 0.0;
	for(uint32_t i = from; i < from + n; i++)
		worst = fmax(worst, fabs(x[at + i] - ref[at + i] * g[i]));
	return worst;
}

static void
TestModeSwitchDip(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 40 * 512, on = 4 * 512, off = 12 * 512;
	ModeRun t(MODE_CLASSIC, total), classic(MODE_CLASSIC, total), spatial(MODE_SPATIAL, total), bypassOnly(MODE_SPATIAL, total);
	ModeVoices(t, s, 30.0f);
	ModeVoices(classic, s, 30.0f);
	ModeVoices(spatial, s, -100.0f);
	ModeVoices(bypassOnly, s, -100.0f);
	bypassOnly.mirror->voice[5].monoGain = 0.0f;
	ModeRun *all[4] = { &t, &classic, &spatial, &bypassOnly };
	for(ModeRun *r : all)
		r->publish();
	std::vector<float> g = DipGains(2048);
	for(uint32_t k = 0; k < total / 512; k++){
		if(k == 3){
			t.mirror->voice[5].azimuth = -100.0f;
			t.publish();
		}
		if(k * 512 == on){
			t.mirror->mode = MODE_SPATIAL;
			t.publish();
		}
		if(k * 512 == off){
			t.mirror->mode = MODE_CLASSIC;
			t.publish();
		}
		for(ModeRun *r : all)
			r->block(k);
		if(k * 512 == on + 1024)
			CHECK(t.m->direction(5).azimuth == -100.0f && t.m->direction(5, 0).azimuth == -100.0f);
	}
	CHECK(!t.spatial[on / 512] && !t.spatial[on / 512 + 1] && t.spatial[on / 512 + 2] && t.spatial[off / 512 - 1]);
	CHECK(Peak(t.b.dryL, on, on + 2048) > 0.1f && Peak(t.mono, on + 1024, on + 2048) > 0.05f);
	CHECK(ScaledError(t.b.dryL, on, classic.b.dryL, g, 0, 1024) <= 1e-6 && ScaledError(t.b.dryR, on, classic.b.dryR, g, 0, 1024) <= 1e-6);
	CHECK(ScaledError(t.b.send, on, classic.b.send, g, 0, 2048) <= 1e-6);
	CHECK(ScaledError(t.b.dryL, on, bypassOnly.b.dryL, g, 1024, 1024) <= 1e-6 && ScaledError(t.b.dryR, on, bypassOnly.b.dryR, g, 1024, 1024) <= 1e-6);
	CHECK(ScaledError(t.mono, on, spatial.mono, g, 1024, 1024) <= 1e-6);
	CHECK(Worst(t.mono, on + 2048, spatial.mono, on + 2048, off - on - 2048) <= 1e-6);
	CHECK(Worst(t.b.dryL, on + 2048, spatial.b.dryL, on + 2048, off - on - 2048) <= 1e-6);
	CHECK(Worst(t.b.send, on + 2048, spatial.b.send, on + 2048, off - on - 2048) <= 1e-6);

	CHECK(t.spatial[off / 512] && t.spatial[off / 512 + 1] && !t.spatial[off / 512 + 2]);
	CHECK(ScaledError(t.mono, off, spatial.mono, g, 0, 1024) <= 1e-6);
	CHECK(ScaledError(t.b.dryL, off, bypassOnly.b.dryL, g, 0, 1024) <= 1e-6);
	CHECK(ScaledError(t.b.dryL, off, classic.b.dryL, g, 1024, 1024) <= 1e-6 && ScaledError(t.b.dryR, off, classic.b.dryR, g, 1024, 1024) <= 1e-6);
	CHECK(Worst(t.b.dryL, off + 2048, classic.b.dryL, off + 2048, total - off - 2048) <= 1e-6);
	CHECK(Worst(t.b.dryR, off + 2048, classic.b.dryR, off + 2048, total - off - 2048) <= 1e-6);
	CHECK(Peak(t.b.dryL, total - 512, total) > 0.1f);
	CHECK(Peak(t.b.strL, 0, total) == 0.0f && Peak(t.b.strR, 0, total) == 0.0f);
}

static void
TestModeSwitchTurnsBack(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 16 * 512, on = 2 * 512;
	ModeRun t(MODE_CLASSIC, total), classic(MODE_CLASSIC, total);
	ModeVoices(t, s, 30.0f);
	ModeVoices(classic, s, 30.0f);
	t.publish();
	classic.publish();
	float worstStep = 0.0f;
	for(uint32_t k = 0; k < total / 512; k++){
		if(k * 512 == on){
			t.mirror->mode = MODE_SPATIAL;
			t.publish();
		}
		if(k * 512 == on + 1536){
			t.mirror->mode = MODE_CLASSIC;
			t.publish();
		}
		t.block(k);
		classic.block(k);
	}
	for(uint32_t i = on + 1; i < on + 4096; i++)
		worstStep = fmaxf(worstStep, fabsf(t.b.send[i] / classic.b.send[i] - t.b.send[i - 1] / classic.b.send[i - 1]));
	CHECK(t.spatial[(on + 1024) / 512] && !t.spatial[(on + 2048) / 512]);
	CHECK(Peak(t.b.send, on + 2040, on + 2056) < 1e-3f);
	CHECK(worstStep < 0.0025f);
	CHECK(Worst(t.b.dryL, on + 3072, classic.b.dryL, on + 3072, total - on - 3072) <= 1e-6);
}

static void
RenderModeSwitchInBlocks(ModeRun &r, uint32_t block, uint32_t on, uint32_t total)
{
	for(uint32_t at = 0; at < total; at += block){
		if(at == on){
			r.mirror->mode = MODE_SPATIAL;
			r.publish();
		}
		bool spatial = RenderSpatial(*r.m, r.b, at, block, kSpatialUseOutputType);
		if(spatial)
			memcpy(&r.mono[at], r.m->mono(5), block * sizeof(float));
	}
}

static void
TestModeSwitchMidBlock(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 9600, on = 2400;
	ModeRun a(MODE_CLASSIC, total), b(MODE_CLASSIC, total), classic(MODE_CLASSIC, total);
	ModeVoices(a, s, 30.0f);
	ModeVoices(b, s, 30.0f);
	ModeVoices(classic, s, 30.0f);
	a.publish();
	b.publish();
	classic.publish();
	RenderModeSwitchInBlocks(a, 300, on, total);
	RenderModeSwitchInBlocks(b, 96, on, total);
	RenderModeSwitchInBlocks(classic, 300, total, total);
	std::vector<float> g = DipGains(2048);
	CHECK(ScaledError(a.b.dryL, on, classic.b.dryL, g, 0, 1024) <= 1e-6);
	CHECK(Worst(a.b.dryL, 0, b.b.dryL, 0, total) <= 1e-6 && Worst(a.b.send, 0, b.b.send, 0, total) <= 1e-6);
	CHECK(Worst(a.mono, on + 1024, b.mono, on + 1024, total - on - 1024) <= 1e-6 && Peak(a.mono, on + 1100, total) > 0.1f);
	CHECK(Peak(a.mono, 0, on + 1024) == 0.0f && Peak(b.mono, 0, on + 1024) == 0.0f);
}

static void
TestModeSwitchDropsTails(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 8 * 512, on = 2 * 512;
	ModeRun t(MODE_CLASSIC, total), bypassOnly(MODE_SPATIAL, total);
	ModeVoices(t, s, 30.0f);
	ModeVoices(bypassOnly, s, 30.0f);
	bypassOnly.mirror->voice[5].monoGain = 0.0f;
	t.publish();
	bypassOnly.publish();
	for(uint32_t k = 0; k < total / 512; k++){
		if(k * 512 == on){
			t.mirror->mode = MODE_SPATIAL;
			t.publish();
		}
		if(k * 512 == on + 1024){
			t.mirror->voice[5].playing = 0;
			t.publish();
		}
		t.block(k);
		bypassOnly.block(k);
	}
	std::vector<float> g = DipGains(2048);
	CHECK(t.spatial[(on + 1024) / 512] && Peak(t.mono, on + 1024, total) == 0.0f);
	CHECK(ScaledError(t.b.dryL, on, bypassOnly.b.dryL, g, 1024, 1024) <= 1e-6 && ScaledError(t.b.dryR, on, bypassOnly.b.dryR, g, 1024, 1024) <= 1e-6);
}

static void
TestModeSwitchWhenSilent(void)
{
	std::vector<int16_t> &s = Ramp();
	ModeRun t(MODE_CLASSIC, 2048), spatial(MODE_SPATIAL, 2048);
	t.publish();
	t.block(0);
	t.mirror->mode = MODE_SPATIAL;
	ModeVoices(t, s, 30.0f);
	ModeVoices(spatial, s, 30.0f);
	t.publish();
	spatial.publish();
	for(uint32_t k = 1; k < 4; k++){
		t.block(k);
		spatial.block(k - 1);
	}
	CHECK(t.spatial[1] && Worst(t.mono, 512, spatial.mono, 0, 1536) <= 1e-6);
	CHECK(Worst(t.b.dryL, 512, spatial.b.dryL, 0, 1536) <= 1e-6);
}

static void
RuleVoices(ModeRun &r, const std::vector<int16_t> &s)
{
	ModeVoices(r, s, 30.0f);
	for(int c = 5; c <= 7; c += 2){
		r.mirror->voice[c].envSend = 0.6f;
		r.mirror->voice[c].envSendHF = 0.4f;
	}
}

static void
TestSendRuleFollowsRouting(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 32 * 512, on = 4 * 512, off = 16 * 512;
	ModeRun t(MODE_CLASSIC, total), classic(MODE_CLASSIC, total), spatial(MODE_SPATIAL, total);
	ModeRun *all[3] = { &t, &classic, &spatial };
	for(ModeRun *r : all){
		RuleVoices(*r, s);
		r->publish();
	}
	for(uint32_t k = 0; k < total / 512; k++){
		if(k * 512 == on || k * 512 == off){
			t.mirror->mode = k * 512 == on ? MODE_SPATIAL : MODE_CLASSIC;
			t.publish();
		}
		for(ModeRun *r : all)
			r->block(k);
	}
	std::vector<float> g = DipGains(2048);
	CHECK(Peak(classic.b.send, 0, total) > 0.05f && Peak(spatial.b.send, 0, total) > 1.5f * Peak(classic.b.send, 0, total));
	CHECK(Worst(t.b.send, 0, classic.b.send, 0, on) <= 1e-6);
	CHECK(ScaledError(t.b.send, on, classic.b.send, g, 0, 1024) <= 1e-6);
	CHECK(Worst(t.b.send, on + 2048, spatial.b.send, on + 2048, off - on - 2048) <= 1e-6);
	CHECK(ScaledError(t.b.send, off, spatial.b.send, g, 0, 1024) <= 1e-6);
	CHECK(Worst(t.b.send, off + 2048, classic.b.send, off + 2048, total - off - 2048) <= 1e-6);
	float worstStep = 0.0f;
	for(uint32_t i = on + 1; i < off + 2048; i++)
		worstStep = fmaxf(worstStep, fabsf(t.b.send[i] - t.b.send[i - 1]));
	CHECK(worstStep < 0.02f);
}

static void
TestSpatialSendSkipsFlatShelf(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 8 * 512;
	ModeRun r(MODE_SPATIAL, total);
	r.mirror->voice[7] = Voice(s, 1, true, -1);
	r.mirror->voice[7].routeBypass = 1;
	r.mirror->voice[7].gainL = r.mirror->voice[7].envSend = 0.25f;
	r.mirror->voice[7].envSendHF = 1.0f;
	r.publish();
	for(uint32_t k = 0; k < total / 512; k++)
		r.block(k);
	CHECK(Peak(r.b.send, 0, total) > 0.05f);
	CHECK(memcmp(r.b.send.data(), r.b.dryL.data(), total * sizeof(float)) == 0);

	ModeRun classic(MODE_CLASSIC, total);
	classic.mirror->voice[7] = r.mirror->voice[7];
	classic.mirror->voice[7].send = 0.25f;
	classic.mirror->voice[7].sendHF = 1.0f;
	classic.publish();
	for(uint32_t k = 0; k < total / 512; k++)
		classic.block(k);
	CHECK(memcmp(classic.b.send.data(), classic.b.dryL.data(), total * sizeof(float)) != 0);

	ModeRun ring(MODE_SPATIAL, total);
	ring.mirror->voice[7] = r.mirror->voice[7];
	ring.mirror->voice[7].envSendHF = 0.3f;
	ring.publish();
	for(uint32_t k = 0; k < 4; k++)
		ring.block(k);
	ring.mirror->voice[7].envSendHF = 1.0f;
	ring.publish();
	for(uint32_t k = 4; k < total / 512; k++)
		ring.block(k);
	CHECK(Worst(ring.b.send, 2048, ring.b.dryL, 2048, 64) > 1e-4);
}

static void
TestMixerRouteSpan(void)
{
	std::vector<int16_t> &s = Ramp();
	const uint32_t total = 9600, on = 2400;
	ModeRun t(MODE_CLASSIC, total);
	ModeVoices(t, s, 30.0f);
	t.mirror->env.place = ENV_TUNNEL;
	t.mirror->env.gain[ENV_TUNNEL] = 0.5f;
	t.mirror->env.gainHF[ENV_TUNNEL] = 0.25f;
	t.mirror->env.serial = 7;
	t.publish();
	uint32_t classic = 0, spatial = 0, mixed = 0;
	bool envSeen = true;
	for(uint32_t done = 0; done < total; done += 300){
		if(done == on){
			t.mirror->mode = MODE_SPATIAL;
			t.publish();
		}
		RenderSpatial(*t.m, t.b, done, 300, kSpatialUseOutputType);
		RouteSpan r = t.m->route();
		envSeen = envSeen && t.m->env().place == ENV_TUNNEL && t.m->env().gain[ENV_TUNNEL] == 0.5f && t.m->env().gainHF[ENV_TUNNEL] == 0.25f && t.m->env().serial == 7;
		classic += (r.before ? 0 : r.split) + (r.after ? 0 : 300 - r.split);
		spatial += (r.before ? r.split : 0) + (r.after ? 300 - r.split : 0);
		mixed += r.before != r.after;
		CHECK(r.split <= 300);
	}
	CHECK(envSeen);
	CHECK(classic == on + 1024 && spatial == total - on - 1024 && mixed == 1);
}

static bool
Near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static EnvSnap
EnvAt(int place, uint32_t serial, float gain = 1.0f, float gainHF = 1.0f)
{
	EnvSnap e;
	memset(&e, 0, sizeof(e));
	e.place = (int8_t)place;
	e.serial = serial;
	for(int u = 0; u < NUM_ENVS; u++){
		e.gain[u] = gain;
		e.gainHF[u] = gainHF;
	}
	return e;
}

struct OutBuffers
{
	std::vector<float> u[NUM_ENVS];
	float *ptr[NUM_ENVS];
	explicit OutBuffers(uint32_t n)
	{
		for(int k = 0; k < NUM_ENVS; k++){
			u[k].assign(n, 9.0f);
			ptr[k] = u[k].data();
		}
	}
};

static void
TestEnvBankObservesOncePerSerial(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	EnvSnap e = EnvAt(ENV_TUNNEL, 1);
	CHECK(!p.observe(e));
	CHECK(!p.observe(e) && !p.observe(e));
	CHECK(p.state().seen == 1 && p.state().current == ENV_OPEN);
	e.serial = 2;
	CHECK(p.observe(e));
	CHECK(p.state().current == ENV_TUNNEL && p.state().fadeLeft != 0);
}

static void
TestEnvBankFirstObserveAfterInit(void)
{
	EnvBank p;
	p.init(ENV_TUNNEL);
	EnvSnap e = EnvAt(ENV_OPEN, 0);
	CHECK(!p.observe(e) && p.state().seen == 1);
	e.serial = 1;
	CHECK(p.observe(e) && p.state().current == ENV_OPEN);
}

static void
TestEnvBankSendTimesWeightTimesGain(void)
{
	EnvBank p;
	p.init(ENV_TUNNEL);
	EnvSnap e = EnvAt(ENV_TUNNEL, 1);
	for(int u = 0; u < NUM_ENVS; u++)
		e.gain[u] = 0.1f * (float)(u + 1);
	std::vector<float> send(512, 0.5f);
	OutBuffers o(512);
	p.observe(e);
	p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
	for(int u = 0; u < NUM_ENVS; u++){
		if(u == ENV_TUNNEL)
			CHECK(o.u[u][0] == 0.5f * 0.3f && o.u[u][511] == 0.5f * 0.3f);
		else
			CHECK(Peak(o.u[u], 0, 512) == 0.0f);
	}
}

static void
TestEnvBankGainRamp(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	EnvSnap e = EnvAt(ENV_OPEN, 1, 0.2f);
	std::vector<float> send(1024, 1.0f);
	OutBuffers o(1024);
	p.split(send.data(), 0, 1024, e, o.ptr, 512, kRate);
	CHECK(o.u[ENV_OPEN][0] == 0.2f && o.u[ENV_OPEN][511] == 0.2f);
	e.gain[ENV_OPEN] = 0.8f;
	e.serial = 2;
	p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
	CHECK(Near(o.u[ENV_OPEN][0], 0.2f + 0.6f / kOalFadeFrames, 1e-6f));
	CHECK(Near(o.u[ENV_OPEN][31], 0.2f + 0.6f * 32 / kOalFadeFrames, 1e-6f));
	CHECK(o.u[ENV_OPEN][kOalFadeFrames - 1] == 0.8f && o.u[ENV_OPEN][511] == 0.8f);
	bool rising = true;
	for(uint32_t i = 1; i < kOalFadeFrames; i++)
		rising = rising && o.u[ENV_OPEN][i] > o.u[ENV_OPEN][i - 1];
	CHECK(rising);
}

static void
TestEnvBankGainRampBlockSizes(void)
{
	std::vector<float> send(1024), whole(1024), part(1024);
	for(uint32_t i = 0; i < 1024; i++)
		send[i] = 0.3f + 0.4f * (float)((i * 7919u) % 1000u) / 1000.0f;
	for(int pass = 0; pass < 2; pass++){
		EnvBank p;
		p.init(ENV_OPEN);
		OutBuffers o(1024);
		EnvSnap e = EnvAt(ENV_OPEN, 1, 0.2f);
		p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
		e.gain[ENV_OPEN] = 0.8f;
		e.serial = 2;
		uint32_t block = pass ? 7 : 512;
		for(uint32_t done = 0; done < 512; done += block){
			uint32_t n = 512 - done < block ? 512 - done : block;
			float *ptr[NUM_ENVS];
			for(int u = 0; u < NUM_ENVS; u++)
				ptr[u] = o.u[u].data() + 512 + done;
			p.split(&send[done], 0, n, e, ptr, n, kRate);
		}
		std::vector<float> &dst = pass ? part : whole;
		dst.assign(o.u[ENV_OPEN].begin() + 512, o.u[ENV_OPEN].end());
	}
	CHECK(memcmp(whole.data(), part.data(), 512 * sizeof(float)) == 0);
}

static void
TestEnvBankShelf(void)
{
	EnvBank p;
	p.init(ENV_GARAGE);
	EnvSnap e = EnvAt(ENV_GARAGE, 1, 0.5f, 0.25f);
	std::vector<float> send(1024), ref(1024);
	for(uint32_t i = 0; i < 1024; i++)
		send[i] = ref[i] = 0.4f * sinf(0.9f * (float)i) + 0.2f * sinf(0.05f * (float)i);
	Biquad shelf;
	memset(&shelf, 0, sizeof(shelf));
	HighShelf(shelf, 5000.0f, kRate, 0.25f);
	for(uint32_t i = 0; i < 1024; i++)
		ref[i] *= 0.5f;
	RunBiquad(shelf, ref.data(), 1024);
	OutBuffers o(1024);
	p.split(send.data(), 0, 1024, e, o.ptr, 1024, kRate);
	CHECK(Worst(o.u[ENV_GARAGE], 0, ref, 0, 1024) <= 1e-6);
	CHECK(Peak(o.u[ENV_OPEN], 0, 1024) == 0.0f && Peak(o.u[ENV_TUNNEL], 0, 1024) == 0.0f);

	EnvBank flat;
	flat.init(ENV_GARAGE);
	EnvSnap f = EnvAt(ENV_GARAGE, 1, 1.0f, 1.0f);
	flat.split(send.data(), 0, 1024, f, o.ptr, 1024, kRate);
	CHECK(memcmp(o.u[ENV_GARAGE].data(), send.data(), 1024 * sizeof(float)) == 0);
}

static void
TestEnvBankShelfFollowsGainChange(void)
{
	EnvBank p;
	p.init(ENV_GARAGE);
	std::vector<float> send(1024), ref(1024);
	for(uint32_t i = 0; i < 1024; i++)
		send[i] = ref[i] = 0.4f * sinf(0.9f * (float)i) + 0.2f * sinf(0.05f * (float)i);
	Biquad shelf;
	memset(&shelf, 0, sizeof(shelf));
	HighShelf(shelf, 5000.0f, kRate, 0.25f);
	RunBiquad(shelf, ref.data(), 512);
	HighShelf(shelf, 5000.0f, kRate, 0.6f);
	RunBiquad(shelf, ref.data() + 512, 512);
	OutBuffers o(1024);
	float *first[NUM_ENVS], *second[NUM_ENVS];
	for(int u = 0; u < NUM_ENVS; u++){
		first[u] = o.u[u].data();
		second[u] = o.u[u].data() + 512;
	}
	EnvSnap e = EnvAt(ENV_GARAGE, 1, 1.0f, 0.25f);
	p.split(send.data(), 0, 512, e, first, 512, kRate);
	e.gainHF[ENV_GARAGE] = 0.6f;
	e.serial = 2;
	p.split(send.data() + 512, 0, 512, e, second, 512, kRate);
	CHECK(Worst(o.u[ENV_GARAGE], 0, ref, 0, 1024) <= 1e-6);

	Biquad once;
	memset(&once, 0, sizeof(once));
	HighShelf(once, 5000.0f, kRate, 0.25f);
	std::vector<float> cmp(send);
	RunBiquad(once, cmp.data(), 1024);
	CHECK(Worst(cmp, 512, ref, 512, 512) > 1e-3);
}

static void
TestEnvBankShelfRingsOutAfterInputStops(void)
{
	EnvBank p;
	p.init(ENV_GARAGE);
	EnvSnap e = EnvAt(ENV_GARAGE, 1, 1.0f, 0.1f);
	std::vector<float> send(512, 0.0f);
	send[511] = 1.0f;
	OutBuffers o(512);
	p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
	std::vector<float> none(512, 0.0f);
	p.split(none.data(), 0, 512, e, o.ptr, 512, kRate);
	CHECK(Peak(o.u[ENV_GARAGE], 0, 16) > 1e-3f);
}

static void
TestEnvBankFlickerChangesNothing(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	std::vector<float> send(512, 0.5f);
	OutBuffers o(512);
	EnvSnap e = EnvAt(ENV_OPEN, 0, 0.3f);
	float worst = 0.0f;
	for(uint32_t k = 1; k <= 200; k++){
		e.place = (int8_t)(k & 1 ? ENV_TUNNEL : ENV_OPEN);
		e.serial = k;
		CHECK(!p.observe(e));
		p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
		worst = fmaxf(worst, fabsf(o.u[ENV_OPEN][511] - 0.15f));
		CHECK(Peak(o.u[ENV_TUNNEL], 0, 512) == 0.0f);
	}
	CHECK(worst == 0.0f && p.state().current == ENV_OPEN && p.state().fadeLeft == 0);
}

static void
EnvBankRun(uint32_t block, uint32_t total, OutBuffers &o)
{
	EnvBank p;
	p.init(ENV_OPEN);
	std::vector<float> send(total);
	for(uint32_t i = 0; i < total; i++)
		send[i] = 0.25f + 0.5f * (float)((i * 7919u) % 1000u) / 1000.0f;
	EnvSnap e = EnvAt(ENV_OPEN, 0);
	uint32_t serial = 0;
	for(uint32_t done = 0; done < total; done += block){
		if(done == 4096 || done == 8192 || done == 20480 || done == 24576){
			e.place = (int8_t)(done < 20000 ? ENV_GARAGE : ENV_TUNNEL);
			e.serial = ++serial;
			p.observe(e);
			e.serial = ++serial;
			p.observe(e);
		}
		float *ptr[NUM_ENVS];
		for(int u = 0; u < NUM_ENVS; u++)
			ptr[u] = o.u[u].data() + done;
		uint32_t n = total - done < block ? total - done : block;
		p.split(&send[done], 0, n, e, ptr, n, kRate);
	}
	for(uint32_t i = 0; i < total; i++)
		for(int u = 0; u < NUM_ENVS; u++)
			o.u[u][i] /= send[i];
}

static void
TestEnvBankBlockSizes(void)
{
	const uint32_t total = 24 * 4096;
	OutBuffers big(total), o(total);
	EnvBankRun(4096, total, big);
	CHECK(big.u[ENV_OPEN][4095] == 1.0f && Peak(big.u[ENV_GARAGE], 0, 4096) == 0.0f && big.u[ENV_GARAGE][4096] > 0.0f);
	CHECK(big.u[ENV_OPEN][12287] < 1.0f && big.u[ENV_GARAGE][12287] > 0.0f);
	CHECK(big.u[ENV_GARAGE][total - 1] == 0.0f && big.u[ENV_TUNNEL][total - 1] == 1.0f);
	float step = 0.0f, power = 0.0f;
	for(uint32_t i = 1; i < total; i++){
		float p = 0.0f;
		for(int u = 0; u < NUM_ENVS; u++){
			step = fmaxf(step, fabsf(big.u[u][i] - big.u[u][i - 1]));
			p += big.u[u][i] * big.u[u][i];
		}
		power = fmaxf(power, fabsf(p - 1.0f));
	}
	CHECK(step < 1e-3f && power < 1e-4f);
	static const uint32_t blocks[3] = { 256, 512, 1 };
	for(uint32_t k = 0; k < 3; k++){
		EnvBankRun(blocks[k], total, o);
		for(int u = 0; u < NUM_ENVS; u++)
			CHECK(memcmp(o.u[u].data(), big.u[u].data(), total * sizeof(float)) == 0);
	}
}

static void
TestEnvBankRouting(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	std::vector<float> send(512, 0.5f);
	OutBuffers o(512);
	EnvSnap e = EnvAt(ENV_OPEN, 0);
	p.split(send.data(), 100, 300, e, o.ptr, 512, kRate);
	CHECK(Peak(o.u[ENV_OPEN], 0, 100) == 0.0f && Peak(o.u[ENV_OPEN], 300, 512) == 0.0f);
	CHECK(o.u[ENV_OPEN][100] == 0.5f && o.u[ENV_OPEN][299] == 0.5f);
	for(int u = 1; u < NUM_ENVS; u++)
		CHECK(Peak(o.u[u], 0, 512) == 0.0f);
	p.split(send.data(), 0, 0, e, o.ptr, 512, kRate);
	for(int u = 0; u < NUM_ENVS; u++)
		CHECK(Peak(o.u[u], 0, 512) == 0.0f);
}

static void
TestEnvBankChangeDuringFadeHasNoStep(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	std::vector<float> send(4096, 1.0f);
	OutBuffers o(4096);
	EnvSnap e = EnvAt(ENV_TUNNEL, 1);
	p.observe(e);
	e.serial = 2;
	p.observe(e);
	p.split(send.data(), 0, 4096, e, o.ptr, 4096, kRate);
	p.split(send.data(), 0, 4096, e, o.ptr, 4096, kRate);
	float last[NUM_ENVS];
	for(int u = 0; u < NUM_ENVS; u++)
		last[u] = o.u[u][4095];
	e.place = ENV_GARAGE;
	e.serial = 3;
	CHECK(!p.observe(e));
	e.serial = 4;
	CHECK(p.observe(e) && p.state().current == ENV_GARAGE && p.state().fadeLeft != 0);
	p.split(send.data(), 0, 4096, e, o.ptr, 4096, kRate);
	float step = 0.0f;
	for(int u = 0; u < NUM_ENVS; u++)
		step = fmaxf(step, fabsf(o.u[u][0] - last[u]));
	CHECK(step < 1e-3f);
	CHECK(o.u[ENV_GARAGE][4095] > 0.0f && o.u[ENV_OPEN][4095] < last[ENV_OPEN] && o.u[ENV_TUNNEL][4095] < last[ENV_TUNNEL]);
}

static void
TestEnvBankSettle(void)
{
	EnvBank p;
	p.init(ENV_OPEN);
	std::vector<float> send(512, 1.0f);
	OutBuffers o(512);
	EnvSnap e = EnvAt(ENV_TUNNEL, 1);
	p.observe(e);
	e.serial = 2;
	p.observe(e);
	p.settle();
	CHECK(p.state().fadeLeft == 0 && p.state().current == ENV_TUNNEL);
	p.split(send.data(), 0, 512, e, o.ptr, 512, kRate);
	CHECK(o.u[ENV_TUNNEL][0] == 1.0f && Peak(o.u[ENV_OPEN], 0, 512) == 0.0f);
}

struct EchoScene
{
	SpatialScene s;
	EchoLines *lines;
	std::vector<float> bus[kEchoTaps], out[kEchoTaps];
	explicit EchoScene(float lineRate = 2000.0f) : lines(new EchoLines())
	{
		lines->allocate(lineRate);
	}
	~EchoScene(void)
	{
		delete lines;
	}
	EchoScene(const EchoScene &) = delete;
	EchoScene &operator=(const EchoScene &) = delete;
	void block(uint32_t n = 512)
	{
		RenderSpatial(*s.m, s.b, 0, n, kSpatialUseOutputType);
		lines->render(*s.m, kSpatialUseOutputType, kRate, 0, n, n);
		for(int t = 0; t < kEchoTaps; t++){
			const float *e = s.m->echo(t);
			const float *o = lines->live(t) ? lines->out(t) : NULL;
			for(uint32_t i = 0; i < n; i++){
				bus[t].push_back(e ? e[i] : 0.0f);
				out[t].push_back(o ? o[i] : 0.0f);
			}
		}
	}
	void blocks(int k)
	{
		for(int i = 0; i < k; i++)
			block();
	}
	void publish(void)
	{
		s.publish();
	}
};

static VoiceSnap
EchoVoice(const std::vector<int16_t> &s, uint32_t playId, float gain0, float freq)
{
	VoiceSnap v = Positioned(s, playId, 0.0f, 0.0f, 0.6f);
	v.freq = freq;
	v.echoGain[0] = gain0;
	return v;
}

static double
WorstIn(const std::vector<float> &a, size_t aFrom, const std::vector<float> &b, size_t bFrom, size_t n)
{
	double worst = 0.0;
	for(size_t i = 0; i < n; i++)
		worst = fmax(worst, fabs(a[aFrom + i] - b[bFrom + i]));
	return worst;
}

static void
TestEchoLineDelay(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene e;
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	e.s.mirror->echo.delayFrames[0] = 700;
	e.publish();
	e.blocks(8);
	const std::vector<float> &bus = e.bus[0], &out = e.out[0];
	CHECK(Peak(bus, 0, 4096) > 0.1f);
	CHECK(Peak(out, 0, 700) == 0.0f);
	CHECK(Peak(out, 700, 4096) > 0.1f);
	CHECK(WorstIn(out, 700, bus, 0, 4096 - 700) == 0.0);
	CHECK(e.lines->live(0) && !e.lines->live(1));
}

static void
TestEchoLineStaleGuard(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene e;
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	e.s.mirror->echo.delayFrames[0] = 700;
	e.publish();
	e.blocks(14);
	e.lines->cut();
	e.blocks(4);
	const std::vector<float> &bus = e.bus[0], &out = e.out[0];
	CHECK(Peak(bus, 7168, 9216) > 0.1f);
	CHECK(Peak(out, 7168, 7168 + 700) == 0.0f);
	CHECK(Peak(out, 7168 + 700, 9216) > 0.1f);
	CHECK(WorstIn(out, 7168 + 700, bus, 7168, 2048 - 700) == 0.0);
}

static void
TestEchoLineHeadNeverSteps(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene e;
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	e.s.mirror->echo.delayFrames[0] = 700;
	e.publish();
	e.blocks(4);
	size_t at = e.out[0].size();
	e.s.mirror->echo.delayFrames[0] = 701;
	e.publish();
	e.blocks(8);
	const std::vector<float> &bus = e.bus[0], &out = e.out[0];
	double worst = 0.0;
	for(size_t k = 0; k < 2048; k++){
		float w = EchoFadeWeight((uint32_t)k);
		double want = bus[at + k - 700] * (1.0f - w) + bus[at + k - 701] * w;
		worst = fmax(worst, fabs(out[at + k] - want));
	}
	CHECK(worst < 1e-6);
	CHECK(WorstIn(out, at + 2048, bus, at + 2048 - 701, 2048) < 1e-6);

	EchoScene f;
	f.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	f.s.mirror->echo.delayFrames[0] = 700;
	f.publish();
	f.blocks(4);
	size_t from = f.out[0].size();
	f.s.mirror->echo.delayFrames[0] = 701;
	f.publish();
	f.block();
	f.s.mirror->echo.delayFrames[0] = 702;
	f.publish();
	f.blocks(12);
	double mid = 0.0, tail = 0.0;
	for(size_t k = 512; k < 2048; k++){
		float w = EchoFadeWeight((uint32_t)k);
		mid = fmax(mid, fabs(f.out[0][from + k] - (f.bus[0][from + k - 700] * (1.0f - w) + f.bus[0][from + k - 701] * w)));
	}
	for(size_t k = 2048 + 2048; k < 2048 + 2048 + 1024; k++){
		tail = fmax(tail, fabs(f.out[0][from + k] - f.bus[0][from + k - 702]));
	}
	CHECK(mid < 1e-6);
	CHECK(tail < 1e-6);
}

static void
TestEchoBusSumsAndClears(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene both, one, two;
	both.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	both.s.mirror->voice[6] = EchoVoice(s, 1, 0.25f, 11025.0f);
	one.s.mirror->voice[5] = both.s.mirror->voice[5];
	two.s.mirror->voice[6] = both.s.mirror->voice[6];
	both.publish();
	one.publish();
	two.publish();
	both.blocks(3);
	one.blocks(3);
	two.blocks(3);
	CHECK(Peak(both.bus[0], 0, 1536) > 0.1f);
	CHECK(Peak(one.bus[0], 0, 1536) > 0.1f && Peak(two.bus[0], 0, 1536) > 0.05f);
	double worst = 0.0;
	for(size_t i = 0; i < 1536; i++)
		worst = fmax(worst, fabs(both.bus[0][i] - (one.bus[0][i] + two.bus[0][i])));
	CHECK(worst == 0.0);
	CHECK(Peak(both.bus[1], 0, 1536) == 0.0f);
}

static void
TestEchoGainRamp(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene fixed, ramp;
	fixed.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	ramp.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	fixed.publish();
	ramp.publish();
	fixed.block(40);
	ramp.block(40);
	ramp.s.mirror->voice[5].echoGain[0] = 0.25f;
	ramp.publish();
	fixed.block(40);
	ramp.block(40);
	fixed.block(40);
	ramp.block(40);
	fixed.block(40);
	ramp.block(40);
	double worst = 0.0;
	int checked = 0;
	for(uint32_t i = 40; i < 160; i++){
		float c = fixed.bus[0][i] / 0.5f;
		if(fabsf(c) < 0.05f)
			continue;
		uint32_t k = i - 40;
		float g = k < 64 ? 0.5f - 0.25f / 64.0f * (float)k : 0.25f;
		worst = fmax(worst, fabs(ramp.bus[0][i] - c * g));
		checked++;
	}
	CHECK(checked > 40 && worst < 2e-6);
}

static void
TestEchoFadeDroppedInClassicBlock(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene e;
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	e.publish();
	e.blocks(2);
	e.s.mirror->voice[5] = EchoVoice(s, 2, 0.0f, 22050.0f);
	e.publish();
	e.block(16);
	CHECK(Peak(e.bus[0], 1024, 1040) > 0.02f);
	Render(*e.s.m, e.s.b, 0, 16);
	size_t from = e.bus[0].size();
	e.blocks(3);
	CHECK(Peak(e.bus[0], from, e.bus[0].size()) == 0.0f);
}

static void
TestEchoCarrierFadeReachesTaps(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene a;
	a.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	a.publish();
	a.blocks(2);
	a.s.mirror->voice[5] = EchoVoice(s, 2, 0.0f, 22050.0f);
	a.publish();
	a.block();
	CHECK(Peak(a.bus[0], 1024, 1024 + 64) > 0.02f);
	CHECK(Peak(a.bus[0], 1024 + 64, 1536) == 0.0f);

	EchoScene c, ref;
	c.s.mirror->voice[5] = TwoD(s, 1);
	c.s.mirror->voice[5].routeBypass = 0;
	c.s.mirror->voice[5].monoGain = 0.6f;
	c.publish();
	c.blocks(2);
	c.s.mirror->voice[5] = EchoVoice(s, 2, 0.5f, 11025.0f);
	c.publish();
	c.block();
	ref.s.mirror->voice[5] = EchoVoice(s, 2, 0.5f, 11025.0f);
	ref.publish();
	ref.block();
	CHECK(Peak(c.bus[0], 1024, 1536) > 0.05f);
	CHECK(WorstIn(c.bus[0], 1024, ref.bus[0], 0, 512) == 0.0);

	EchoScene f;
	f.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	f.publish();
	f.blocks(2);
	f.s.mirror->voice[5].routeBypass = 1;
	memset(f.s.mirror->voice[5].echoGain, 0, sizeof(f.s.mirror->voice[5].echoGain));
	f.publish();
	f.block();
	CHECK(Peak(f.bus[0], 1024, 1024 + 64) > 0.02f);
	CHECK(Peak(f.bus[0], 1024 + 64, 1536) == 0.0f);

	EchoScene g;
	g.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	g.publish();
	g.blocks(2);
	g.s.mirror->voice[5] = EchoVoice(s, 2, 0.25f, 11025.0f);
	g.publish();
	g.block();
	EchoScene r2;
	r2.s.mirror->voice[5] = EchoVoice(s, 2, 0.25f, 11025.0f);
	r2.publish();
	r2.block();
	double fadeOnly = 0.0, fadeAtOldGains = 0.0;
	for(size_t i = 64; i < 512; i++)
		fadeOnly = fmax(fadeOnly, fabs(g.bus[0][1024 + i] - r2.bus[0][i]));
	for(size_t i = 0; i < 64; i++)
		fadeAtOldGains = fmax(fadeAtOldGains, fabs((g.bus[0][1024 + i] - r2.bus[0][i]) - a.bus[0][1024 + i]));
	CHECK(fadeOnly == 0.0);
	CHECK(Peak(a.bus[0], 1024, 1024 + 64) > 0.02f && fadeAtOldGains < 1e-6);

	EchoScene stop;
	stop.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	stop.publish();
	stop.blocks(2);
	stop.s.mirror->voice[5].playing = false;
	stop.publish();
	stop.block();
	CHECK(Peak(stop.bus[0], 1024, 1024 + 64) > 0.02f);
	CHECK(WorstIn(a.bus[0], 1024, stop.bus[0], 1024, 512) <= 1e-7);
}

static void
TestEchoRampResetOnLeavingMono(void)
{
	std::vector<int16_t> s = Sine(22050);
	EchoScene e;
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	e.publish();
	e.blocks(2);
	e.s.mirror->voice[5].routeBypass = 1;
	memset(e.s.mirror->voice[5].echoGain, 0, sizeof(e.s.mirror->voice[5].echoGain));
	e.publish();
	e.blocks(3);
	size_t back = e.bus[0].size();
	e.s.mirror->voice[5] = EchoVoice(s, 1, 0.25f, 22050.0f);
	e.publish();
	e.block();
	float first = Peak(e.bus[0], back, back + 8);
	float later = Peak(e.bus[0], back + 128, back + 512);
	CHECK(later > 0.05f);
	CHECK(first < 0.02f);

	EchoScene a, ref;
	a.s.mirror->voice[5] = EchoVoice(s, 1, 0.5f, 22050.0f);
	ref.s.mirror->voice[5] = EchoVoice(s, 1, 0.25f, 22050.0f);
	a.publish();
	ref.publish();
	a.blocks(2);
	ref.blocks(2);
	a.s.mirror->mode = ref.s.mirror->mode = MODE_CLASSIC;
	a.publish();
	ref.publish();
	a.blocks(8);
	ref.blocks(8);
	a.s.mirror->voice[5].echoGain[0] = 0.25f;
	a.s.mirror->mode = ref.s.mirror->mode = MODE_SPATIAL;
	a.publish();
	ref.publish();
	size_t from = a.bus[0].size();
	a.blocks(6);
	ref.blocks(6);
	CHECK(Peak(ref.bus[0], from, from + 3072) > 0.0f && WorstIn(a.bus[0], from, ref.bus[0], from, 3072) == 0.0);
}

int
main(void)
{
	InitResampler();
	TestSilence();
	TestPlayingVoice();
	TestSpatialRouting();
	TestSpatialClassicMode();
	TestJumpElapsedFromPublish();
	TestSpatialDirection();
	TestSpatialChunkedDirections();
	TestReusedChannelPositionedThen2D();
	TestReusedChannel2DThenPositioned();
	TestRouteFlipOnPlayingVoice();
	TestRouteFlipWithStop();
	TestStopRampsHeldValue();
	TestRestartFadesOldVoice();
	TestOneShotEnds();
	TestEmptyVoiceFinishes();
	TestBlockSizes();
	TestSendShelf();
	TestSendShelfPerVoice();
	TestShelfAfterRateChange();
	TestSendFiniteAt8k();
	TestNonFiniteSendDropped();
	TestRendersCounted();
	TestStats();
	TestLogPrefix();
	TestModeSwitchDip();
	TestModeSwitchTurnsBack();
	TestModeSwitchMidBlock();
	TestModeSwitchDropsTails();
	TestModeSwitchWhenSilent();
	TestSendRuleFollowsRouting();
	TestSpatialSendSkipsFlatShelf();
	TestMixerRouteSpan();
	TestEnvBankObservesOncePerSerial();
	TestEnvBankFirstObserveAfterInit();
	TestEnvBankSendTimesWeightTimesGain();
	TestEnvBankGainRamp();
	TestEnvBankGainRampBlockSizes();
	TestEnvBankShelf();
	TestEnvBankShelfFollowsGainChange();
	TestEnvBankShelfRingsOutAfterInputStops();
	TestEnvBankFlickerChangesNothing();
	TestEnvBankBlockSizes();
	TestEnvBankRouting();
	TestEnvBankChangeDuringFadeHasNoStep();
	TestEnvBankSettle();
	TestEchoLineDelay();
	TestEchoLineStaleGuard();
	TestEchoLineHeadNeverSteps();
	TestEchoBusSumsAndClears();
	TestEchoGainRamp();
	TestEchoCarrierFadeReachesTaps();
	TestEchoFadeDroppedInClassicBlock();
	TestEchoRampResetOnLeavingMono();
	if(failures)
		return 1;
	printf("all mixer tests passed\n");
	return 0;
}
