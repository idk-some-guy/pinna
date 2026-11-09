// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -I tests/support tests/audio_compare_test.cpp -o build/tests/audio_compare_test && build/tests/audio_compare_test
#include <cstdio>
#include <cmath>
#include <new>
#include "compare.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static Wave
Tone(uint32_t rate, float hz, float amp, float seconds)
{
	Wave w;
	w.rate = rate;
	size_t n = (size_t)(rate * seconds);
	w.l.resize(n);
	w.r.resize(n);
	for(size_t i = 0; i < n; i++){
		w.l[i] = amp * sinf(2.0f * 3.14159265f * hz * i / rate);
		w.r[i] = 0.5f * w.l[i];
	}
	return w;
}

static Wave
Clicks(uint32_t rate, float shiftMs)
{
	Wave w;
	w.rate = rate;
	w.l.assign(rate, 0.0f);
	w.r.assign(rate, 0.0f);
	const float at[3] = { 0.1f, 0.4f, 0.75f };
	for(int k = 0; k < 3; k++){
		size_t s = (size_t)((at[k] + shiftMs / 1000.0f) * rate);
		for(size_t i = 0; i < rate / 20 && s + i < (size_t)rate; i++)
			w.l[s + i] = w.r[s + i] = 0.5f * expf(-(float)i / (rate / 200.0f)) * sinf(0.3f * i);
	}
	return w;
}

static Wave
DecayingNoise(uint32_t rate, float t60, float startS, float seconds, float floor)
{
	Wave w;
	w.rate = rate;
	size_t n = (size_t)(rate * seconds);
	w.l.assign(n, 0.0f);
	w.r.assign(n, 0.0f);
	unsigned seed = 3;
	for(size_t i = (size_t)(startS * rate); i < n; i++){
		float t = (float)i / rate - startS;
		float env = 0.5f * powf(10.0f, -3.0f * t / t60) + floor;
		seed = seed * 1103515245u + 12345u;
		w.l[i] = env * (((seed >> 9) & 0xffff) / 32768.0f - 1.0f);
		seed = seed * 1103515245u + 12345u;
		w.r[i] = env * (((seed >> 9) & 0xffff) / 32768.0f - 1.0f);
	}
	return w;
}

static void
TestLevels(void)
{
	Wave a = Tone(32000, 440.0f, 0.5f, 1.0f), b = Tone(48000, 440.0f, 0.5f, 1.0f);
	LevelResult lr = CompareLevels(a, b, 50.0f, -60.0f);
	CHECK(lr.windows == 40 && lr.worstDb < 0.01f);
	Wave c = Tone(48000, 440.0f, 0.5f * 0.95f, 1.0f);
	lr = CompareLevels(a, c, 50.0f, -60.0f);
	CHECK(fabsf(lr.worstDb - 0.4455f) < 0.01f && lr.over == 0);
	Wave d = b;
	const int dips[3] = { 5, 6, 15 };
	for(int k = 0; k < 3; k++)
		for(int i = 0; i < 2400; i++)
			d.l[dips[k] * 2400 + i] *= 0.9f;
	lr = CompareLevels(a, d, 50.0f, -60.0f);
	CHECK(lr.over == 3 && fabsf(lr.worstDb - 0.915f) < 0.01f);
	Wave quiet = Tone(48000, 440.0f, 0.0005f, 1.0f), quieter = Tone(32000, 440.0f, 0.0002f, 1.0f);
	lr = CompareLevels(quieter, quiet, 50.0f, -60.0f);
	CHECK(lr.windows == 0);
}

static void
TestOnsets(void)
{
	OnsetResult o = CompareOnsets(Clicks(32000, 0.0f), Clicks(48000, 4.0f));
	CHECK(o.refCount == 3 && o.testCount == 3 && fabsf(o.worstMs - 4.0f) < 1.0f);
	o = CompareOnsets(Clicks(32000, 0.0f), Clicks(48000, 15.0f));
	CHECK(o.worstMs > 10.0f);
	Wave atZero = Tone(48000, 440.0f, 0.5f, 1.0f);
	std::vector<double> on = Onsets(atZero);
	CHECK(on.size() == 1 && on[0] < 0.001);
}

