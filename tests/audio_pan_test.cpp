// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_pan_test.cpp -o build/tests/audio_pan_test && build/tests/audio_pan_test
#include <cstdio>
#include <cmath>
#include "CaPan.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static StereoGain
At(float azDeg, float evDeg)
{
	float a = azDeg * 3.14159265f / 180.0f, e = evDeg * 3.14159265f / 180.0f;
	return OalPanGains(-7.0f * sinf(a) * cosf(e), 7.0f * sinf(e), -7.0f * cosf(a) * cosf(e));
}

static bool
Near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void
TestHeader(void)
{
	CHECK(kOalPanWarp == 1.5f);
	CHECK(fabsf(kOalPanCentre[0] - kOalPanCentre[1]) < 1e-5f);
	CHECK(kOalStereoSource[0][0] > 10.0f * kOalStereoSource[0][1]);
	CHECK(kOalStereoSource[1][1] > 10.0f * kOalStereoSource[1][0]);
	CHECK(kOalStreamPan[63][0] > 10.0f * kOalStreamPan[63][1]);
	CHECK(kOalStreamPan[63][3] > 10.0f * kOalStreamPan[63][2]);
	CHECK(OalStreamPanGains(200) == kOalStreamPan[127]);
}

static void
TestModel(void)
{
	StereoGain g = OalPanGains(10.0f, 0.0f, 0.0f);
	CHECK(g.l > g.r && Near(g.l, kOalPanK0 + kOalPanSide, 1e-5f) && Near(g.r, kOalPanK0 - kOalPanSide, 1e-5f));
	g = OalPanGains(-10.0f, 0.0f, 0.0f);
	CHECK(g.r > g.l && Near(g.r, kOalPanK0 + kOalPanSide, 1e-5f));
	g = OalPanGains(0.0f, 0.0f, -5.0f);
	CHECK(Near(g.l, kOalPanK0 + kOalPanFront, 1e-6f) && Near(g.r, kOalPanK0 + kOalPanFront, 1e-6f));
	g = OalPanGains(0.0f, 0.0f, 5.0f);
	CHECK(Near(g.l, kOalPanK0 - kOalPanFront, 1e-6f) && Near(g.r, kOalPanK0 - kOalPanFront, 1e-6f));
	g = OalPanGains(0.0f, 0.0f, 0.0f);
	CHECK(g.l == kOalPanCentre[0] && g.r == kOalPanCentre[1]);
	g = At(40.0f, 0.0f);
	CHECK(Near(g.r, kOalPanK0 + kOalPanSide * 0.8660254f + kOalPanFront * 0.5f, 1e-5f));
	CHECK(Near(g.l, kOalPanK0 - kOalPanSide * 0.8660254f + kOalPanFront * 0.5f, 1e-5f));
}

static void
TestMirror(void)
{
	static const float dirs[][3] = { { 3.0f, 1.0f, -2.0f }, { 1.0f, -4.0f, 0.5f }, { 0.2f, 0.0f, 5.0f }, { 6.0f, 2.0f, 1.0f } };
	for(const float *d : dirs){
		StereoGain a = OalPanGains(d[0], d[1], d[2]), b = OalPanGains(-d[0], d[1], d[2]);
		CHECK(a.l == b.r && a.r == b.l);
	}
}

static void
TestClampEdge(void)
{
	static const float evs[] = { 0.0f, 30.0f, -45.0f };
	for(float ev : evs){
		StereoGain side = At(90.0f, ev), left = At(-90.0f, ev);
		for(float az = 60.5f; az < 90.0f; az += 4.75f){
			StereoGain g = At(az, ev), m = At(-az, ev);
			CHECK(Near(g.l, side.l, 2e-5f) && Near(g.r, side.r, 2e-5f));
			CHECK(Near(m.l, left.l, 2e-5f) && Near(m.r, left.r, 2e-5f));
		}
		StereoGain inside = At(57.0f, ev);
		CHECK(fabsf(inside.r - side.r) > 1e-4f);
	}
}

static void
TestLobes(void)
{
	StereoGain g = At(95.0f, 0.0f);
	CHECK(g.l < -0.005f && g.r > 0.98f);
	g = At(-95.0f, 0.0f);
	CHECK(g.r < -0.005f && g.l > 0.98f);
	g = At(79.0f / 1.5f, 0.0f);
	CHECK(g.r > 1.008f);
	g = At(-79.0f / 1.5f, 0.0f);
	CHECK(g.l > 1.008f);
}

