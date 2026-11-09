// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -I src tests/audio_voice_test.cpp src/CaResampler.cpp src/CaVoice.cpp -o build/tests/audio_voice_test && build/tests/audio_voice_test
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "CaVoice.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static std::vector<int16_t>
Sine(float hz, float rate, int n, float amp)
{
	std::vector<int16_t> s(n);
	for(int i = 0; i < n; i++)
		s[i] = (int16_t)lrintf(amp * 32767.0f * sinf(2.0f * 3.14159265f * hz * i / rate));
	return s;
}

static VoiceSnap
Snap(const std::vector<int16_t> &s, float freq, bool loop, uint32_t ls, uint32_t le)
{
	VoiceSnap v = {};
	v.data = s.data();
	v.length = (uint32_t)s.size();
	v.loopStart = ls;
	v.loopEnd = le ? le : (uint32_t)s.size();
	v.playId = 1;
	v.freq = freq;
	v.gainL = 1.0f;
	v.gainR = 1.0f;
	v.send = 0.0f;
	v.loop = loop;
	v.playing = 1;
	v.poolSlot = -1;
	return v;
}

static std::vector<float>
Render(VoiceState &st, const VoiceSnap &sn, float outRate, int frames, bool *ended)
{
	std::vector<float> l(frames), r(frames), s(frames);
	*ended = false;
	for(int done = 0; done < frames; done += 512){
		int n = frames - done < 512 ? frames - done : 512;
		if(RenderVoice(st, sn, outRate, &l[done], &r[done], &s[done], n))
			*ended = true;
	}
	return l;
}

static double
QStep(float freq, float outRate)
{
	return (double)(uint32_t)(freq / outRate * 65536.0f) / 65536.0;
}

