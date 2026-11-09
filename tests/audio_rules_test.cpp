// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_rules_test.cpp -o build/tests/audio_rules_test && build/tests/audio_rules_test
#include <cstdio>
#include <cmath>
#include <cfloat>
#include "CaRules.h"
#include "CaFilter.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void
TestAttenuation(void)
{
	CHECK(OalInverseClampedGain(0.0f, 10.0f, 40.0f) == 1.0f);
	CHECK(OalInverseClampedGain(10.0f, 10.0f, 40.0f) == 1.0f);
	CHECK(Near(OalInverseClampedGain(20.0f, 10.0f, 40.0f), 0.5f, 1e-6f));
	CHECK(Near(OalInverseClampedGain(40.0f, 10.0f, 40.0f), 0.25f, 1e-6f));
	CHECK(Near(OalInverseClampedGain(400.0f, 10.0f, 40.0f), 0.25f, 1e-6f));
	CHECK(OalInverseClampedGain(5.0f, 0.0f, 0.0f) == 1.0f);
	CHECK(Near(OalInverseClampedGain(30.0f, 10.0f, 5.0f), 1.0f, 1e-6f));
	CHECK(Near(OalInverseClampedGain(1000.0f, 25000.0f, 100000.0f), 1.0f, 1e-6f));
}

static void
TestVolumes(void)
{
	CHECK(ScaledVolume(127, 127, 127) == 125);
	CHECK(ScaledVolume(127, 100, 49) == 37);
	CHECK(ScaledVolume(0, 127, 127) == 0);
	CHECK(Near(GainFromVolume(127), 1.0f, 1e-6f));
	CHECK(Near(GainFromVolume(37), 37.0f / 127.0f, 1e-6f));
	CHECK(StreamMusicVolume(100, 127, 127, 0.0f) == (127u * 100 * 127 >> 14));
	CHECK(StreamMusicVolume(100, 64, 127, 1.0f) == (127u * 100 * 128 >> 14));
}

static void
TestLoopRange(void)
{
	LoopRange r = OalLoopRange(0, -1, 1000);
	CHECK(r.start == 0 && r.end == 1000);
	r = OalLoopRange(0, 34662, 17332);
	CHECK(r.start == 0 && r.end == 17332);
	r = OalLoopRange(200, 1800, 1000);
	CHECK(r.start == 100 && r.end == 900);
	r = OalLoopRange(200, -1, 1000);
	CHECK(r.start == 0 && r.end == 1000);
	r = OalLoopRange(1800, 200, 1000);
	CHECK(r.start == 0 && r.end == 1000);
	r = OalLoopRange(200, 4000, 1000);
	CHECK(r.start == 0 && r.end == 1000);
	r = OalLoopRange(200, 2000, 1000);
	CHECK(r.start == 100 && r.end == 1000);
	r = OalLoopRange(1, 2000, 1000);
	CHECK(r.start == 0 && r.end == 1000);
}

static void
TestReverb(void)
{
	float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	CHECK(ReverbRatio(none, 100.0f, 100.0f, 100.0f, 100.0f) == 0.0f);
	float low[4] = { 5.0f, 5.0f, 5.0f, 5.0f };
	CHECK(Near(ReverbRatio(low, 100.0f, 100.0f, 100.0f, 100.0f), 0.6f, 1e-6f));
	float one[4] = { 5.0f, 0.0f, 0.0f, 0.0f };
	CHECK(Near(ReverbRatio(one, 100.0f, 100.0f, 100.0f, 100.0f), 0.25f, 1e-6f));
	CHECK(Near(ReverbRatio(none, 2.0f, 2.0f, 100.0f, 100.0f), 1.0f / 6.0f, 1e-6f));
	CHECK(ReverbRatio(none, 0.4f, 0.4f, 100.0f, 100.0f) == 0.0f);
	float edge[4] = { 10.0f, 0.5f, 0.0f, 0.0f };
	CHECK(ReverbRatio(edge, 100.0f, 100.0f, 100.0f, 100.0f) == 0.0f);
	CHECK(Near(EfxSendLevel(0.6f), 0.48f, 1e-6f));
	CHECK(EfxSendLevel(0.0f) == 0.0f);
}

