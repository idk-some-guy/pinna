// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_env_test.cpp -o build/tests/audio_env_test && build/tests/audio_env_test
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include "CaEnvironment.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static const float kLowCeiling = 6.0f;

static int
Place(float n, float s, float w, float e, float c0, float c1, float c2, float c3, bool interior = false, bool room = false)
{
	PlaceInput p;
	p.north = n;
	p.south = s;
	p.west = w;
	p.east = e;
	p.ceil[0] = c0;
	p.ceil[1] = c1;
	p.ceil[2] = c2;
	p.ceil[3] = c3;
	p.enclosed = interior;
	p.roomed = room;
	p.lowCeiling = kLowCeiling;
	return ClassifyPlace(p);
}

static int
Place4(float n, float s, float w, float e, float c, bool interior = false, bool room = false)
{
	return Place(n, s, w, e, c, c, c, c, interior, room);
}

static void
TestClassify(void)
{
	CHECK(Place4(100, 100, 100, 100, 100) == ENV_OPEN);
	CHECK(Place4(100, 100, 5, 6, 100) == ENV_NARROW_STREET);
	CHECK(Place4(100, 100, 4, 4, 6) == ENV_TUNNEL);
	CHECK(Place4(4, 4, 100, 100, 6) == ENV_TUNNEL);
	CHECK(Place4(10, 12, 9, 11, 3) == ENV_GARAGE);
	CHECK(Place4(20, 25, 18, 30, 12) == ENV_INTERIOR);
	CHECK(Place4(20, 25, 18, 30, 12, false, true) == ENV_GARAGE);
	CHECK(Place(10, 12, 9, 11, 3, 3, 3, 100) == ENV_GARAGE);
	CHECK(Place(20, 25, 18, 30, 5, 5, 5, 100) == ENV_GARAGE);
	CHECK(Place(20, 25, 18, 30, 7, 7, 7, 100) == ENV_INTERIOR);
	CHECK(Place(11, 12.5f, 100, 100, 100, 100, 100, 100) == ENV_NARROW_STREET);
}

static void
TestLowCeilingIsTheInputs(void)
{
	PlaceInput p = { 20, 25, 18, 30, { 5, 5, 5, 5 }, false, false, 6.0f };
	CHECK(ClassifyPlace(p) == ENV_GARAGE);
	p.lowCeiling = 4.0f;
	CHECK(ClassifyPlace(p) == ENV_INTERIOR);
	p.lowCeiling = 5.5f;
	CHECK(ClassifyPlace(p) == ENV_GARAGE);
	p.roomed = true;
	p.lowCeiling = 1.0f;
	CHECK(ClassifyPlace(p) == ENV_GARAGE);
	p.enclosed = true;
	CHECK(ClassifyPlace(p) == ENV_INTERIOR);
}

static void
TestClassifyInterior(void)
{
	CHECK(Place4(100, 100, 100, 100, 100, true) == ENV_INTERIOR);
	CHECK(Place4(100, 100, 4, 4, 6, true) == ENV_INTERIOR);
	CHECK(Place4(10, 12, 9, 11, 3, true) == ENV_INTERIOR);
	CHECK(Place4(100, 100, 5, 6, 100, true, true) == ENV_INTERIOR);
	CHECK(Place4(NAN, -1, INFINITY, 2, NAN, true) == ENV_INTERIOR);
}

static void
TestClassifyThresholds(void)
{
	CHECK(Place(10, 12, 9, 11, 3, 3, 19.9f, 100) == ENV_INTERIOR);
	CHECK(Place(10, 12, 9, 11, 3, 3, 20, 100) == ENV_NARROW_STREET);
	CHECK(Place4(100, 100, 14.9f, 14.9f, 6) == ENV_TUNNEL);
	CHECK(Place4(100, 100, 15, 15, 6) == ENV_INTERIOR);
	CHECK(Place4(30.1f, 30.1f, 4, 4, 6) == ENV_TUNNEL);
	CHECK(Place4(30, 30, 4, 4, 6) == ENV_INTERIOR);
	CHECK(Place4(20, 25, 18, 30, 5.9f) == ENV_GARAGE);
	CHECK(Place4(20, 25, 18, 30, 6) == ENV_INTERIOR);
	CHECK(Place4(100, 100, 11.9f, 11.9f, 100) == ENV_NARROW_STREET);
	CHECK(Place4(100, 100, 12, 12, 100) == ENV_OPEN);
}