static void
TestPitch(void)
{
	Wave p = Tone(32000, 220.0f, 0.5f, 1.0f), q = Tone(48000, 221.0f, 0.5f, 1.0f);
	PitchResult pr = ComparePitch(p, q, 50.0f, 0.0005, 0.02);
	CHECK(pr.windows >= 15 && fabsf(pr.worstRatio - 1.0f / 220.0f) < 0.0015f);
	pr = ComparePitch(p, Tone(48000, 220.0f, 0.5f, 1.0f), 50.0f, 0.0005, 0.02);
	CHECK(pr.worstRatio < 0.0005f);
}

static void
TestWav(void)
{
	Wave a = Tone(32000, 440.0f, 0.5f, 1.0f);
	a.l[7] = 3.25f;
	const char *path = "build/tests/compare_test.wav";
	CHECK(WriteWav(path, a));
	Wave back;
	CHECK(ReadWav(path, back) && back.rate == 32000 && back.l.size() == a.l.size() && back.r[100] == a.r[100] && back.l[7] == 3.25f);
	CHECK(!ReadWav("build/tests/no_such_compare_test.wav", back));
	CHECK(back.rate == 0 && back.l.empty() && back.r.empty());
	alignas(Wave) unsigned char raw[sizeof(Wave)];
	memset(raw, 0xab, sizeof(raw));
	Wave *fresh = new(raw) Wave;
	CHECK(fresh->rate == 0);
	CHECK(!ReadWav("build/tests/no_such_compare_test.wav", *fresh) && fresh->rate == 0);
	fresh->l.assign(48000, 0.5f);
	fresh->r.assign(48000, 0.5f);
	CHECK(Onsets(*fresh).empty());
	OnsetResult o = CompareOnsets(*fresh, a);
	CHECK(o.refCount == 0);
	CHECK(CompareLevels(*fresh, a, 50.0f, -60.0f).windows == 0);
	CHECK(ComparePitch(*fresh, a, 50.0f, 0.0005, 0.02).windows == 0);
	fresh->~Wave();
}

static void
TestPeakAndTail(void)
{
	Wave a = Tone(48000, 440.0f, 0.5f, 1.0f);
	a.r[4000] = -1.75f;
	CHECK(PeakAbs(a) == 1.75f);
	Wave t = Tone(48000, 440.0f, 0.1f, 1.0f);
	double db = EnergyDb(t, 0.5, 0.2);
	CHECK(fabs(db - 10.0 * log10(0.5 * 0.01 * 1.25)) < 0.01);
	CHECK(EnergyDb(t, 2.0, 0.1) < -150.0);
}

static void
TestDecay(void)
{
	Wave w = DecayingNoise(48000, 1.5f, 0.2f, 2.5f, 0.0f);
	double t60 = DecaySeconds(w, 0.2, 2.5);
	CHECK(fabs(t60 - 1.5) < 0.05);
	Wave v = DecayingNoise(48000, 0.6f, 0.0f, 2.0f, 0.0f);
	CHECK(fabs(DecaySeconds(v, 0.0, 2.0) - 0.6) < 0.03);
	Wave floored = DecayingNoise(48000, 1.0f, 0.0f, 4.0f, 0.001f);
	CHECK(fabs(DecaySeconds(floored, 0.0, 4.0) - 1.0) < 0.05);
}

static void
TestFadeOut(void)
{
	Wave a = Tone(48000, 300.0f, 0.5f, 1.0f);
	size_t cut = 24000 + 40;
	Wave hard = a;
	for(size_t i = cut; i < hard.l.size(); i++)
		hard.l[i] = hard.r[i] = 0.0f;
	Wave faded = a;
	for(size_t i = cut; i < faded.l.size(); i++){
		float g = i < cut + 240 ? 1.0f - (float)(i - cut) / 240.0f : 0.0f;
		faded.l[i] *= g;
		faded.r[i] *= g;
	}
	CHECK(FadeOutFrames(hard.l, 48000, (double)cut / 48000) == 0);
	int f = FadeOutFrames(faded.l, 48000, (double)cut / 48000);
	CHECK(f >= 230 && f <= 240);
	CHECK(FadeOutFrames(a.l, 48000, 0.5) < 0);
	std::vector<float> gated(48000);
	for(size_t i = 0; i < gated.size(); i++)
		gated[i] = (i / 10) % 2 ? 0.5f : 0.0f;
	CHECK(FadeOutFrames(gated, 48000, 0.5) < 0);
}

static void
TestSplineBand(void)
{
	CHECK(OalSplineResponseDb(0.0) > -1e-9);
	CHECK(OalSplineResponseDb(kOalSplineFlatFraction) > -0.1);
	CHECK(OalSplineResponseDb(0.2) < -0.25);
	CHECK(fabs(OalSplineResponseDb(0.5) + 4.2) < 0.05);
}