static void
TestDefaultDistances(void)
{
	SourceDistances d = OalSourceDefaultDistances();
	CHECK(d.ref == 1.0f);
	CHECK(d.maxDist == FLT_MAX);
	CHECK(Near(OalInverseClampedGain(4.0f, d.ref, d.maxDist), 0.25f, 1e-7f));
	CHECK(Near(OalInverseClampedGain(1e6f, d.ref, d.maxDist), 1e-6f, 1e-12f));
}

static void
TestReverbLevelRule(void)
{
	float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float low[4] = { 5.0f, 5.0f, 5.0f, 5.0f };
	ReverbLevelState st = { 0.0f, 0.0f };
	CHECK(!UpdateReverbLevel(st, 16, none, 0.0f, 0.0f, 0.0f, 0.0f));
	CHECK(st.level == 0.0f);
	CHECK(!UpdateReverbLevel(st, 17, low, 100.0f, 100.0f, 100.0f, 100.0f));
	CHECK(!UpdateReverbLevel(st, 40, low, 100.0f, 100.0f, 100.0f, 100.0f));
	CHECK(st.level == 0.0f);
	CHECK(UpdateReverbLevel(st, 32, low, 100.0f, 100.0f, 100.0f, 100.0f));
	CHECK(st.level == EfxSendLevel(0.6f) && st.prevRatio == ReverbRatio(low, 100.0f, 100.0f, 100.0f, 100.0f));
	CHECK(!UpdateReverbLevel(st, 48, low, 100.0f, 100.0f, 100.0f, 100.0f));
	CHECK(UpdateReverbLevel(st, 64, none, 0.0f, 0.0f, 0.0f, 0.0f));
	CHECK(st.level == 0.0f);
}

static void
TestSendFilter(void)
{
	SendFilter f = OalSendFilter(0.48f);
	CHECK(f.gain == powf(10.0f, -637.0f / 2000.0f));
	CHECK(f.gainHF == f.gain);
	CHECK(OalSendFilter(1.0f).gain == 1.0f);
	CHECK(OalSendFilter(0.0f).gain == 0.0f && OalSendFilter(0.0f).gainHF == 0.0f);
	CHECK(OalSendFilter(1e-6f).gain == 0.0f);
}

static void
TestSendDistanceGain(void)
{
	CHECK(OalSendDistanceGain(5.0f, 10.0f, 40.0f) == 1.0f);
	CHECK(OalSendDistanceGain(10.0f, 10.0f, 40.0f) == 1.0f);
	CHECK(Near(OalSendDistanceGain(20.0f, 10.0f, 40.0f), 0.966595f, 1e-5f));
	CHECK(Near(OalSendDistanceGain(400.0f, 10.0f, 40.0f), 0.300570f, 1e-5f));
	CHECK(Near(OalSendDistanceGain(30.0f, 7.5f, 30.0f), 0.891939f, 1e-5f));
	CHECK(OalSendDistanceGain(50.0f, 0.0f, 100.0f) == 1.0f);
}

static float
ShelfGainAt(float hz, float rate, float gain)
{
	Biquad b = {};
	HighShelf(b, 5000.0f, rate, gain);
	static float x[4800];
	double in = 0.0, out = 0.0;
	for(int i = 0; i < 4800; i++)
		x[i] = sinf(2.0f * 3.14159265f * hz * i / rate);
	RunBiquad(b, x, 4800);
	for(int i = 2400; i < 4800; i++){
		double s = sin(2.0 * M_PI * hz * i / rate);
		in += s * s;
		out += (double)x[i] * x[i];
	}
	return (float)sqrt(out / in);
}

static void
TestShelf(void)
{
	CHECK(fabsf(ShelfGainAt(50.0f, 48000.0f, 0.25f) - 1.0f) < 0.01f);
	CHECK(fabsf(ShelfGainAt(5000.0f, 48000.0f, 0.25f) - 0.25f) < 0.01f);
	CHECK(fabsf(ShelfGainAt(20000.0f, 48000.0f, 0.25f) - 0.0625f) < 0.005f);
	CHECK(fabsf(ShelfGainAt(15000.0f, 48000.0f, 1.0f) - 1.0f) < 1e-3f);
}