static void
TestClassifyBadRays(void)
{
	CHECK(Place4(100, 100, -1, -1, 100) == ENV_OPEN);
	CHECK(Place4(100, 100, -INFINITY, 5, 100) == ENV_OPEN);
	CHECK(Place4(NAN, 100, 4, 4, 6) == ENV_TUNNEL);
	CHECK(Place4(100, NAN, 4, 4, 6) == ENV_TUNNEL);
	CHECK(Place4(INFINITY, 100, 4, 4, 6) == ENV_TUNNEL);
	CHECK(Place(20, 25, 18, 30, -1, -1, -1, 100) == ENV_OPEN);
	CHECK(Place(20, 25, 18, 30, -INFINITY, -2, -3, 100) == ENV_OPEN);
	CHECK(Place(10, 12, 9, 11, 3, 3, 3, NAN) == ENV_GARAGE);
}

static void
TestClassifyZeroRays(void)
{
	CHECK(Place4(0, 0, 0, 0, 0) == ENV_OPEN);
	CHECK(Place(10, 12, 9, 11, 3, 3, 0, 100) == ENV_NARROW_STREET && Place(10, 12, 9, 11, 3, 3, 100, 100) == ENV_NARROW_STREET);
	CHECK(Place4(100, 100, 4, 0, 6) == ENV_INTERIOR && Place4(100, 100, 4, 100, 6) == ENV_INTERIOR);
	CHECK(Place4(0.01f, 0.01f, 0.01f, 0.01f, 0.01f) == ENV_GARAGE);
}

static bool
PresetIs(int env, float d0, float dn, float mn, float mx, float g, float base, float scale)
{
	const EnvPreset &p = kEnvPresets[env];
	return p.decay0 == d0 && p.decayNyquist == dn && p.minDelay == mn && p.maxDelay == mx &&
		p.gainDb == g && p.base == base && p.rayScale == scale;
}

static void
TestPresets(void)
{
	CHECK(PresetIs(ENV_OPEN, 0.9f, 0.35f, 0.012f, 0.060f, -12.0f, 0.10f, 0.0f));
	CHECK(PresetIs(ENV_NARROW_STREET, 1.3f, 0.55f, 0.006f, 0.035f, -9.0f, 0.15f, 0.8f));
	CHECK(PresetIs(ENV_TUNNEL, 2.91f, 3.783f, 0.015f, 0.037f, -16.6f, 0.35f, 0.8f));
	CHECK(PresetIs(ENV_GARAGE, 1.8f, 0.9f, 0.004f, 0.025f, -10.0f, 0.30f, 0.8f));
	CHECK(PresetIs(ENV_INTERIOR, 0.7f, 0.35f, 0.003f, 0.018f, -8.0f, 0.25f, 0.5f));
	CHECK(kEnvFadeSeconds == 1.0f);
}

static void
TestAmount(void)
{
	CHECK(Near(EnvAmount(ENV_TUNNEL, 0.6f, 1.0f), 0.83f, 1e-5f));
	CHECK(Near(EnvAmount(ENV_OPEN, 0.6f, 1.0f), 0.10f, 1e-6f));
	CHECK(Near(EnvAmount(ENV_NARROW_STREET, 0.0f, 0.5f), 0.075f, 1e-6f));
	CHECK(Near(EnvAmount(ENV_TUNNEL, 1.0f, 1.0f), 1.0f, 1e-6f));
	CHECK(Near(EnvAmount(ENV_TUNNEL, 1.0f, 0.5f), 0.5f, 1e-6f));
	CHECK(Near(EnvAmount(ENV_TUNNEL, -5.0f, 1.0f), 0.0f, 1e-6f));
}