static double
ToneLevel(float hz, float srcRate, float outRate)
{
	std::vector<int16_t> s(88200);
	for(int i = 0; i < 88200; i++)
		s[i] = (int16_t)lrint(0.5 * 32767.0 * sin(2.0 * M_PI * hz * (double)i / srcRate));
	VoiceSnap sn = Snap(s, srcRate, false, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	int frames = (int)(88200.0 * outRate / srcRate) - 256;
	bool ended;
	std::vector<float> out = Render(st, sn, outRate, frames, &ended);
	double e = 0.0;
	for(int i = 256; i < frames; i++)
		e += out[i] * out[i];
	return 10.0 * log10(e / (frames - 256) / 0.125);
}

static void
TestDc(void)
{
	std::vector<int16_t> dc(4000, 16384);
	VoiceSnap sn = Snap(dc, 22050.0f, true, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	bool ended;
	std::vector<float> out = Render(st, sn, 48000.0f, 4096, &ended);
	float worst = 0.0f;
	for(int i = 40; i < 4096; i++)
		worst = fmaxf(worst, fabsf(out[i] - 0.5f));
	CHECK(worst < 1e-4f);
	CHECK(!ended);
}

static void
TestSine(void)
{
	std::vector<int16_t> s = Sine(1000.0f, 22050.0f, 44100, 0.5f);
	VoiceSnap sn = Snap(s, 22050.0f, false, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	bool ended;
	std::vector<float> out = Render(st, sn, 48000.0f, 48000, &ended);
	double sig = 0.0, err = 0.0;
	for(int i = 64; i < 48000; i++){
		double ideal = 0.5 * 32767.0 / 32768.0 * sin(2.0 * M_PI * 1000.0 * i * QStep(22050.0f, 48000.0f) / 22050.0);
		sig += ideal * ideal;
		err += (out[i] - ideal) * (out[i] - ideal);
	}
	double snr = 10.0 * log10(sig / err);
	printf("1 kHz SNR %.1f dB\n", snr);
	CHECK(snr > 60.0);

	std::vector<int16_t> h = Sine(4000.0f, 22050.0f, 44100, 0.5f);
	VoiceSnap hn = Snap(h, 22050.0f, false, 0, 0);
	StartVoice(st, hn);
	out = Render(st, hn, 48000.0f, 48000, &ended);
	double e = 0.0;
	for(int i = 64; i < 48000; i++)
		e += out[i] * out[i];
	double db = 10.0 * log10(e / (48000 - 64) / (0.125 * 32767.0 / 32768.0 * 32767.0 / 32768.0));
	printf("4 kHz level %.3f dB\n", db);
	CHECK(fabs(db) < 0.5);
}

static void
TestOneShotEnd(void)
{
	std::vector<int16_t> s(22050, 1000);
	VoiceSnap sn = Snap(s, 22050.0f, false, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(512), r(512), x(512);
	int blocks = 0;
	bool ended = false;
	while(!ended && blocks < 200){
		ended = RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
		blocks++;
	}
	CHECK(ended);
	CHECK(blocks == 94);
	CHECK(!st.active);
	CHECK(!RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512));
}

static void
TestLoopSeam(void)
{
	std::vector<int16_t> s = Sine(441.0f, 22050.0f, 2000, 0.5f);
	static const uint32_t shifts[] = { 0, 12 };
	for(int k = 0; k < 2; k++){
		uint32_t ls = 500 + shifts[k], le = 1500 + shifts[k];
		VoiceSnap sn = Snap(s, 22050.0f, true, ls, le);
		VoiceState st = {};
		StartVoice(st, sn);
		bool ended;
		std::vector<float> out = Render(st, sn, 22050.0f, 6000, &ended);
		float worst = 0.0f;
		for(int i = 16; i < 6000; i++){
			double srcPos = i < (int)le ? i : ls + (i - ls) % 1000;
			double ideal = 0.5 * 32767.0 / 32768.0 * sin(2.0 * M_PI * 441.0 * srcPos / 22050.0);
			worst = fmaxf(worst, (float)fabs(out[i] - ideal));
		}
		CHECK(worst < 2e-3f);
		CHECK(!ended && st.active);
	}
}

static void
TestLoopWrapDc(void)
{
	std::vector<int16_t> dc(3000, 16384);
	static const float rates[] = { 48000.0f, 22050.0f, 8820.0f };
	for(int k = 0; k < 3; k++){
		VoiceSnap sn = Snap(dc, 22050.0f, true, 0, 0);
		VoiceState st = {};
		StartVoice(st, sn);
		bool ended;
		std::vector<float> out = Render(st, sn, rates[k], 30000, &ended);
		float worst = 0.0f;
		for(int i = 64; i < 30000; i++)
			worst = fmaxf(worst, fabsf(out[i] - 0.5f));
		CHECK(worst < 1e-4f);
	}
}

static void
TestShortLoop(void)
{
	std::vector<int16_t> dc(3, 16384);
	VoiceSnap sn = Snap(dc, 480000.0f, true, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	bool ended;
	std::vector<float> out = Render(st, sn, 48000.0f, 1024, &ended);
	CHECK(st.pos >= 0.0 && st.pos < 3.0);
	CHECK(fabsf(out[700] - 0.5f) < 1e-4f);
}

static void
TestShortLoopPeriod(void)
{
	std::vector<int16_t> s = { 1000, -3000, 5000, 2000, -7000 };
	static const float freqs[] = { 48000.0f, 24000.0f, 60000.0f };
	static const int periods[] = { 5, 10, 4 };
	for(int k = 0; k < 3; k++){
		VoiceSnap sn = Snap(s, freqs[k], true, 0, 0);
		VoiceState st = {};
		StartVoice(st, sn);
		bool ended;
		std::vector<float> out = Render(st, sn, 48000.0f, 2100, &ended);
		bool same = st.wrapped;
		for(int n = 100; n < 2000; n++)
			same = same && out[n] == out[n + periods[k]];
		CHECK(same);
		double step = freqs[k] / 48000.0;
		float worst = 0.0f;
		for(int n = 100; n < 120; n++){
			double pos = fmod(n * step, 5.0);
			int64_t i = (int64_t)pos;
			float w[kSincMaxTaps];
			int first;
			int taps = SincWeights(pos - (double)i, step, w, &first);
			float ideal = 0.0f;
			for(int j = 0; j < taps; j++){
				int64_t m = ((i + first + j) % 5 + 5) % 5;
				ideal += s[m] * (1.0f / 32768.0f) * w[j];
			}
			worst = fmaxf(worst, fabsf(out[n] - ideal));
		}
		CHECK(worst < 1e-6f);
	}
}

static void
TestLoopFieldsChange(void)
{
	std::vector<int16_t> dc(300, 16384);
	static const uint32_t starts[] = { 100, 200, 0 };
	static const uint32_t ends[] = { 100, 50, 1000 };
	bool ok = true;
	for(int k = 0; k < 3; k++){
		VoiceSnap sn = Snap(dc, 22050.0f, true, 0, 0);
		VoiceState st = {};
		StartVoice(st, sn);
		bool ended;
		Render(st, sn, 48000.0f, 512, &ended);
		sn.loopStart = starts[k];
		sn.loopEnd = ends[k];
		std::vector<float> out = Render(st, sn, 48000.0f, 4096, &ended);
		for(int n = 0; n < 4096; n++)
			ok = ok && fabsf(out[n] - 0.5f) < 1e-4f;
		ok = ok && st.active && st.wrapped;

		VoiceState fresh = {};
		StartVoice(fresh, sn);
		out = Render(fresh, sn, 48000.0f, 4096, &ended);
		for(int n = 64; n < 4096; n++)
			ok = ok && fabsf(out[n] - 0.5f) < 1e-4f;
		ok = ok && fresh.active;
	}
	CHECK(ok);
}

static void
TestLoopTaps(void)
{
	std::vector<int16_t> s(2400);
	for(int i = 0; i < 2400; i++)
		s[i] = i < 512 ? 9830 : i >= 1512 ? -13107 : (int16_t)lrint(0.5 * 32767.0 * sin(2.0 * M_PI * 441.0 * i / 22050.0));
	static const float rates[] = { 22050.0f, 8820.0f };
	for(int k = 0; k < 2; k++){
		double step = 22050.0 / rates[k];
		VoiceSnap sn = Snap(s, 22050.0f, true, 512, 1512);
		VoiceState st = {};
		StartVoice(st, sn);
		bool ended;
		std::vector<float> out = Render(st, sn, rates[k], 6000, &ended);
		float worst = 0.0f;
		for(int i = 0; i < 6000; i++){
			double pos = i * step;
			if(pos < 512.0 + 48.0)
				continue;
			double ideal = 0.5 * sin(2.0 * M_PI * 441.0 * pos / 22050.0);
			worst = fmaxf(worst, (float)fabs(out[i] - ideal));
		}
		CHECK(worst < 2e-3f);
	}
}

static void
TestGainRamp(void)
{
	CHECK(kOalFadeFrames == 64);
	std::vector<int16_t> dc(8000, 32767);
	VoiceSnap sn = Snap(dc, 48000.0f, true, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(256), r(256), x(256);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 256);
	sn.gainL = 0.0f;
	sn.send = 0.5f;
	for(int i = 0; i < 256; i++)
		l[i] = r[i] = x[i] = 0.0f;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 256);
	float v = 32767.0f / 32768.0f;
	float worst = 0.0f;
	for(int k = 0; k < 64; k++){
		worst = fmaxf(worst, fabsf(l[k] - v * (1.0f - k / 64.0f)));
		worst = fmaxf(worst, fabsf(x[k] - 0.5f * v * (k / 64.0f)));
	}
	CHECK(worst < 1e-6f);
	CHECK(l[64] == 0.0f && l[255] == 0.0f);
	CHECK(fabsf(r[255] - v) < 1e-6f);
	CHECK(fabsf(x[64] - 0.5f * v) < 1e-6f && fabsf(x[255] - 0.5f * v) < 1e-6f);
}

static void
TestPitchStepIsImmediateAndContinuous(void)
{
	std::vector<int16_t> s = Sine(441.0f, 22050.0f, 22050, 0.5f);
	VoiceSnap sn = Snap(s, 22050.0f, true, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(2000), r(2000), x(2000);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 1000);
	double before = st.pos;
	sn.freq = 33075.0f;
	RenderVoice(st, sn, 48000.0f, &l[1000], &r[1000], &x[1000], 1000);
	CHECK(st.pos - before == 1000.0 * QStep(33075.0f, 48000.0f));
	float slope = 0.5f * 2.0f * 3.14159265f * 441.0f * 1.5f / 48000.0f;
	float worst = 0.0f;
	for(int i = 900; i < 1100; i++)
		worst = fmaxf(worst, fabsf(l[i + 1] - l[i]));
	CHECK(worst <= slope * 1.02f);
}

static void
TestNegativeGain(void)
{
	std::vector<int16_t> dc(8000, 16384);
	VoiceSnap sn = Snap(dc, 48000.0f, true, 0, 0);
	sn.gainL = 0.8f;
	sn.gainR = -0.25f;
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(512), r(512), x(512);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	CHECK(fabsf(l[300] - 0.4f) < 1e-3f && fabsf(r[300] + 0.125f) < 1e-3f);
	sn.gainR = -0.5f;
	for(int i = 0; i < 512; i++)
		l[i] = r[i] = x[i] = 0.0f;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	CHECK(r[32] < -0.125f && r[32] > -0.25f);
	CHECK(fabsf(r[64] + 0.25f) < 1e-3f);
}

static void
TestStop(void)
{
	std::vector<int16_t> dc(8000, 32767), poison(8000, -32768);
	VoiceSnap sn = Snap(dc, 48000.0f, true, 0, 0);
	sn.gainR = 0.5f;
	sn.send = 0.25f;
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(512), r(512), x(512);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	sn.playing = 0;
	for(int i = 0; i < 512; i++)
		l[i] = r[i] = x[i] = 0.0f;
	CHECK(!RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 16));
	CHECK(!st.active);
	sn.data = poison.data();
	CHECK(!RenderVoice(st, sn, 48000.0f, &l[16], &r[16], &x[16], 496));
	float v = 32767.0f / 32768.0f, worst = 0.0f;
	for(int k = 0; k < 64; k++){
		float ramp = 1.0f - k / 64.0f;
		worst = fmaxf(worst, fabsf(l[k] - v * ramp));
		worst = fmaxf(worst, fabsf(r[k] - 0.5f * v * ramp));
		worst = fmaxf(worst, fabsf(x[k] - 0.25f * v * ramp));
	}
	CHECK(worst < 1e-6f);
	bool silent = true;
	for(int i = 64; i < 512; i++)
		silent = silent && l[i] == 0.0f && r[i] == 0.0f && x[i] == 0.0f;
	CHECK(silent);
	l[0] = 0.0f;
	CHECK(!RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512));
	CHECK(l[0] == 0.0f);
}

static void
TestMonoRender(void)
{
	std::vector<int16_t> s = Sine(440.0f, 22050.0f, 6000, 0.5f);
	VoiceSnap sn = Snap(s, 22050.0f, true, 0, 0);
	sn.gainL = 0.7f;
	sn.gainR = 0.3f;
	sn.send = 0.2f;
	VoiceState a = {}, b = {};
	StartVoice(a, sn);
	StartVoice(b, sn);
	std::vector<float> l(2048), r(2048), x(2048), m(2048), mx(2048);
	RenderVoice(a, sn, 48000.0f, l.data(), r.data(), x.data(), 700);
	RenderVoice(b, sn, 48000.0f, m.data(), NULL, mx.data(), 700);
	sn.gainL = 0.4f;
	RenderVoice(a, sn, 48000.0f, &l[700], &r[700], &x[700], 600);
	RenderVoice(b, sn, 48000.0f, &m[700], NULL, &mx[700], 600);
	sn.playing = 0;
	RenderVoice(a, sn, 48000.0f, &l[1300], &r[1300], &x[1300], 748);
	RenderVoice(b, sn, 48000.0f, &m[1300], NULL, &mx[1300], 748);
	bool same = true, audible = false;
	for(int i = 0; i < 2048; i++){
		same = same && m[i] == l[i] && mx[i] == x[i];
		audible = audible || fabsf(m[i]) > 0.1f;
	}
	CHECK(same && audible);
	CHECK(m[1300] != 0.0f && m[1300 + 64] == 0.0f);
}

static double
StopError(std::vector<int16_t> &s, bool loop, uint32_t loopEnd, uint32_t frames, uint32_t heldAt)
{
	VoiceSnap sn = Snap(s, 48000.0f, loop, 0, loopEnd);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(frames + 128), r(frames + 128), x(frames + 128);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), frames);
	sn.playing = 0;
	RenderVoice(st, sn, 48000.0f, &l[frames], &r[frames], &x[frames], 128);
	uint32_t n = (uint32_t)s.size();
	std::vector<int16_t> lin(frames + 256);
	for(uint32_t j = 0; j < lin.size(); j++){
		uint32_t m = j < heldAt ? j : heldAt;
		lin[j] = loop ? s[m % loopEnd] : s[m < n ? m : n - 1];
	}
	VoiceSnap ln = Snap(lin, 48000.0f, false, 0, 0);
	VoiceState ref = {};
	StartVoice(ref, ln);
	std::vector<float> rl(frames + 128), rr(frames + 128), rx(frames + 128);
	RenderVoice(ref, ln, 48000.0f, rl.data(), rr.data(), rx.data(), frames + 128);
	double worst = 0.0;
	for(uint32_t k = 0; k < 128; k++)
		worst = fmax(worst, fabs(l[frames + k] - (k < 64 ? rl[frames + k] * (1.0f - k / 64.0f) : 0.0f)) * 32768.0);
	return worst;
}