static Wave
TwoBand(float lowAmp, float highAmp)
{
	Wave w = Tone(48000, 500.0f, lowAmp, 1.0f);
	for(size_t i = 0; i < w.l.size(); i++){
		w.l[i] += highAmp * sinf(2.0f * 3.14159265f * 9000.0f * i / 48000.0f);
		w.r[i] = w.l[i];
	}
	return w;
}

static void
TestLowPass(void)
{
	Wave a = TwoBand(0.3f, 0.3f), b = TwoBand(0.3f, 0.1f);
	CHECK(CompareLevels(a, b, 50.0f, -60.0f).worstDb > 2.0f);
	LevelResult lr = CompareLevels(LowPassWave(a, 2000.0), LowPassWave(b, 2000.0), 50.0f, -60.0f);
	CHECK(lr.windows > 0 && lr.worstDb < 0.01f);
	Wave c = TwoBand(0.25f, 0.3f);
	CHECK(CompareLevels(LowPassWave(a, 2000.0), LowPassWave(c, 2000.0), 50.0f, -60.0f).worstDb > 1.5f);
}

static Wave
Bursts(float shiftMs)
{
	Wave w = Tone(48000, 500.0f, 0.3f, 1.0f);
	size_t shift = (size_t)(shiftMs * 48.0f);
	for(size_t i = 0; i < w.l.size(); i++){
		bool on = i >= shift && ((i - shift) / 4800) % 2 == 0;
		w.l[i] = w.r[i] = on ? w.l[i] : 0.0f;
	}
	return w;
}

static void
TestBandRule(void)
{
	Wave a = TwoBand(0.3f, 0.3f), above = TwoBand(0.3f, 0.1f);
	LevelResult full = CompareLevels(a, above, 50.0f, -60.0f);
	LevelRecord rec = { full.worstDb, full.over };
	BandLevels b = CompareLevelsByBand(a, above, 22050, &rec);
	CHECK(fabs(b.cutoffHz - 0.15 * 22050) < 1e-6);
	CHECK(b.bandOk && b.band.windows > 0 && b.band.worstDb < 0.01f);
	CHECK(!b.fullOk && b.full.worstDb > 2.0f && b.full.over == full.over);
	CHECK(b.verdict == LEVELS_RESAMPLER);
	CHECK(CompareLevelsByBand(a, above, 22050, NULL).verdict == LEVELS_FAIL);
	LevelRecord off = { full.worstDb + 0.4f, full.over };
	CHECK(CompareLevelsByBand(a, above, 22050, &off).verdict == LEVELS_FAIL);
	LevelRecord fewer = { full.worstDb, full.over + 1 };
	CHECK(CompareLevelsByBand(a, above, 22050, &fewer).verdict == LEVELS_FAIL);
	LevelRecord more = { full.worstDb, full.over - 1 };
	CHECK(CompareLevelsByBand(a, above, 22050, &more).verdict == LEVELS_FAIL);

	Wave quieter = a;
	for(size_t i = 0; i < quieter.l.size(); i++){
		quieter.l[i] *= 0.891251f;
		quieter.r[i] *= 0.891251f;
	}
	LevelResult broad = CompareLevels(a, quieter, 50.0f, -60.0f);
	LevelRecord broadRec = { broad.worstDb, broad.over };
	b = CompareLevelsByBand(a, quieter, 22050, &broadRec);
	CHECK(!b.bandOk && fabsf(b.band.worstDb - 1.0f) < 0.02f && b.verdict == LEVELS_FAIL);

	Wave early = Bursts(0.0f), late = Bursts(30.0f);
	LevelResult shifted = CompareLevels(early, late, 50.0f, -60.0f);
	LevelRecord shiftRec = { shifted.worstDb, shifted.over };
	b = CompareLevelsByBand(early, late, 22050, &shiftRec);
	CHECK(!b.bandOk && b.verdict == LEVELS_FAIL);

	b = CompareLevelsByBand(a, a, 22050, NULL);
	CHECK(b.bandOk && b.fullOk && b.verdict == LEVELS_PASS);
	CHECK(CompareLevelsByBand(a, a, 22050, &rec).verdict == LEVELS_FAIL);
	b = CompareLevelsByBand(a, a, UINT32_MAX, NULL);
	CHECK(!b.bandOk && b.verdict == LEVELS_PASS);
}