static void
TestAmountBadInput(void)
{
	float bad[] = { NAN, INFINITY, -INFINITY };
	for(float b : bad){
		CHECK(EnvAmount(ENV_TUNNEL, b, 1.0f) == 0.0f);
		CHECK(EnvAmount(ENV_GARAGE, 0.3f, b) == 0.0f);
	}
	CHECK(Near(EnvAmount(NUM_ENVS, 0.6f, 1.0f), 0.10f, 1e-6f));
	CHECK(Near(EnvAmount(-1, 0.6f, 1.0f), 0.10f, 1e-6f));
	CHECK(Near(EnvAmount(1000, 0.6f, 0.5f), 0.05f, 1e-6f));
}

static void
TestFrames(void)
{
	CHECK(EnvFrames(kEnvFadeSeconds, 48000.0f) == 48000);
	CHECK(EnvFrames(kEnvFadeSeconds, 44100.0f) == 44100);
	CHECK(EnvFrames(1.0f, 1e9f) == 1);
	CHECK(EnvDecayFrames(ENV_OPEN, 48000.0f) == 43200);
	CHECK(EnvDecayFrames(ENV_TUNNEL, 48000.0f) == 181584);
	CHECK(EnvDecayFrames(ENV_GARAGE, 48000.0f) == 86400);
	CHECK(EnvDecayFrames(ENV_NARROW_STREET, 44100.0f) == 57330);
	CHECK(EnvDecayFrames(NUM_ENVS, 48000.0f) == 43200);
	float bad[] = { NAN, INFINITY, -INFINITY, 0.0f, -48000.0f };
	for(float b : bad){
		CHECK(EnvFrames(kEnvFadeSeconds, b) == 1);
		CHECK(EnvDecayFrames(ENV_TUNNEL, b) == 1);
	}
}

static bool
OneHot(const EnvState &s, int place)
{
	for(int k = 0; k < NUM_ENVS; k++)
		if(s.weight[k] != (k == place ? 1.0f : 0.0f))
			return false;
	return true;
}

static float
Power(const float *w)
{
	float p = 0.0f;
	for(int k = 0; k < NUM_ENVS; k++)
		p += w[k] * w[k];
	return p;
}

static void
Weights(EnvState &s, uint32_t frames, float rate, float *w)
{
	EnvFadeAdvance(s, frames, rate, w);
}

static void
TestInit(void)
{
	EnvState s;
	EnvInit(s, ENV_TUNNEL);
	CHECK(s.current == ENV_TUNNEL && s.candidate == ENV_TUNNEL && s.seen == 0);
	CHECK(OneHot(s, ENV_TUNNEL) && s.fadeLeft == 0);
	float w[NUM_ENVS];
	Weights(s, 512, 48000.0f, w);
	for(int k = 0; k < NUM_ENVS; k++)
		CHECK(w[k] == (k == ENV_TUNNEL ? 1.0f : 0.0f));
	EnvInit(s, 12);
	CHECK(s.current == ENV_OPEN && OneHot(s, ENV_OPEN));
	EnvInit(s, -2);
	CHECK(s.current == ENV_OPEN);
}

static void
TestHysteresis(void)
{
	EnvState s;
	EnvInit(s, ENV_OPEN);
	CHECK(!EnvObserve(s, ENV_OPEN));
	CHECK(!EnvObserve(s, ENV_TUNNEL));
	CHECK(!EnvObserve(s, ENV_OPEN));
	CHECK(!EnvObserve(s, ENV_TUNNEL));
	CHECK(s.current == ENV_OPEN);
	CHECK(EnvObserve(s, ENV_TUNNEL));
	CHECK(s.current == ENV_TUNNEL);
	CHECK(s.fadeLeft != 0);

	EnvInit(s, ENV_OPEN);
	CHECK(!EnvObserve(s, ENV_TUNNEL));
	CHECK(!EnvObserve(s, ENV_GARAGE));
	CHECK(EnvObserve(s, ENV_GARAGE));
	CHECK(s.current == ENV_GARAGE);
}

static void
TestPlaceClamped(void)
{
	EnvState s;
	EnvInit(s, ENV_TUNNEL);
	CHECK(!EnvObserve(s, 9));
	CHECK(EnvObserve(s, 9));
	CHECK(s.current == ENV_OPEN);

	EnvInit(s, ENV_GARAGE);
	CHECK(!EnvObserve(s, -3));
	CHECK(EnvObserve(s, NUM_ENVS));
	CHECK(s.current == ENV_OPEN);
}