static void
TestStopHoldsQuietestNextSample(void)
{
	std::vector<int16_t> s(200, 10000);
	s[15] = -300;
	s[30] = 200;
	s[31] = -200;
	s[34] = 0;
	CHECK(StopError(s, false, 0, 10, 30) < 0.5);
	std::vector<int16_t> e(200, 10000);
	e[197] = 5000;
	e[198] = 4000;
	e[199] = 3000;
	CHECK(StopError(e, false, 0, 197, 199) < 0.5);
	std::vector<int16_t> w(200, 10000);
	w[2] = 100;
	w[55] = 50;
	CHECK(StopError(w, true, 50, 40, 52) < 0.5);
}

static std::vector<float>
StopThenStart(const VoiceSnap &a, const VoiceSnap *b, uint32_t frames)
{
	VoiceState st = {};
	StartVoice(st, a);
	std::vector<float> l(frames + 128), r(frames + 128), x(frames + 128);
	RenderVoice(st, a, 48000.0f, l.data(), r.data(), x.data(), frames);
	std::fill(l.begin(), l.end(), 0.0f);
	StopVoice(st, a);
	if(b){
		StartVoice(st, *b);
		RenderVoice(st, *b, 48000.0f, l.data(), r.data(), x.data(), 128);
	}else
		RenderVoice(st, a, 48000.0f, l.data(), r.data(), x.data(), 128);
	l.resize(128);
	return l;
}