static void
TestShape(void)
{
	Wave a = Tone(48000, 300.0f, 0.5f, 0.1f), faded = a, cut = a;
	for(size_t i = 1000; i < a.l.size(); i++){
		float g = i < 1064 ? (1064.0f - i) / 64.0f : 0.0f;
		faded.l[i] *= g;
		faded.r[i] *= g;
		cut.l[i] = cut.r[i] = i < 1010 ? a.l[i] : 0.0f;
	}
	ShapeDiff d = CompareShape(faded, faded, 1000, 64);
	CHECK(d.worst == 0.0f && d.peakRef > 0.0f && d.peakRef == d.peakTest);
	d = CompareShape(faded, cut, 1000, 64);
	CHECK(d.worst > 0.1f);
	CHECK(CompareShape(faded, cut, 1100, 64).worst == 0.0f);
	Wave right = faded;
	right.r[1020] += 0.25f;
	CHECK(fabsf(CompareShape(faded, right, 1000, 64).worst - 0.25f) < 1e-6f);
	CHECK(CompareShape(faded, faded, a.l.size() - 10, 64).worst == 0.0f);
}

static const size_t kHoldEnd = 8100, kHoldBoundary = 8000 + 341;

static Wave
Engine(float under)
{
	Wave w = Tone(48000, 300.0f, 0.2f, 0.5f);
	for(size_t i = 0; i < w.l.size(); i++)
		w.r[i] = w.l[i] = i >= kHoldEnd && i < 12000 ? under * sinf(2.0f * 3.14159265f * 700.0f * i / 48000.0f) : w.l[i];
	return w;
}

static Wave
Held(const Wave &engine, float held, size_t boundary)
{
	Wave w = engine;
	for(size_t i = kHoldEnd; i <= boundary + 64; i++){
		float g = i <= boundary ? 1.0f : 1.0f - (float)(i - boundary) / 64.0f;
		w.l[i] += held * g;
		w.r[i] += held * g;
	}
	return w;
}

static void
TestHoldMask(void)
{
	Wave test = Engine(0.0f), ref = Held(test, 0.12f, kHoldBoundary);
	CHECK(CompareLevels(ref, test, 50.0f, -60.0f).worstDb > 0.5f);
	std::vector<HoldSpan> spans;
	std::vector<uint8_t> mask = HoldMask(ref, test, 30.0f, 341, &spans);
	CHECK(spans.size() == 2 && spans[0].boundary == kHoldBoundary && fabsf(spans[0].held - 0.12f) < 1e-6f);
	CHECK(spans.size() == 2 && spans[0].from == kHoldEnd && spans[0].to == kHoldBoundary + 65);
	CHECK(CompareLevels(ref, test, 50.0f, -60.0f, &mask).worstDb < 0.01f);

	Wave louder = test;
	for(size_t i = 0; i < louder.l.size(); i++)
		louder.l[i] *= 1.2f;
	spans.clear();
	HoldMask(louder, test, 30.0f, 341, &spans);
	CHECK(spans.empty());
}

