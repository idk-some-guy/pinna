// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_echo_test.cpp -o build/tests/audio_echo_test && build/tests/audio_echo_test
#include <cstdio>
#include <cmath>
#include "CaEcho.h"

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
TestEligible(void)
{
	CHECK(!EchoTapEligible(8.2f, 60));
	CHECK(EchoTapEligible(8.24f, 60));
	CHECK(EchoTapEligible(20, 60));
	CHECK(!EchoTapEligible(31, 60));
	CHECK(!EchoTapEligible(30, 60));
	CHECK(EchoTapEligible(29.9f, 60));
	CHECK(!EchoTapEligible(100, 400));
	CHECK(EchoTapEligible(99.9f, 400));
	CHECK(!EchoTapEligible(0, 60));
	CHECK(!EchoTapEligible(-5, 60));
	CHECK(!EchoTapEligible(NAN, 60));
}

static void
TestGain(void)
{
	CHECK(Near(EchoTapGain(100, 10, 60, 20), 0.14f, 1e-6f));
	CHECK(EchoTapGain(100, 10, 60, 29) == 0.0f);
	CHECK(Near(EchoTapGain(100, 40, 60, 20), 0.0225f, 1e-6f));
	CHECK(EchoTapGain(100, 10, 60, 5) == 0.0f);
	CHECK(EchoTapGain(100, 10, 60, 31) == 0.0f);
	CHECK(EchoTapGain(0, 10, 60, 20) == 0.0f);
	CHECK(EchoTapGain(100, 10, 60, NAN) == 0.0f);
	CHECK(Near(EchoTapGain(100, 10, 60, 23), 0.13f, 1e-6f));
	CHECK(Near(EchoTapGain(255, 13, 60, 20), 71.0f / 255.0f * 0.25f, 1e-6f));
}

static uint8_t
OldComputeVolume(uint8_t emitting, float maxDistance, float distance)
{
	if(maxDistance <= 0.0f)
		return 0;
	float minDistance = maxDistance / 5.0f;
	if(minDistance > distance)
		return emitting;
	uint8_t v = emitting * (((maxDistance - minDistance - (distance - minDistance)) / (maxDistance - minDistance)) *
		((maxDistance - minDistance - (distance - minDistance)) / (maxDistance - minDistance)));
	return v < 127 ? v : 127;
}

static uint8_t
OldAdjustVolume(uint8_t emitting, float maxDistance, float distance)
{
	float minDistance = maxDistance / 4.0f;
	float diffDistance = maxDistance - minDistance;
	if(distance > diffDistance)
		return (minDistance - (distance - diffDistance)) * (float)emitting / minDistance;
	return emitting;
}

static uint32_t
OldCopyChannelVolume(uint8_t queuedVolume, float maxDist, float reflection, uint32_t scale)
{
	uint8_t emitting = (9 * queuedVolume) >> 4;
	float half = maxDist / 2.0f;
	if(!(reflection > 0.0f && reflection < 100.0f && reflection < half))
		return 0;
	uint8_t delay = reflection * 500.0f / 1029.0f;
	if(!(delay > 3))
		return 0;
	if(!(OldComputeVolume(emitting, half, reflection) > (emitting >> 4)))
		return 0;
	return (OldAdjustVolume(emitting, half, reflection) * scale) / 127;
}

static void
TestTapVolume(void)
{
	CHECK(EchoTapVolume(127, 60, 20) == 71);
	CHECK(EchoTapScaled(127, 127, 60, 20) == 71);
	CHECK(EchoTapScaled(127, 64, 60, 20) == 35);
	CHECK(EchoTapVolume(100, 60, 20) == 56);
	CHECK(EchoTapVolume(7, 100, 20) == 3);
	CHECK(EchoTapScaled(7, 64, 100, 20) == 1);
	CHECK(EchoTapVolume(127, 60, 29) == 0);
	CHECK(EchoTapVolume(127, 60, 5) == 0);
	CHECK(EchoTapVolume(127, 60, NAN) == 0);
	CHECK(EchoTapVolume(0, 60, 20) == 0);
	CHECK(EchoTapVolume(300, 60, 20) == 0);

	static const uint32_t scales[4] = { 127, 64, 100, 1 };
	long mismatches = 0, sounding = 0;
	for(int max = 20; max <= 250; max += 5)
		for(uint32_t v = 0; v <= 127; v++)
			for(int r = 1; r <= 1000; r++){
				float d = r * 0.1f;
				for(uint32_t scale : scales){
					uint32_t want = OldCopyChannelVolume((uint8_t)v, (float)max, d, scale);
					sounding += want != 0;
					if(EchoTapScaled(v, scale, (float)max, d) != want)
						mismatches++;
				}
			}
	CHECK(mismatches == 0);
	CHECK(sounding > 100000);

	long volumeMismatches = 0, lastQuarter = 0;
	for(int max = 20; max <= 250; max += 10)
		for(uint32_t e = 10; e <= 127; e++)
			for(int s = 0; s <= 2 * max; s++){
				float src = s * 0.5f;
				uint8_t v = OldComputeVolume((uint8_t)e, (float)max, src);
				if(EchoComputeVolume((uint8_t)e, (float)max, src) != v)
					volumeMismatches++;
				lastQuarter += src > 0.75f * max && v > 0;
				for(int r = 1; r <= 10; r++){
					float d = r * max / 20.0f;
					float gain = EchoTapGain(e, src, (float)max, d);
					float want = (float)OldCopyChannelVolume(v, (float)max, d, 127) / (float)e * OalInverseClampedGain(100.0f, max / 8.0f, max / 2.0f);
					if(fabsf(gain - want) > 1e-6f)
						volumeMismatches++;
				}
			}
	CHECK(volumeMismatches == 0);
	CHECK(lastQuarter > 1000);
}