static void
TestOverlappingStops(void)
{
	std::vector<int16_t> dc(4000, 10000), shortOne(20, -5000);
	VoiceSnap a = Snap(dc, 48000.0f, true, 0, 0), b = Snap(shortOne, 48000.0f, false, 0, 0);
	b.playId = 2;
	std::vector<float> both = StopThenStart(a, &b, 300), oldOnly = StopThenStart(a, NULL, 300);
	VoiceState fresh = {};
	StartVoice(fresh, b);
	std::vector<float> l(128, 0.0f), r(128), x(128);
	RenderVoice(fresh, b, 48000.0f, l.data(), r.data(), x.data(), 128);
	float worst = 0.0f;
	for(int k = 0; k < 128; k++)
		worst = fmaxf(worst, fabsf(both[k] - oldOnly[k] - l[k]));
	CHECK(worst < 1e-6f);
	CHECK(fabsf(oldOnly[40]) > 0.1f && fabsf(l[40]) > 0.05f);
}

static void
TestStepQuantizedLikeOpenAL(void)
{
	std::vector<int16_t> s = Sine(441.0f, 26513.0f, 30000, 0.5f);
	VoiceSnap sn = Snap(s, 26513.0f, true, 0, 0);
	sn.baseRate = 26513;
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(4096), r(4096), x(4096);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 4096);
	float pitch = 26513.0f / 26513.0f * (26513.0f / 48000.0f);
	double step = (double)(uint32_t)(pitch * 65536.0f) / 65536.0;
	CHECK(step != 26513.0 / 48000.0);
	CHECK(st.pos == 4096.0 * step);
	sn.freq = 30011.0f;
	double before = st.pos;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 1000);
	pitch = 30011.0f / 26513.0f * (26513.0f / 48000.0f);
	CHECK(st.pos - before == 1000.0 * ((double)(uint32_t)(pitch * 65536.0f) / 65536.0));
	sn.freq = 26513.0f * 40.0f;
	before = st.pos;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 10);
	CHECK(st.pos - before == 100.0);
}