static void
TestPoles(void)
{
	static const float dirs[][3] = { { 0.0f, 5.0f, 0.0f }, { 0.0f, -5.0f, 0.0f }, { -0.0f, 1.0f, -0.0f }, { 0.0f, 1e-3f, 0.0f }, { 1e-30f, 3.0f, -1e-30f } };
	for(const float *d : dirs){
		StereoGain g = OalPanGains(d[0], d[1], d[2]);
		CHECK(std::isfinite(g.l) && std::isfinite(g.r));
		CHECK(Near(g.l, kOalPanK0, 1e-6f) && Near(g.r, kOalPanK0, 1e-6f));
	}
	StereoGain g = At(30.0f, 89.9f);
	CHECK(std::isfinite(g.l) && Near(g.l, kOalPanK0, 2e-3f));
	g = OalPanGains(1e30f, 0.0f, 0.0f);
	CHECK(std::isfinite(g.l) && Near(g.l, kOalPanK0 + kOalPanSide, 1e-5f));
}

static void
TestNearZero(void)
{
	static const float centre[][3] = { { 0.0f, 1e-30f, 0.0f }, { 1e-4f, 0.0f, 0.0f }, { 0.0f, 3.4526e-4f, 0.0f }, { 2e-4f, 2e-4f, -1e-4f }, { 1e-20f, 1e-20f, 1e-20f } };
	for(const float *d : centre){
		StereoGain g = OalPanGains(d[0], d[1], d[2]);
		CHECK(g.l == kOalPanCentre[0] && g.r == kOalPanCentre[1]);
	}
	StereoGain g = OalPanGains(3.4527e-4f, 0.0f, 0.0f);
	CHECK(Near(g.l, kOalPanK0 + kOalPanSide, 1e-5f) && Near(g.r, kOalPanK0 - kOalPanSide, 1e-5f));
	g = OalPanGains(0.0f, 3.4527e-4f, 0.0f);
	CHECK(Near(g.l, kOalPanK0, 1e-6f) && Near(g.r, kOalPanK0, 1e-6f));
}

static void
TestNonFinite(void)
{
	static const float bad[][3] = { { NAN, 1.0f, 0.0f }, { 1.0f, INFINITY, 0.0f }, { 0.0f, 0.0f, -INFINITY }, { INFINITY, INFINITY, INFINITY } };
	for(const float *d : bad){
		StereoGain g = OalPanGains(d[0], d[1], d[2]);
		CHECK(g.l == kOalPanCentre[0] && g.r == kOalPanCentre[1]);
		CHECK(!OalFinitePosition(d[0], d[1], d[2]));
	}
	CHECK(OalFinitePosition(1e30f, -3.0f, 0.0f));
	CHECK(OalFinitePosition(0.0f, 0.0f, 0.0f));
}

static void
TestDistances(void)
{
	static const float good[] = { 5.0f, 0.0f, -0.0f, 1e30f, FLT_MAX };
	static const float bad[] = { -1.0f, -1e-30f, NAN, INFINITY, -INFINITY };
	for(float v : good)
		CHECK(OalDistanceValid(v));
	for(float v : bad)
		CHECK(!OalDistanceValid(v));
}

static void
TestSourceGains(void)
{
	StereoGain p = OalPanGains(3.0f, 4.0f, 0.0f);
	StereoGain g = OalSourceGains(3.0f, 4.0f, 0.0f, 2.0f, 8.0f);
	CHECK(Near(g.l, 0.4f * p.l, 1e-6f) && Near(g.r, 0.4f * p.r, 1e-6f));
	g = OalSourceGains(30.0f, 40.0f, 0.0f, 2.0f, 8.0f);
	CHECK(Near(g.l, 0.25f * p.l, 1e-6f) && Near(g.r, 0.25f * p.r, 1e-6f));
	g = OalSourceGains(0.3f, 0.4f, 0.0f, 2.0f, 8.0f);
	CHECK(g.l == p.l && g.r == p.r);
	g = OalSourceGains(0.0f, 0.0f, 0.0f, 15.0f, 60.0f);
	CHECK(g.l == kOalPanCentre[0] && g.r == kOalPanCentre[1]);
	g = OalSourceGains(1e30f, 1e30f, 1e30f, 2.0f, 8.0f);
	CHECK(std::isfinite(g.l) && std::isfinite(g.r) && fabsf(g.l) <= 0.25f && fabsf(g.r) <= 0.25f);
	g = OalSourceGains(-6.0f, 0.0f, 8.0f, 0.0f, 100.0f);
	p = OalPanGains(-6.0f, 0.0f, 8.0f);
	CHECK(g.l == p.l && g.r == p.r);
}

int
main(void)
{
	TestHeader();
	TestModel();
	TestMirror();
	TestClampEdge();
	TestLobes();
	TestPoles();
	TestNearZero();
	TestNonFinite();
	TestDistances();
	TestSourceGains();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all pan tests passed\n");
	return 0;
}