static void
Switch(EnvState &s, int place)
{
	CHECK(!EnvObserve(s, place));
	CHECK(EnvObserve(s, place));
}

static void
TestFadeCurve(void)
{
	const float q = 0.38268343f, h = 0.70710678f, t = 0.92387953f;
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 0, 48000.0f, w);
	CHECK(w[ENV_OPEN] == 1.0f && w[ENV_TUNNEL] == 0.0f);
	Weights(s, 12000, 48000.0f, w);
	CHECK(Near(w[ENV_OPEN], t, 1e-6f) && Near(w[ENV_TUNNEL], q, 1e-6f));
	CHECK(s.weight[ENV_TUNNEL] == w[ENV_TUNNEL]);
	Weights(s, 12000, 48000.0f, w);
	CHECK(Near(w[ENV_OPEN], h, 1e-6f) && Near(w[ENV_TUNNEL], h, 1e-6f));
	Weights(s, 12000, 48000.0f, w);
	CHECK(Near(w[ENV_OPEN], q, 1e-6f) && Near(w[ENV_TUNNEL], t, 1e-6f));
	CHECK(s.fadeLeft == 12000);
	for(int k = 0; k < NUM_ENVS; k++)
		CHECK(k == ENV_OPEN || k == ENV_TUNNEL || w[k] == 0.0f);
	Weights(s, 12000, 48000.0f, w);
	CHECK(w[ENV_OPEN] == 0.0f && w[ENV_TUNNEL] == 1.0f);
	CHECK(s.fadeLeft == 0 && OneHot(s, ENV_TUNNEL));
	Weights(s, 4096, 48000.0f, w);
	CHECK(w[ENV_OPEN] == 0.0f && w[ENV_TUNNEL] == 1.0f);

	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 22050, 44100.0f, w);
	CHECK(Near(w[ENV_OPEN], h, 1e-6f) && Near(w[ENV_TUNNEL], h, 1e-6f));
	Weights(s, 22050, 44100.0f, w);
	CHECK(w[ENV_OPEN] == 0.0f && w[ENV_TUNNEL] == 1.0f && s.fadeLeft == 0);
}

static void
TestChangeDuringFadeStartsAtOnce(void)
{
	EnvState s;
	float before[NUM_ENVS], after[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 20000, 48000.0f, before);
	CHECK(!EnvObserve(s, ENV_GARAGE));
	CHECK(s.current == ENV_TUNNEL && s.fadeLeft == 28000);
	CHECK(EnvObserve(s, ENV_GARAGE));
	CHECK(s.current == ENV_GARAGE && s.fadeLeft != 0);
	Weights(s, 0, 48000.0f, after);
	for(int k = 0; k < NUM_ENVS; k++)
		CHECK(Near(before[k], after[k], 1e-5f));
	CHECK(s.fadeLeft == 48000);
	Weights(s, 24000, 48000.0f, after);
	CHECK(after[ENV_GARAGE] > before[ENV_GARAGE] && after[ENV_OPEN] < before[ENV_OPEN] && after[ENV_TUNNEL] < before[ENV_TUNNEL]);
	CHECK(Near(Power(after), 1.0f, 1e-5f));
	Weights(s, 24000, 48000.0f, after);
	CHECK(OneHot(s, ENV_GARAGE) && after[ENV_GARAGE] == 1.0f);
}

static void
TestChangeBackDuringFade(void)
{
	EnvState s;
	float w[NUM_ENVS], prev[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 24000, 48000.0f, w);
	CHECK(Near(w[ENV_OPEN], 0.70710678f, 1e-6f));
	Switch(s, ENV_OPEN);
	Weights(s, 0, 48000.0f, w);
	CHECK(Near(w[ENV_OPEN], 0.70710678f, 1e-5f) && Near(w[ENV_TUNNEL], 0.70710678f, 1e-5f));
	memcpy(prev, w, sizeof(prev));
	bool rising = true, falling = true;
	for(int i = 0; i < 48000; i += 8){
		Weights(s, 8, 48000.0f, w);
		rising = rising && w[ENV_OPEN] >= prev[ENV_OPEN];
		falling = falling && w[ENV_TUNNEL] <= prev[ENV_TUNNEL];
		CHECK(Near(Power(w), 1.0f, 1e-5f));
		memcpy(prev, w, sizeof(prev));
	}
	CHECK(rising && falling);
	CHECK(OneHot(s, ENV_OPEN));
}