static void
TestFrozenAndUnclamped(void)
{
	std::vector<int16_t> s(40000, 2000);
	VoiceSnap sn = Snap(s, 0.0f, false, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(512), r(512), x(512);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	CHECK(st.pos == 512.0 / 65536.0 && l[100] > 0.0f && st.active);
	sn.freq = 48000.0f * 10.0f;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	CHECK(fabs(st.pos - 512.0 / 65536.0 - 10.0 * 512.0) < 1e-6);
	double before = st.pos;
	for(int i = 0; i < 512; i++)
		l[i] = 0.0f;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512);
	CHECK(fabs(st.pos - before - 10.0 * 512.0) < 1e-6);
	CHECK(fabsf(l[300] - 2000.0f / 32768.0f) < 1e-4f);
}

static void
TestFrequencyZeroHolds(void)
{
	std::vector<int16_t> s = Sine(441.0f, 22050.0f, 22050, 0.5f);
	VoiceSnap sn = Snap(s, 22050.0f, true, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(3000), r(3000), x(3000);
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 1000);
	double before = st.pos;
	sn.freq = 0.0f;
	RenderVoice(st, sn, 48000.0f, &l[1000], &r[1000], &x[1000], 1000);
	CHECK(st.active && st.pos - before == 1000.0 / 65536.0);
	float slope = 0.5f * 2.0f * 3.14159265f * 441.0f / 48000.0f, worst = 0.0f, drift = 0.0f;
	for(int i = 900; i < 1100; i++)
		worst = fmaxf(worst, fabsf(l[i + 1] - l[i]));
	for(int i = 1001; i < 2000; i++)
		drift = fmaxf(drift, fabsf(l[i] - l[1000]));
	CHECK(worst <= slope * 1.02f);
	CHECK(fabsf(l[1999]) > 0.3f && drift < 2e-3f);
	before = st.pos;
	sn.freq = 22050.0f;
	RenderVoice(st, sn, 48000.0f, &l[2000], &r[2000], &x[2000], 1000);
	CHECK(st.pos - before == 1000.0 * QStep(22050.0f, 48000.0f));
	worst = 0.0f;
	for(int i = 1900; i < 2100; i++)
		worst = fmaxf(worst, fabsf(l[i + 1] - l[i]));
	CHECK(worst <= slope * 1.02f);
	before = st.pos;
	sn.freq = NAN;
	RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 100);
	CHECK(st.active && st.pos == before);
}