static void
TestRendersEcho(void)
{
	CHECK(RendersEchoFor(MODE_SPATIAL, true));
	CHECK(!RendersEchoFor(MODE_CLASSIC, true));
	CHECK(!RendersEchoFor(MODE_REIMAGINED, true));
	CHECK(!RendersEchoFor(MODE_SPATIAL, false));
	CHECK(!RendersEchoFor(MODE_CLASSIC, false));
}

static void
TestQueueEcho(void)
{
	EchoQueue q = EchoQueueFor(true, false, true);
	CHECK(q.flag && !q.copies);
	q = EchoQueueFor(true, false, false);
	CHECK(!q.flag && q.copies);
	q = EchoQueueFor(true, true, true);
	CHECK(q.flag && !q.copies);
	q = EchoQueueFor(true, true, false);
	CHECK(!q.flag && q.copies);
	q = EchoQueueFor(false, true, true);
	CHECK(q.flag && !q.copies);
	q = EchoQueueFor(false, true, false);
	CHECK(q.flag && !q.copies);
	q = EchoQueueFor(false, false, true);
	CHECK(!q.flag && !q.copies);
	q = EchoQueueFor(false, false, false);
	CHECK(!q.flag && !q.copies);
}

static void
TestDelay(void)
{
	CHECK(EchoDelayFrames(34.33f, 48000) == 9600);
	CHECK(EchoDelayFrames(20, 48000) == 5593);
	CHECK(EchoDelayFrames(20, 44100) == 5138);
	CHECK(EchoDelayFrames(0, 48000) == 0);
	CHECK(EchoDelayFrames(-1, 48000) == 0);
	CHECK(EchoDelayFrames(NAN, 48000) == 0);
}

static void
TestDirection(void)
{
	const float N[3] = { 0, 1, 0 }, E[3] = { 1, 0, 0 }, Z[3] = { 0, 0, 1 };
	const float R[3] = { 1, 0, 0 }, Rneg[3] = { 0, -1, 0 };
	Direction d;

	d = EchoTapDirection(0, R, N, Z);
	CHECK(Near(d.azimuth, 0, 1e-4f) && Near(d.elevation, 0, 1e-4f));
	d = EchoTapDirection(3, R, N, Z);
	CHECK(Near(d.azimuth, 90, 1e-3f) && Near(d.elevation, 0, 1e-4f));
	d = EchoTapDirection(2, R, N, Z);
	CHECK(Near(d.azimuth, -90, 1e-3f));
	d = EchoTapDirection(1, R, N, Z);
	CHECK(Near(d.azimuth, 180, 1e-3f));
	for(int t = 4; t < kEchoTaps; t++){
		d = EchoTapDirection(t, R, N, Z);
		CHECK(Near(d.elevation, 90, 1e-3f));
	}

	d = EchoTapDirection(3, Rneg, E, Z);
	CHECK(Near(d.azimuth, 0, 1e-4f));
	d = EchoTapDirection(0, Rneg, E, Z);
	CHECK(Near(d.azimuth, -90, 1e-3f));
	d = EchoTapDirection(1, Rneg, E, Z);
	CHECK(Near(d.azimuth, 90, 1e-3f));
	d = EchoTapDirection(2, Rneg, E, Z);
	CHECK(Near(d.azimuth, 180, 1e-3f));

	d = EchoTapDirection(-1, R, N, Z);
	CHECK(d.azimuth == 0.0f && d.elevation == 0.0f);
	d = EchoTapDirection(kEchoTaps, R, N, Z);
	CHECK(d.azimuth == 0.0f && d.elevation == 0.0f);
}