static void
TestQuickChangeBackEarlyInFade(void)
{
	EnvState s;
	float before[NUM_ENVS], w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 4800, 48000.0f, before);
	Weights(s, 0, 48000.0f, before);
	CHECK(before[ENV_OPEN] > 0.98f && before[ENV_OPEN] < 1.0f && before[ENV_TUNNEL] > 0.0f);
	Switch(s, ENV_OPEN);
	CHECK(s.current == ENV_OPEN && s.fadeLeft != 0);
	Weights(s, 0, 48000.0f, w);
	for(int k = 0; k < NUM_ENVS; k++)
		CHECK(Near(w[k], before[k], 1e-5f));
	CHECK(w[ENV_TUNNEL] > 0.0f && w[ENV_OPEN] < 1.0f);
	Weights(s, 2400, 48000.0f, w);
	CHECK(w[ENV_OPEN] > before[ENV_OPEN] && w[ENV_TUNNEL] < before[ENV_TUNNEL] && w[ENV_TUNNEL] > 0.0f);
	CHECK(Near(Power(w), 1.0f, 1e-5f));
	Weights(s, 48000, 48000.0f, w);
	CHECK(OneHot(s, ENV_OPEN));
}

static void
TestChangeToTheWeightedPlace(void)
{
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Switch(s, ENV_OPEN);
	CHECK(s.current == ENV_OPEN && s.fadeLeft == 0 && OneHot(s, ENV_OPEN));
	Weights(s, 100, 48000.0f, w);
	CHECK(w[ENV_OPEN] == 1.0f);
}

static void
TestFadeEqualPower(void)
{
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	const int chain[] = { ENV_TUNNEL, ENV_GARAGE, ENV_NARROW_STREET, ENV_INTERIOR, ENV_OPEN, ENV_GARAGE };
	float worst = 0.0f;
	for(int place : chain){
		Switch(s, place);
		for(int i = 0; i < 13000; i++){
			Weights(s, 1, 48000.0f, w);
			worst = fmaxf(worst, fabsf(Power(w) - 1.0f));
			for(int k = 0; k < NUM_ENVS; k++)
				CHECK(w[k] >= 0.0f && w[k] <= 1.0f);
		}
	}
	CHECK(worst < 1e-5f);
}

static void
RecordFade(std::vector<float> &out, uint32_t block, uint32_t total)
{
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	out.clear();
	Weights(s, 0, 48000.0f, w);
	out.insert(out.end(), w, w + NUM_ENVS);
	for(uint32_t done = 0; done < total; done += block){
		if(done == 12288)
			Switch(s, ENV_GARAGE);
		Weights(s, block, 48000.0f, w);
		out.insert(out.end(), w, w + NUM_ENVS);
	}
}

static void
TestFadeBlocks(void)
{
	std::vector<float> one, big;
	RecordFade(one, 1, 70000);
	uint32_t blocks[] = { 128, 512, 4096 };
	for(uint32_t n : blocks){
		RecordFade(big, n, 70000);
		int bad = 0;
		for(size_t k = 0; k * NUM_ENVS < big.size(); k++){
			size_t f = k * n;
			if((f + 1) * NUM_ENVS > one.size())
				break;
			for(int u = 0; u < NUM_ENVS; u++)
				if(big[k * NUM_ENVS + u] != one[f * NUM_ENVS + u])
					bad++;
		}
		CHECK(bad == 0);
	}
}

static void
TestFadeBadRate(void)
{
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 1, NAN, w);
	CHECK(w[ENV_OPEN] == 0.0f && w[ENV_TUNNEL] == 1.0f && s.fadeLeft == 0);
}