static void
TestAliasing(void)
{
	static const double steps[] = { 2.0, 4.0, 8.0 };
	static const double mults[] = { 1.15, 1.3, 1.5, 2.0, 2.5, 3.0, 3.5, 4.5, 6.0, 7.5 };
	for(int k = 0; k < 3; k++){
		double outNyq = 22050.0 / steps[k];
		double worst = -200.0;
		for(int m = 0; m < 10; m++){
			double hz = outNyq * mults[m];
			if(hz > 0.95 * 22050.0)
				continue;
			worst = fmax(worst, ToneLevel((float)hz, 44100.0f, (float)(44100.0 / steps[k])));
		}
		printf("step %.0f worst alias %.1f dB\n", steps[k], worst);
		CHECK(worst < (steps[k] <= 4.0 ? -60.0 : -30.0));
	}
}

static void
TestStepContinuity(void)
{
	double below = ToneLevel(8000.0f, 22050.0f, 22050.0f / 0.99f);
	double above = ToneLevel(8000.0f, 22050.0f, 22050.0f / 1.01f);
	CHECK(fabs(below - above) < 0.05);
}

static void
TestPhaseEdge(void)
{
	std::vector<int16_t> dc(4000, 16384);
	static const float rates[] = { 48000.0f, 22050.0f, 9586.9565f, 5512.5f };
	static const double eps[] = { 1e-7, 1e-9, 1e-12 };
	bool ok = true;
	for(int k = 0; k < 4; k++)
		for(int e = 0; e < 3; e++){
			VoiceSnap sn = Snap(dc, 22050.0f, true, 0, 0);
			VoiceState st = {};
			StartVoice(st, sn);
			float l = 0.0f, r = 0.0f, x = 0.0f;
			RenderVoice(st, sn, rates[k], &l, &r, &x, 1);
			st.pos = 2000.0 - eps[e];
			l = 0.0f;
			RenderVoice(st, sn, rates[k], &l, &r, &x, 1);
			ok = ok && std::isfinite(l) && fabsf(l - 0.5f) < 1e-4f;
		}
	CHECK(ok);
}