static void
TestShelfRetuneKeepsState(void)
{
	Biquad b = {};
	HighShelf(b, 5000.0f, 48000.0f, 0.3f);
	float x[64];
	for(int i = 0; i < 64; i++)
		x[i] = sinf(0.7f * i);
	RunBiquad(b, x, 64);
	float z1 = b.z1, z2 = b.z2;
	HighShelf(b, 5000.0f, 48000.0f, 0.6f);
	CHECK(z1 != 0.0f && b.z1 == z1 && b.z2 == z2);
}

static void
TestShelfStateNeverSubnormal(void)
{
	Biquad b = {};
	HighShelf(b, 5000.0f, 48000.0f, 0.3f);
	float x[256];
	bool subnormal = false;
	for(int k = 0; k < 4000; k++){
		for(int i = 0; i < 256; i++)
			x[i] = k == 0 && i == 0 ? 1.0f : 0.0f;
		RunBiquad(b, x, 256);
		for(int i = 0; i < 256; i++)
			subnormal = subnormal || fpclassify(x[i]) == FP_SUBNORMAL;
		subnormal = subnormal || fpclassify(b.z1) == FP_SUBNORMAL || fpclassify(b.z2) == FP_SUBNORMAL;
	}
	CHECK(!subnormal);
	CHECK(b.z1 == 0.0f && b.z2 == 0.0f);
}

static void
TestShelfStableAtLowRates(void)
{
	const float rates[] = { 8000.0f, 11025.0f, 16000.0f, 44100.0f, 48000.0f };
	const float gains[] = { 0.0f, 0.25f, 0.5f, 1.0f };
	for(float rate : rates)
		for(float gain : gains){
			Biquad b = {};
			HighShelf(b, 5000.0f, rate, gain);
			bool stable = fabsf(b.a2) < 1.0f && fabsf(b.a1) < 1.0f + b.a2;
			if(!stable)
				printf("FAIL unstable shelf at %.0f Hz gain %.2f: a1 %g a2 %g\n", rate, gain, b.a1, b.a2);
			CHECK(stable);
			static float x[48000];
			for(int i = 0; i < 48000; i++)
				x[i] = i == 0 ? 1.0f : 0.0f;
			RunBiquad(b, x, 48000);
			float tail = 0.0f;
			bool finite = true;
			for(int i = 0; i < 48000; i++){
				finite = finite && std::isfinite(x[i]);
				if(i >= 47000)
					tail = fmaxf(tail, fabsf(x[i]));
			}
			if(!finite || tail > 1e-6f)
				printf("FAIL impulse does not decay at %.0f Hz gain %.2f: tail %g\n", rate, gain, tail);
			CHECK(finite && tail <= 1e-6f);
		}
}

static void
TestShelfAt48kUnchanged(void)
{
	const float gains[] = { 0.0f, 0.25f, 0.5f, 1.0f };
	const float want[4][5] = {
		{ 0x1.0f6f5ep-13f, 0x1.d7e4d2p-13f, 0x1.a1713cp-14f, -0x1.f83a7p+0f, 0x1.f0b062p-1f },
		{ 0x1.e877a6p-4f, -0x1.b30f7ap-5f, 0x1.942918p-6f, -0x1.87e502p+0f, 0x1.3e494ep-1f },
		{ 0x1.5ddfb8p-2f, -0x1.1a05d8p-2f, 0x1.944404p-4f, -0x1.595decp+0f, 0x1.07314ap-1f },
		{ 0x1p+0f, -0x1.1bf66ap+0f, 0x1.97b53ap-2f, -0x1.1bf66ap+0f, 0x1.97b53ap-2f },
	};
	for(int g = 0; g < 4; g++){
		Biquad b = {};
		HighShelf(b, 5000.0f, 48000.0f, gains[g]);
		CHECK(b.b0 == want[g][0] && b.b1 == want[g][1] && b.b2 == want[g][2] && b.a1 == want[g][3] && b.a2 == want[g][4]);
	}
}

int
main(void)
{
	TestAttenuation();
	TestVolumes();
	TestLoopRange();
	TestReverb();
	TestDefaultDistances();
	TestReverbLevelRule();
	TestSendFilter();
	TestSendDistanceGain();
	TestShelf();
	TestShelfRetuneKeepsState();
	TestShelfStateNeverSubnormal();
	TestShelfStableAtLowRates();
	TestShelfAt48kUnchanged();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all rules tests passed\n");
	return 0;
}