static void
TestHead(void)
{
	EchoHead h = { 5593, 5593, 0, 0, false };
	EchoHeadSet(h, 9600);
	CHECK(h.current == 5593 && h.target == 9600 && h.fadeLeft == 2048 && !h.hasPending);

	h = { 5593, 5593, 0, 0, false };
	EchoHeadSet(h, 5594);
	CHECK(h.current == 5593 && h.target == 5594 && h.fadeLeft == 2048);

	h = { 5593, 5593, 0, 0, false };
	EchoHeadSet(h, 5593 - 7);
	CHECK(h.current == 5593 && h.target == 5593 - 7 && h.fadeLeft == 2048);

	h = { 5593, 5593, 0, 0, false };
	EchoHeadSet(h, 5593);
	CHECK(h.current == 5593 && h.target == 5593 && h.fadeLeft == 0 && !h.hasPending);

	h = { 5593, 9600, 1000, 0, false };
	EchoHeadSet(h, 9605);
	CHECK(h.current == 5593 && h.target == 9600 && h.fadeLeft == 1000 && h.hasPending && h.pending == 9605);
	EchoHeadSet(h, 7000);
	CHECK(h.current == 5593 && h.target == 9600 && h.fadeLeft == 1000 && h.hasPending && h.pending == 7000);
	EchoHeadSet(h, 9600);
	CHECK(h.current == 5593 && h.target == 9600 && h.fadeLeft == 1000 && !h.hasPending);

	h = { 5593, 9600, 2, 0, false };
	EchoHeadSet(h, 9601);
	EchoHeadStep(h);
	CHECK(h.current == 5593 && h.target == 9600 && h.fadeLeft == 1 && h.hasPending);
	EchoHeadStep(h);
	CHECK(h.current == 9600 && h.target == 9601 && h.fadeLeft == 2048 && !h.hasPending);

	h = { 5593, 9600, 1, 0, false };
	EchoHeadStep(h);
	CHECK(h.current == 9600 && h.target == 9600 && h.fadeLeft == 0 && !h.hasPending);
	EchoHeadStep(h);
	CHECK(h.current == 9600 && h.target == 9600 && h.fadeLeft == 0);

	h = { 5593, 5593, 0, 0, false };
	EchoHeadSet(h, 6000);
	for(int i = 0; i < 100; i++)
		EchoHeadStep(h);
	EchoHeadSet(h, 6100);
	EchoHeadSet(h, 6200);
	int fades = 0;
	for(int i = 0; i < 2 * 2048; i++){
		uint32_t before = h.fadeLeft;
		EchoHeadStep(h);
		if(h.fadeLeft > before)
			fades++;
	}
	CHECK(fades == 1 && h.current == 6200 && h.target == 6200 && h.fadeLeft == 0 && !h.hasPending);

	h = { 5593, 5593, 0, 0, false };
	for(int i = 0; i < 10; i++){
		EchoHeadSet(h, 5593 + 10 * (i + 1));
		for(int k = 0; k < 500; k++)
			EchoHeadStep(h);
	}
	for(int k = 0; k < 3 * 2048; k++)
		EchoHeadStep(h);
	CHECK(h.current == 5593 + 100 && h.target == 5593 + 100 && h.fadeLeft == 0);

	CHECK(Near(EchoFadeWeight(0), 0.0f, 1e-6f));
	CHECK(Near(EchoFadeWeight(kEchoFadeFrames), 1.0f, 1e-6f));
	CHECK(Near(EchoFadeWeight(kEchoFadeFrames / 2), 0.5f, 1e-6f));
	CHECK(Near(EchoFadeWeight(kEchoFadeFrames / 4), 0.5f - 0.5f * cosf(3.14159265f / 4), 1e-5f));
	CHECK(Near(EchoFadeWeight(kEchoFadeFrames + 5), 1.0f, 1e-6f));
	bool table = true;
	for(uint32_t i = 0; i <= kEchoFadeFrames + 3; i++)
		table = table && EchoFadeWeightAt(i) == EchoFadeWeight(i);
	CHECK(table);
}

int
main(void)
{
	TestEligible();
	TestGain();
	TestTapVolume();
	TestDelay();
	TestRendersEcho();
	TestQueueEcho();
	TestDirection();
	TestHead();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all audio_echo tests passed\n");
	return 0;
}