static void
TestBlockSizes(void)
{
	std::vector<int16_t> s = Sine(441.0f, 22050.0f, 3000, 0.5f);
	VoiceSnap sn = Snap(s, 22050.0f, true, 700, 2900);
	VoiceState a = {}, b = {};
	StartVoice(a, sn);
	StartVoice(b, sn);
	std::vector<float> l1(8192), r1(8192), x1(8192), l2(8192), r2(8192), x2(8192);
	static const uint32_t sizes[] = { 1, 7, 64, 333, 512, 1000 };
	uint32_t done = 0;
	for(int phase = 0; phase < 2; phase++){
		uint32_t end = phase == 0 ? 3000 : 8192;
		RenderVoice(a, sn, 48000.0f, &l1[done], &r1[done], &x1[done], end - done);
		for(int k = 0; done < end; k = (k + 1) % 6){
			uint32_t n = end - done < sizes[k] ? end - done : sizes[k];
			RenderVoice(b, sn, 48000.0f, &l2[done], &r2[done], &x2[done], n);
			done += n;
		}
		sn.freq = 30000.0f;
		sn.gainL = 0.25f;
		sn.send = 0.7f;
	}
	bool same = a.pos == b.pos;
	for(int i = 0; i < 8192; i++)
		same = same && l1[i] == l2[i] && x1[i] == x2[i];
	CHECK(same);
}

static void
TestEndFrame(void)
{
	std::vector<int16_t> s(1000, 1000);
	VoiceSnap sn = Snap(s, 48000.0f, false, 0, 0);
	VoiceState st = {};
	StartVoice(st, sn);
	std::vector<float> l(512), r(512), x(512);
	CHECK(!RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512));
	CHECK(st.active);
	for(int i = 0; i < 512; i++)
		l[i] = 0.0f;
	CHECK(RenderVoice(st, sn, 48000.0f, l.data(), r.data(), x.data(), 512));
	CHECK(!st.active);
	float v = 1000.0f / 32768.0f;
	CHECK(fabsf(l[487] - v) < 1e-6f);
	std::vector<float> l2(512, 0.0f);
	CHECK(!RenderVoice(st, sn, 48000.0f, l2.data(), r.data(), x.data(), 512));
	float worst = 0.0f;
	for(int k = 0; k < 64; k++)
		worst = fmaxf(worst, fabsf((k < 24 ? l[488 + k] : l2[k - 24]) - v * (1.0f - k / 64.0f)));
	CHECK(worst < 1e-6f);
	bool silent = true;
	for(int i = 40; i < 512; i++)
		silent = silent && l2[i] == 0.0f;
	CHECK(silent);

	std::vector<int16_t> t(1024, 1000);
	VoiceSnap tn = Snap(t, 48000.0f, false, 0, 0);
	StartVoice(st, tn);
	CHECK(!RenderVoice(st, tn, 48000.0f, l.data(), r.data(), x.data(), 512));
	CHECK(RenderVoice(st, tn, 48000.0f, l.data(), r.data(), x.data(), 512));
	CHECK(!st.active);
}

int
main(void)
{
	InitResampler();
	TestMonoRender();
	TestDc();
	TestSine();
	TestOneShotEnd();
	TestLoopSeam();
	TestLoopWrapDc();
	TestLoopTaps();
	TestShortLoop();
	TestShortLoopPeriod();
	TestLoopFieldsChange();
	TestGainRamp();
	TestNegativeGain();
	TestPitchStepIsImmediateAndContinuous();
	TestStop();
	TestStopHoldsQuietestNextSample();
	TestOverlappingStops();
	TestStepQuantizedLikeOpenAL();
	TestFrozenAndUnclamped();
	TestFrequencyZeroHolds();
	TestAliasing();
	TestStepContinuity();
	TestPhaseEdge();
	TestBlockSizes();
	TestEndFrame();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all voice tests passed\n");
	return 0;
}