static void
TestSettle(void)
{
	EnvState s;
	float w[NUM_ENVS];
	EnvInit(s, ENV_OPEN);
	Switch(s, ENV_TUNNEL);
	Weights(s, 20000, 48000.0f, w);
	EnvSettle(s);
	CHECK(s.current == ENV_TUNNEL && s.fadeLeft == 0 && OneHot(s, ENV_TUNNEL));
	Weights(s, 1, 48000.0f, w);
	CHECK(w[ENV_TUNNEL] == 1.0f && w[ENV_OPEN] == 0.0f);
}

static void
TestLongestDecay(void)
{
	CHECK(EnvLongestDecayFrames(48000.0f) == 181584);
	CHECK(EnvLongestDecayFrames(44100.0f) == 166831);
	float bad[] = { NAN, INFINITY, -INFINITY, 0.0f, -48000.0f };
	for(float b : bad)
		CHECK(EnvLongestDecayFrames(b) == 1);
}

static uint32_t
PullBlocksAfterSpatial(uint32_t block)
{
	uint32_t hold = 0, pulled = 0;
	CHECK(EnvPullBlock(hold, block, block, 48000.0f));
	for(uint32_t i = 0; i < 400000; i++)
		if(EnvPullBlock(hold, 0, block, 48000.0f))
			pulled++;
	return pulled;
}

static void
TestPullHold(void)
{
	uint32_t hold = 0;
	for(int i = 0; i < 100; i++)
		CHECK(!EnvPullBlock(hold, 0, 512, 48000.0f));
	CHECK(hold == 0);
	CHECK(EnvPullBlock(hold, 1, 512, 48000.0f) && hold == 181584);
	CHECK(EnvPullBlock(hold, 512, 512, 48000.0f) && hold == 181584);
	CHECK(PullBlocksAfterSpatial(512) == 355);
	CHECK(PullBlocksAfterSpatial(4096) == 45);
	CHECK(PullBlocksAfterSpatial(1) == 181584);
	hold = 100;
	CHECK(EnvPullBlock(hold, 0, 512, 48000.0f) && hold == 0);
	CHECK(!EnvPullBlock(hold, 0, 512, 48000.0f));
	hold = 512;
	CHECK(EnvPullBlock(hold, 0, 512, 48000.0f) && hold == 0);
	CHECK(!EnvPullBlock(hold, 0, 512, 48000.0f));
	hold = 513;
	CHECK(EnvPullBlock(hold, 0, 512, 48000.0f) && hold == 1);
	CHECK(EnvPullBlock(hold, 0, 512, 48000.0f) && hold == 0);
	CHECK(!EnvPullBlock(hold, 0, 512, 48000.0f));
}

static void
TestPullHoldRestartsOnSpatial(void)
{
	uint32_t hold = 0;
	EnvPullBlock(hold, 512, 512, 48000.0f);
	for(int i = 0; i < 200; i++)
		EnvPullBlock(hold, 0, 512, 48000.0f);
	CHECK(hold < 181584 && hold > 0);
	CHECK(EnvPullBlock(hold, 1, 512, 48000.0f) && hold == 181584);
	for(int i = 0; i < 354; i++)
		CHECK(EnvPullBlock(hold, 0, 512, 48000.0f));
	CHECK(hold == 181584 - 354 * 512 && hold > 0);
	CHECK(EnvPullBlock(hold, 0, 512, 48000.0f) && hold == 0);
	CHECK(!EnvPullBlock(hold, 0, 512, 48000.0f));
}

int
main(void)
{
	TestClassify();
	TestLowCeilingIsTheInputs();
	TestClassifyInterior();
	TestClassifyThresholds();
	TestClassifyBadRays();
	TestClassifyZeroRays();
	TestPresets();
	TestAmount();
	TestAmountBadInput();
	TestFrames();
	TestInit();
	TestHysteresis();
	TestPlaceClamped();
	TestFadeCurve();
	TestChangeDuringFadeStartsAtOnce();
	TestChangeBackDuringFade();
	TestQuickChangeBackEarlyInFade();
	TestChangeToTheWeightedPlace();
	TestFadeEqualPower();
	TestFadeBlocks();
	TestFadeBadRate();
	TestSettle();
	TestLongestDecay();
	TestPullHold();
	TestPullHoldRestartsOnSpatial();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all audio_env tests passed\n");
	return 0;
}