static void
TestHoldRule(void)
{
	Wave silent = Engine(0.0f), held = Held(silent, 0.12f, kHoldBoundary);
	LevelResult full = CompareLevels(held, silent, 50.0f, -60.0f);
	BandLevels plain = CompareLevelsByBand(held, silent, 22050, NULL);
	CHECK(!plain.fullOk && !plain.bandOk);
	HoldRecord fullRec = { full.worstDb, 2 }, bandRec = { plain.band.worstDb, 2 };
	LevelRecords recs = { NULL, &fullRec, &bandRec };
	BandLevels b = CompareLevelsByBand(held, silent, 22050, recs, 30.0f, 341);
	CHECK(b.verdict == LEVELS_HOLD && b.bandVerdict == LEVELS_HOLD);
	CHECK(b.held.segments == 2 && b.held.frames == kHoldBoundary + 65 - kHoldEnd);
	CHECK(b.held.full.worstDb < 0.01f && b.held.band.worstDb < 0.05f && b.held.full.windows > 0);
	LevelRecords none = { NULL, NULL, NULL };
	b = CompareLevelsByBand(held, silent, 22050, none, 30.0f, 341);
	CHECK(b.verdict == LEVELS_FAIL && b.bandVerdict == LEVELS_FAIL && b.held.segments == 2);
	HoldRecord moreSegments = { full.worstDb, 3 }, offRecord = { full.worstDb + 0.4f, 2 };
	LevelRecords wrongCount = { NULL, &moreSegments, &bandRec }, wrongLevel = { NULL, &offRecord, &bandRec };
	CHECK(CompareLevelsByBand(held, silent, 22050, wrongCount, 30.0f, 341).verdict == LEVELS_FAIL);
	CHECK(CompareLevelsByBand(held, silent, 22050, wrongLevel, 30.0f, 341).verdict == LEVELS_FAIL);
	LevelRecords onlyBand = { NULL, NULL, &bandRec };
	b = CompareLevelsByBand(silent, silent, 22050, onlyBand, 30.0f, 341);
	CHECK(b.verdict == LEVELS_PASS && b.bandVerdict == LEVELS_FAIL);

	Wave offGrid = Held(silent, 0.12f, kHoldBoundary - 40);
	LevelResult offFull = CompareLevels(offGrid, silent, 50.0f, -60.0f);
	HoldRecord offFullRec = { offFull.worstDb, 2 }, offBandRec = { CompareLevelsByBand(offGrid, silent, 22050, NULL).band.worstDb, 2 };
	LevelRecords offRecs = { NULL, &offFullRec, &offBandRec };
	CHECK(offFull.worstDb > 0.5f);
	b = CompareLevelsByBand(offGrid, silent, 22050, offRecs, 30.0f, 341);
	CHECK(b.held.segments == 0 && b.verdict == LEVELS_FAIL && b.bandVerdict == LEVELS_FAIL);

	Wave under = Engine(0.01f), heldOver = Held(under, 0.12f, kHoldBoundary);
	LevelResult overFull = CompareLevels(heldOver, under, 50.0f, -60.0f);
	HoldRecord overFullRec = { overFull.worstDb, 2 }, overBandRec = { CompareLevelsByBand(heldOver, under, 22050, NULL).band.worstDb, 2 };
	LevelRecords overRecs = { NULL, &overFullRec, &overBandRec };
	CHECK(overFull.worstDb > 0.5f);
	b = CompareLevelsByBand(heldOver, under, 22050, overRecs, 30.0f, 341);
	CHECK(b.held.segments == 2 && b.held.frames == kHoldBoundary + 65 - kHoldEnd);
	CHECK(b.verdict == LEVELS_HOLD && b.bandVerdict == LEVELS_HOLD && b.held.full.worstDb < 0.01f);

	Wave heldOther = Held(Engine(0.012f), 0.12f, kHoldBoundary);
	LevelResult otherFull = CompareLevels(heldOther, under, 50.0f, -60.0f);
	HoldRecord otherFullRec = { otherFull.worstDb, 2 }, otherBandRec = { CompareLevelsByBand(heldOther, under, 22050, NULL).band.worstDb, 2 };
	LevelRecords otherRecs = { NULL, &otherFullRec, &otherBandRec };
	CHECK(0.002f > kHoldVariation * 0.12f && otherFull.worstDb > 0.5f);
	b = CompareLevelsByBand(heldOther, under, 22050, otherRecs, 30.0f, 341);
	CHECK(b.held.segments == 0 && b.verdict == LEVELS_FAIL && b.bandVerdict == LEVELS_FAIL);

	Wave quieter = silent;
	for(size_t i = 19200; i < 21600; i++){
		quieter.l[i] *= 0.8f;
		quieter.r[i] *= 0.8f;
	}
	LevelResult both = CompareLevels(held, quieter, 50.0f, -60.0f);
	HoldRecord bothFullRec = { both.worstDb, 2 }, bothBandRec = { CompareLevelsByBand(held, quieter, 22050, NULL).band.worstDb, 2 };
	LevelRecords bothRecs = { NULL, &bothFullRec, &bothBandRec };
	b = CompareLevelsByBand(held, quieter, 22050, bothRecs, 30.0f, 341);
	CHECK(b.held.segments == 2 && b.held.full.worstDb > 1.5f && b.verdict == LEVELS_FAIL && b.bandVerdict == LEVELS_FAIL);
}

int
main(void)
{
	TestLevels();
	TestOnsets();
	TestPitch();
	TestWav();
	TestPeakAndTail();
	TestDecay();
	TestFadeOut();
	TestSplineBand();
	TestLowPass();
	TestHoldMask();
	TestHoldRule();
	TestBandRule();
	TestShape();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all compare tests passed\n");
	return 0;
}
