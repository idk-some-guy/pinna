// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_spatial_test.cpp -o build/tests/audio_spatial_test && build/tests/audio_spatial_test
#include <cstdio>
#include <cmath>
#include <vector>
#include "CaSpatial.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static bool
Dir(float x, float y, float z, float az, float el)
{
	Direction d = SpatialDirection(x, y, z);
	return Near(d.azimuth, az, 1e-4f) && Near(d.elevation, el, 1e-4f);
}

// The spike measured left minus right ILD of +5.5 dB at azimuth -90 and -5.4 dB at +90: the mixer takes positive azimuth as right.
static void
TestDirection(void)
{
	CHECK(Dir(0.0f, 10.0f, 0.0f, 0.0f, 0.0f));
	CHECK(Dir(0.0f, -10.0f, 0.0f, 180.0f, 0.0f));
	CHECK(Dir(-0.0f, -10.0f, 0.0f, 180.0f, 0.0f));
	CHECK(Dir(10.0f, 0.0f, 0.0f, -90.0f, 0.0f));
	CHECK(Dir(-10.0f, 0.0f, 0.0f, 90.0f, 0.0f));
	CHECK(Dir(-5.0f, 5.0f, 0.0f, 45.0f, 0.0f));
	CHECK(Dir(0.0f, 10.0f, 10.0f, 0.0f, 45.0f));
	CHECK(Dir(0.0f, 0.0f, 10.0f, 0.0f, 90.0f));
	CHECK(Dir(0.0f, 0.0f, -10.0f, 0.0f, -90.0f));
	CHECK(Dir(0.0f, 0.0f, 0.0f, 0.0f, 0.0f));

	for(int i = 0; i < 64; i++){
		float a = (float)i * 0.1f;
		Direction d = SpatialDirection(30.0f * cosf(a * 5.0f), 30.0f * sinf(a * 7.0f), 20.0f * sinf(a * 3.0f));
		CHECK(d.azimuth > -180.0f && d.azimuth <= 180.0f);
		CHECK(d.elevation >= -90.0f && d.elevation <= 90.0f);
	}
}

static void
TestDirectionNotFinite(void)
{
	float bad[] = { NAN, INFINITY, -INFINITY };
	for(float b : bad){
		CHECK(Dir(b, 10.0f, 0.0f, 0.0f, 0.0f));
		CHECK(Dir(-10.0f, b, 0.0f, 0.0f, 0.0f));
		CHECK(Dir(-10.0f, 0.0f, b, 0.0f, 0.0f));
	}
}

static void
Test2DVoice(void)
{
	CHECK(Is2DVoice(1000.0f, 0.0f, 0.0f, 100000.0f));
	CHECK(Is2DVoice(0.0f, 0.0f, 0.0f, 100000.0f));
	CHECK(Is2DVoice(-1000.0f, 0.0f, 0.0f, 100000.0f));
	CHECK(!Is2DVoice(5.0f, 0.0f, 0.0f, 120.0f));
	CHECK(!Is2DVoice(0.0f, 0.001f, 0.0f, 100000.0f));
	CHECK(!Is2DVoice(0.0f, 0.0f, 0.001f, 100000.0f));
	CHECK(!Is2DVoice(1000.0f, 0.0f, 0.0f, 99999.0f));

	CHECK(Is2DVoice(0.0f, NAN, 0.0f, 100000.0f));
	CHECK(Is2DVoice(0.0f, 0.0f, INFINITY, 120.0f));
	CHECK(Is2DVoice(5.0f, 3.0f, 0.0f, NAN));
	CHECK(Is2DVoice(-INFINITY, 3.0f, 2.0f, 120.0f));
}

static void
TestVoiceGain(void)
{
	CHECK(Near(SpatialVoiceGain(1.0f, 40.0f, 10.0f, 40.0f, 1.0f), 0.25f, 1e-4f));
	CHECK(Near(SpatialVoiceGain(1.0f, 5.0f, 10.0f, 40.0f, 1.0f), 1.0f, 1e-4f));
	CHECK(Near(SpatialVoiceGain(0.5f, 80.0f, 10.0f, 40.0f, 2.0f), 0.25f, 1e-4f));

	float bad[] = { NAN, INFINITY, -INFINITY };
	for(float b : bad){
		CHECK(SpatialVoiceGain(b, 20.0f, 10.0f, 40.0f, 1.0f) == 0.0f);
		CHECK(SpatialVoiceGain(1.0f, b, 10.0f, 40.0f, 1.0f) == 0.0f);
		CHECK(SpatialVoiceGain(1.0f, 20.0f, b, 40.0f, 1.0f) == 0.0f);
		CHECK(SpatialVoiceGain(1.0f, 20.0f, 10.0f, b, 1.0f) == 0.0f);
		CHECK(SpatialVoiceGain(1.0f, 20.0f, 10.0f, 40.0f, b) == 0.0f);
	}
}

static std::vector<float>
DipByFrame(int *switchAt)
{
	std::vector<float> g;
	DipState s = { 0, 0 };
	ModeDipStart(s);
	*switchAt = -1;
	for(int f = 0; f < 2100; f++){
		int32_t before = s.phase;
		g.push_back(ModeDipGain(s, 1));
		if(before == kDipOut && s.phase != kDipOut && *switchAt < 0)
			*switchAt = f + 1;
	}
	return g;
}

static std::vector<float>
DipByBlock(uint32_t block, int *switchAt)
{
	std::vector<float> g(2100 + block);
	DipState s = { 0, 0 };
	ModeDipStart(s);
	*switchAt = -1;
	for(uint32_t at = 0; at < 2100; at += block){
		int32_t k = ModeDipFill(s, &g[at], block);
		if(k >= 0 && *switchAt < 0)
			*switchAt = (int)(at + k);
	}
	g.resize(2100);
	return g;
}

static void
TestDip(void)
{
	DipState idle = { kDipIdle, 0 };
	CHECK(ModeDipGain(idle, 512) == 1.0f);
	CHECK(idle.phase == kDipIdle && idle.frame == 0);

	int switchAt;
	std::vector<float> g = DipByFrame(&switchAt);
	CHECK(g[0] == 1.0f);
	CHECK(Near(g[256], 0.853553f, 1e-4f));
	CHECK(Near(g[512], 0.5f, 1e-4f));
	CHECK(Near(g[1023], 0.0f, 1e-5f));
	CHECK(switchAt == kModeDipFrames);
	CHECK(Near(g[1024], 0.0f, 1e-6f));
	CHECK(Near(g[1024 + 512], 0.5f, 1e-4f));
	CHECK(g[2048] == 1.0f);
	CHECK(g[2099] == 1.0f);
	for(int f = 1; f < 2048; f++)
		CHECK(fabsf(g[f] - g[f - 1]) < 0.0016f);

	DipState s = { 0, 0 };
	ModeDipStart(s);
	for(int f = 0; f < 2048; f++)
		ModeDipGain(s, 1);
	CHECK(s.phase == kDipIdle);

	uint32_t blocks[] = { 1, 341, 4096 };
	for(uint32_t b : blocks){
		int at;
		std::vector<float> h = DipByBlock(b, &at);
		CHECK(at == kModeDipFrames);
		bool same = true;
		for(int f = 0; f < 2100; f++)
			same = same && h[f] == g[f];
		CHECK(same);
	}

	DipState a = { 0, 0 };
	ModeDipStart(a);
	CHECK(ModeDipGain(a, 300) == 1.0f);
	CHECK(Near(ModeDipGain(a, 1024), 0.5f + 0.5f * cosf(3.14159265f * 300.0f / 1024.0f), 1e-6f));
	CHECK(a.phase == kDipIn && a.frame == 300);
	ModeDipGain(a, 5000);
	CHECK(a.phase == kDipIdle);
}

// A second request while a dip runs: during the out half nothing changes and the caller switches once, to the latest mode, when the out half ends; during the in half the dip turns back into an out half from the same gain.
static void
TestDipRequestDuringDip(void)
{
	DipState s = { 0, 0 };
	ModeDipStart(s);
	for(int f = 0; f < 400; f++)
		ModeDipGain(s, 1);
	DipState kept = s;
	ModeDipStart(s);
	CHECK(s.phase == kept.phase && s.frame == kept.frame);
	int switches = 0;
	for(int f = 400; f < 2048; f++){
		int32_t before = s.phase;
		ModeDipGain(s, 1);
		if(before == kDipOut && s.phase != kDipOut)
			switches++;
	}
	CHECK(switches == 1);
	CHECK(s.phase == kDipIdle);

	DipState t = { 0, 0 };
	ModeDipStart(t);
	for(int f = 0; f < 1024 + 300; f++)
		ModeDipGain(t, 1);
	float last = ModeDipGain(t, 0);
	ModeDipStart(t);
	CHECK(t.phase == kDipOut);
	CHECK(Near(ModeDipGain(t, 0), last, 1e-6f));
	std::vector<float> g(2048);
	CHECK(ModeDipFill(t, g.data(), 400) == 300);
	CHECK(Near(g[0], last, 1e-6f));
	CHECK(g[0] < 0.5f && g[299] < g[0]);
	CHECK(Near(g[300], 0.0f, 1e-6f));
	CHECK(t.phase == kDipIn);

	DipState u = { 0, 0 };
	ModeDipStart(u);
	for(int f = 0; f < 1024; f++)
		ModeDipGain(u, 1);
	CHECK(u.phase == kDipIn && u.frame == 0);
	ModeDipStart(u);
	CHECK(u.phase == kDipOut && u.frame == kModeDipFrames);
	CHECK(ModeDipFill(u, g.data(), 4) == 0);
	CHECK(u.phase == kDipIn);
	CHECK(g[0] == 0.0f);
}

static Direction
D(float az, float el)
{
	Direction d = { az, el };
	return d;
}

static bool
At(const VoiceDirection &v, float az, float el)
{
	return Near(v.azimuth, az, 1e-3f) && Near(v.elevation, el, 1e-3f);
}

static bool
AllEqual(const std::vector<float> &g, uint32_t from, uint32_t to, float value)
{
	for(uint32_t i = from; i < to; i++)
		if(g[i] != value)
			return false;
	return true;
}

static void
TestCalibAndLimits(void)
{
	CHECK(kSpatialCalibHeadphones == 0.8128f);
	CHECK(kSpatialCalibBuiltInSpeakers == 0.8442f);
	SpatialLimits u = SpatialLimitsFor(kSpatialUseOutputType);
	CHECK(u.azPerSecond == 900.0f && u.elPerSecond == 450.0f && u.jumpDegrees == 45.0f && u.holdFrames == 1024);
	SpatialLimits h = SpatialLimitsFor(kSpatialHRTFHQ);
	CHECK(h.azPerUpdate == 20.0f && h.elPerUpdate == 5.0f && h.jumpDegrees == 120.0f && h.holdFrames == 0);
	SpatialLimits other = SpatialLimitsFor(7);
	CHECK(other.azPerSecond == 900.0f && other.jumpDegrees == 45.0f);
	CHECK(kJumpRampFrames == 256);
}

static void
TestOutputRouting(void)
{
	CHECK(OutputRoutesSpatial(OUTPUT_HEADPHONES) && OutputRoutesSpatial(OUTPUT_BUILTIN) && OutputRoutesSpatial(OUTPUT_MULTICHANNEL));
	CHECK(!OutputRoutesSpatial(OUTPUT_EXTERNAL) && !OutputRoutesSpatial(OUTPUT_AUTO) && !OutputRoutesSpatial(-1));
	CHECK(SpatialCalibFor(OUTPUT_HEADPHONES) == 0.8128f && SpatialCalibFor(OUTPUT_BUILTIN) == 0.8442f);
	CHECK(SpatialCalibFor(OUTPUT_EXTERNAL) == 0.8128f && SpatialCalibFor(99) == 0.8128f);
	CHECK(kSpatialCalibMultichannel == 0.8424f && SpatialCalibFor(OUTPUT_MULTICHANNEL) == 0.8424f);
}

static void
TestAngleBetween(void)
{
	CHECK(Near(AngleBetween(D(0, 0), D(90, 0)), 90.0f, 1e-3f));
	CHECK(Near(AngleBetween(D(170, 0), D(-170, 0)), 20.0f, 1e-3f));
	CHECK(Near(AngleBetween(D(0, 89), D(180, 89)), 2.0f, 1e-2f));
	CHECK(Near(AngleBetween(D(0, -30), D(0, 30)), 60.0f, 1e-3f));
	CHECK(Near(AngleBetween(D(10, 20), D(10, 20)), 0.0f, 1e-3f));
}

static void
TestVoiceStart(void)
{
	std::vector<float> g(4096, 7.0f);
	VoiceDirection v;
	VoiceDirectionStart(v, D(120, 30));
	CHECK(At(v, 120, 30) && v.phase == kJumpIdle);
	CHECK(!VoiceDirectionAdvance(v, D(120, 30), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 120, 30));
	CHECK(g[0] == 7.0f);
	VoiceDirectionStart(v, D(NAN, 0));
	CHECK(At(v, 0, 0));
}

static void
TestSlew(void)
{
	std::vector<float> g(4096);
	VoiceDirection v;
	VoiceDirectionStart(v, D(0, 0));
	CHECK(!VoiceDirectionAdvance(v, D(30, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 9.6f, 0));
	VoiceDirectionAdvance(v, D(30, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, 19.2f, 0));
	VoiceDirectionAdvance(v, D(30, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	VoiceDirectionAdvance(v, D(30, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, 30, 0));

	VoiceDirectionStart(v, D(0, 0));
	VoiceDirectionAdvance(v, D(0, 20), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, 0, 4.8f));
	VoiceDirectionStart(v, D(0, 0));
	VoiceDirectionAdvance(v, D(-40, 0), kSpatialUseOutputType, 1024, 48000.0f, g.data());
	CHECK(At(v, -19.2f, 0));
	VoiceDirectionStart(v, D(0, 0));
	VoiceDirectionAdvance(v, D(40, 0), kSpatialUseOutputType, 512, 44100.0f, g.data());
	CHECK(At(v, 900.0f * 512.0f / 44100.0f, 0));

	VoiceDirectionStart(v, D(0, 0));
	CHECK(!VoiceDirectionAdvance(v, D(30, 0), kSpatialHRTFHQ, 512, 48000.0f, g.data()));
	CHECK(At(v, 20, 0));
	VoiceDirectionAdvance(v, D(30, 0), kSpatialHRTFHQ, 4096, 48000.0f, g.data());
	CHECK(At(v, 30, 0));
	VoiceDirectionStart(v, D(0, 0));
	VoiceDirectionAdvance(v, D(0, -20), kSpatialHRTFHQ, 4096, 48000.0f, g.data());
	CHECK(At(v, 0, -5));
	VoiceDirectionAdvance(v, D(0, -20), kSpatialHRTFHQ, 64, 48000.0f, g.data());
	CHECK(At(v, 0, -10));
}

static void
TestSlewShorterWay(void)
{
	std::vector<float> g(512);
	VoiceDirection v;
	VoiceDirectionStart(v, D(170, 0));
	CHECK(!VoiceDirectionAdvance(v, D(-170, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 179.6f, 0));
	VoiceDirectionAdvance(v, D(-170, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, -170.8f, 0));
	VoiceDirectionAdvance(v, D(-170, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, -170, 0));
	VoiceDirectionStart(v, D(-175, 0));
	VoiceDirectionAdvance(v, D(175, 0), kSpatialHRTFHQ, 512, 48000.0f, g.data());
	CHECK(At(v, 175, 0));
	VoiceDirectionStart(v, D(-179.4f, 0));
	VoiceDirectionAdvance(v, D(179, 0), kSpatialUseOutputType, 64, 48000.0f, g.data());
	CHECK(At(v, 179.4f, 0));
	CHECK(v.azimuth > -180.0f && v.azimuth <= 180.0f);
}

static void
TestJumpUseOutputType(void)
{
	std::vector<float> g(512);
	VoiceDirection v;
	VoiceDirectionStart(v, D(0, 0));
	CHECK(!VoiceDirectionAdvance(v, D(45, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 9.6f, 0));

	VoiceDirectionStart(v, D(0, 0));
	CHECK(VoiceDirectionAdvance(v, D(46, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 0, 0));
	CHECK(g[0] == 1.0f);
	CHECK(Near(g[64], 0.853553f, 1e-5f));
	CHECK(Near(g[128], 0.5f, 1e-5f));
	CHECK(g[255] > 0.0f && g[255] < 1e-3f);
	CHECK(AllEqual(g, 256, 512, 0.0f));
	CHECK(VoiceDirectionAdvance(v, D(46, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 46, 0));
	CHECK(AllEqual(g, 0, 512, 0.0f));
	CHECK(VoiceDirectionAdvance(v, D(46, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(AllEqual(g, 0, 512, 0.0f));
	CHECK(VoiceDirectionAdvance(v, D(46, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(g[0] == 0.0f);
	CHECK(Near(g[64], 0.146447f, 1e-5f));
	CHECK(Near(g[128], 0.5f, 1e-5f));
	CHECK(g[255] < 1.0f && g[255] > 0.999f);
	CHECK(AllEqual(g, 256, 512, 1.0f));
	CHECK(v.phase == kJumpIdle);
	CHECK(!VoiceDirectionAdvance(v, D(46, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));

	VoiceDirectionStart(v, D(0, -20));
	CHECK(VoiceDirectionAdvance(v, D(0, 30), kSpatialUseOutputType, 512, 48000.0f, g.data()));
}

static void
TestJumpHRTFHQ(void)
{
	std::vector<float> g(512);
	VoiceDirection v;
	VoiceDirectionStart(v, D(0, 0));
	CHECK(!VoiceDirectionAdvance(v, D(120, 0), kSpatialHRTFHQ, 512, 48000.0f, g.data()));
	CHECK(At(v, 20, 0));

	VoiceDirectionStart(v, D(0, 0));
	CHECK(VoiceDirectionAdvance(v, D(121, 0), kSpatialHRTFHQ, 256, 48000.0f, g.data()));
	CHECK(At(v, 0, 0));
	CHECK(g[0] == 1.0f && g[255] > 0.0f && g[255] < 1e-3f);
	CHECK(VoiceDirectionAdvance(v, D(121, 0), kSpatialHRTFHQ, 256, 48000.0f, g.data()));
	CHECK(At(v, 121, 0));
	CHECK(g[0] == 0.0f && Near(g[128], 0.5f, 1e-5f) && g[255] > 0.999f);
	CHECK(v.phase == kJumpIdle);
}

static void
TestJumpDuringDip(void)
{
	std::vector<float> g(512);
	VoiceDirection v;
	VoiceDirectionStart(v, D(0, 0));
	VoiceDirectionAdvance(v, D(90, 0), kSpatialUseOutputType, 128, 48000.0f, g.data());
	VoiceDirectionAdvance(v, D(-90, 0), kSpatialUseOutputType, 128, 48000.0f, g.data());
	CHECK(At(v, 0, 0) && v.phase == kJumpWait);
	VoiceDirectionAdvance(v, D(-90, 0), kSpatialUseOutputType, 128, 48000.0f, g.data());
	CHECK(At(v, -90, 0) && v.phase == kJumpHold);
	CHECK(AllEqual(g, 0, 128, 0.0f));

	CHECK(VoiceDirectionAdvance(v, D(90, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 90, 0) && v.phase == kJumpHold && v.frame == 512);
	CHECK(AllEqual(g, 0, 512, 0.0f));
	VoiceDirectionAdvance(v, D(90, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(AllEqual(g, 0, 512, 0.0f));
	VoiceDirectionAdvance(v, D(90, 0), kSpatialUseOutputType, 100, 48000.0f, g.data());
	CHECK(v.phase == kJumpUp);
	float last = g[99];
	CHECK(last > 0.1f && last < 0.5f);
	CHECK(VoiceDirectionAdvance(v, D(-60, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(Near(g[0], last, 0.02f));
	CHECK(At(v, 90, 0));
	CHECK(AllEqual(g, 200, 512, 0.0f));
	VoiceDirectionAdvance(v, D(-60, 0), kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(At(v, -60, 0));
}

static void
TestDirectionNotFiniteHolds(void)
{
	std::vector<float> g(512, 7.0f);
	VoiceDirection v;
	VoiceDirectionStart(v, D(10, 5));
	CHECK(!VoiceDirectionAdvance(v, D(NAN, 0), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 10, 5));
	CHECK(!VoiceDirectionAdvance(v, D(0, INFINITY), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 10, 5));
	CHECK(!VoiceDirectionAdvance(v, D(10, 5), kSpatialUseOutputType, 512, 48000.0f, g.data()));
	CHECK(At(v, 10, 5) && v.phase == kJumpIdle);
	CHECK(!VoiceDirectionAdvance(v, D(10, 5), kSpatialUseOutputType, 512, NAN, g.data()));
	CHECK(At(v, 10, 5));
	CHECK(g[0] == 7.0f);
}

static void
TestJumpThreshold(void)
{
	SpatialLimits u = SpatialLimitsFor(kSpatialUseOutputType);
	SpatialLimits h = SpatialLimitsFor(kSpatialHRTFHQ);
	CHECK(u.jumpCap == 150.0f && h.jumpCap == 170.0f);
	CHECK(JumpThreshold(u, 0, 48000.0f) == 45.0f);
	CHECK(JumpThreshold(u, 256, 48000.0f) == 45.0f);
	CHECK(JumpThreshold(u, 512, 48000.0f) == 45.0f);
	CHECK(Near(JumpThreshold(u, 1024, 48000.0f), 90.0f, 1e-3f));
	CHECK(Near(JumpThreshold(u, 1536, 48000.0f), 135.0f, 1e-3f));
	CHECK(JumpThreshold(u, 2048, 48000.0f) == 150.0f);
	CHECK(JumpThreshold(u, 4096, 48000.0f) == 150.0f);
	CHECK(kSpatialBaseBlock == 512 && SpatialChunkFrames(48000.0f) == kSpatialBaseBlock && SpatialChunkFrames(0.0f) == kSpatialBaseBlock);
	CHECK(SpatialChunkFrames(96000.0f) == 2 * kSpatialBaseBlock && SpatialChunkFrames(24000.0f) == kSpatialBaseBlock / 2);
	CHECK(Near(JumpThreshold(u, 2 * kSpatialBaseBlock, 48000.0f), 90.0f, 1e-3f) && Near(JumpThreshold(u, 4 * kSpatialBaseBlock, 96000.0f), 90.0f, 1e-3f));
	CHECK(Near(JumpThreshold(u, 1024, 44100.0f), 45.0f * 1024.0f * 48000.0f / (512.0f * 44100.0f), 1e-3f));
	CHECK(Near(JumpThreshold(u, 1024, 96000.0f), 45.0f, 1e-3f));
	CHECK(JumpThreshold(u, 4096, NAN) == 45.0f);
	CHECK(JumpThreshold(u, 4096, 0.0f) == 45.0f);
	CHECK(JumpThreshold(u, 4096, INFINITY) == 45.0f);
	CHECK(JumpThreshold(h, 512, 48000.0f) == 120.0f);
	CHECK(JumpThreshold(h, 640, 48000.0f) == 150.0f);
	CHECK(JumpThreshold(h, 1024, 48000.0f) == 170.0f);
	CHECK(JumpThreshold(h, 0, 48000.0f) == 120.0f);
}

static void
TestChunkFrames(void)
{
	CHECK(SpatialChunkFrames(48000.0f) == 512);
	CHECK(SpatialChunkFrames(44100.0f) == 470);
	CHECK(SpatialChunkFrames(96000.0f) == 1024);
	CHECK(SpatialChunkFrames(8000.0f) == 85);
	CHECK(SpatialChunkFrames(4000.0f) == 64);
	CHECK(SpatialChunkFrames(NAN) == 512);
	CHECK(SpatialChunkFrames(0.0f) == 512);
	CHECK(SpatialChunkFrames(1e12f) == 4096);
}

static void
TestElapsedCountsStepFrames(void)
{
	std::vector<float> g(4096);
	VoiceDirection v;
	VoiceDirectionStart(v, D(0, 0));
	CHECK(v.elapsed == 0);
	VoiceDirectionStep(v, kSpatialUseOutputType, 300, 48000.0f, g.data());
	VoiceDirectionStep(v, kSpatialUseOutputType, 700, 48000.0f, g.data());
	CHECK(v.elapsed == 1000);
	CHECK(VoiceDirectionTarget(v, D(80, 0), kSpatialUseOutputType, 48000.0f));
	CHECK(v.elapsed == 0 && v.phase == kJumpIdle);
	VoiceDirectionStep(v, kSpatialUseOutputType, 512, 48000.0f, g.data());
	CHECK(VoiceDirectionTarget(v, D(-20, 0), kSpatialUseOutputType, 48000.0f));
	CHECK(v.phase == kJumpDown);
	CHECK(!VoiceDirectionTarget(v, D(NAN, 0), kSpatialUseOutputType, 48000.0f));
	CHECK(v.targetAz == -20.0f);
}

static void
TestElapsedCountsFromPublish(void)
{
	std::vector<float> g(4096);
	Direction dirs[kMaxBlock / 64];
	bool dipped;
	VoiceDirection v;
	VoiceDirectionBlock(v, D(0, 0), true, true, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	for(int k = 0; k < 2; k++)
		VoiceDirectionBlock(v, D(0, 0), false, false, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	CHECK(v.elapsed == 1536);
	VoiceDirectionBlock(v, D(80, 0), false, true, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	CHECK(!dipped && v.phase == kJumpIdle && v.elapsed == 512);
	VoiceDirectionBlock(v, D(80, 0), false, true, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	CHECK(v.elapsed == 512);
	VoiceDirectionBlock(v, D(-40, 0), false, false, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	CHECK(!dipped && v.targetAz == 80.0f && v.elapsed == 1024);
	VoiceDirectionBlock(v, D(-40, 0), false, true, kSpatialUseOutputType, 512, 48000.0f, dirs, g.data(), &dipped);
	CHECK(dipped && v.targetAz == -40.0f);
}

struct BlockRun
{
	std::vector<float> gain, azimuth, elevation;
	float maxStep;
	int dips;
	uint32_t chunks;
};

typedef Direction (*TargetAt)(double seconds);

// With fps set, the target is the caller's last publish before the block, as at 30, 60 or 120 frames per second.
static BlockRun
RunBlocks(TargetAt at, int32_t algorithm, uint32_t block, double seconds, double fps = 0.0)
{
	const float rate = 48000.0f;
	uint32_t total = (uint32_t)(seconds * rate);
	BlockRun r;
	r.gain.assign(total, 1.0f);
	r.azimuth.assign(total, 0.0f);
	r.elevation.assign(total, 0.0f);
	r.maxStep = 0.0f;
	r.dips = 0;
	r.chunks = 0;
	VoiceDirection v;
	std::vector<float> g(block);
	Direction dirs[kMaxBlock / 64];
	Direction last = at(0.0);
	int64_t published = -1;
	for(uint32_t done = 0; done < total; done += block){
		uint32_t n = total - done < block ? total - done : block;
		bool dipped = false;
		double t = done / (double)rate;
		int64_t frame = fps > 0.0 ? (int64_t)floor(t * fps + 1e-9) : (int64_t)done;
		if(fps > 0.0)
			t = frame / fps;
		uint32_t k = VoiceDirectionBlock(v, at(t), done == 0, frame != published, algorithm, n, rate, dirs, g.data(), &dipped);
		published = frame;
		uint32_t chunk = SpatialChunkFrames(rate);
		for(uint32_t i = 0; i < k; i++){
			r.maxStep = fmaxf(r.maxStep, AngleBetween(last, dirs[i]));
			last = dirs[i];
			for(uint32_t f = i * chunk; f < (i + 1) * chunk && f < n; f++){
				r.azimuth[done + f] = dirs[i].azimuth;
				r.elevation[done + f] = dirs[i].elevation;
			}
		}
		r.chunks += k;
		if(dipped)
			for(uint32_t f = 0; f < n; f++)
				r.gain[done + f] = g[f];
	}
	for(uint32_t f = 0; f < total; f++)
		if(r.gain[f] < 1.0f && (f == 0 || r.gain[f - 1] == 1.0f))
			r.dips++;
	return r;
}

static Direction
FastTurn(double t)
{
	return D(WrapAzimuth((float)(t < 0.15 ? 1200.0 * t : 180.0)), 0.0f);
}

static Direction
JumpAt200ms(double t)
{
	return D(t < 0.2 ? 0.0f : 180.0f, 0.0f);
}

static Direction
Circling(double t)
{
	return D(WrapAzimuth((float)(120.0 * t)), (float)(20.0 * sin(3.14159265358979 * t)));
}

static const uint32_t kDeviceBlocks[5] = { 256, 512, 1024, 2048, 4096 };

static void
TestFastTurnNeverDips(void)
{
	for(int a = 0; a < 2; a++)
		for(int b = 0; b < 5; b++){
			BlockRun r = RunBlocks(FastTurn, a ? kSpatialHRTFHQ : kSpatialUseOutputType, kDeviceBlocks[b], 0.5);
			CHECK(r.dips == 0);
			CHECK(Near(r.azimuth.back(), 180.0f, 1e-3f) || Near(r.azimuth.back(), -180.0f, 1e-3f));
		}
}

static void
TestJumpDipsOnce(void)
{
	for(int a = 0; a < 2; a++)
		for(int b = 0; b < 5; b++){
			BlockRun r = RunBlocks(JumpAt200ms, a ? kSpatialHRTFHQ : kSpatialUseOutputType, kDeviceBlocks[b], 0.6);
			CHECK(r.dips == 1);
			CHECK(r.gain.back() == 1.0f && Near(fabsf(r.azimuth.back()), 180.0f, 1e-3f));
		}
}

static const double kFrameRates[3] = { 30.0, 60.0, 120.0 };

static Direction
FasterTurn(double t)
{
	return D(WrapAzimuth((float)(t < 0.1 ? 1800.0 * t : 180.0)), 0.0f);
}

static void
TestFrameRateTurnNeverDips(void)
{
	for(int a = 0; a < 2; a++)
		for(int f = 0; f < 3; f++)
			for(int b = 0; b < 5; b++){
				int32_t alg = a ? kSpatialHRTFHQ : kSpatialUseOutputType;
				BlockRun r = RunBlocks(FastTurn, alg, kDeviceBlocks[b], 0.5, kFrameRates[f]);
				CHECK(r.dips == 0);
				CHECK(Near(fabsf(r.azimuth.back()), 180.0f, 1e-3f));
				BlockRun q = RunBlocks(FasterTurn, alg, kDeviceBlocks[b], 0.5, kFrameRates[f]);
				if(q.dips != 0)
					printf("FAIL 0.1 s turn dips %d times at %.0f fps, %u-frame blocks, algorithm %d\n", q.dips, kFrameRates[f], kDeviceBlocks[b], alg);
				CHECK(q.dips == 0);
			}
}

static void
TestFrameRateJumpDipsOnce(void)
{
	for(int a = 0; a < 2; a++)
		for(int f = 0; f < 3; f++)
			for(int b = 0; b < 5; b++){
				BlockRun r = RunBlocks(JumpAt200ms, a ? kSpatialHRTFHQ : kSpatialUseOutputType, kDeviceBlocks[b], 0.6, kFrameRates[f]);
				CHECK(r.dips == 1);
				CHECK(r.gain.back() == 1.0f && Near(fabsf(r.azimuth.back()), 180.0f, 1e-3f));
			}
}

static void
TestSlewStepWithinCleanStep(void)
{
	for(int b = 0; b < 5; b++){
		BlockRun u = RunBlocks(FastTurn, kSpatialUseOutputType, kDeviceBlocks[b], 0.5);
		CHECK(u.maxStep <= 900.0f * 512.0f / 48000.0f + 1e-3f);
		CHECK(u.chunks == (kDeviceBlocks[b] < 512 ? 24000 / kDeviceBlocks[b] + (24000 % kDeviceBlocks[b] != 0) : 47));
		BlockRun h = RunBlocks(FastTurn, kSpatialHRTFHQ, kDeviceBlocks[b], 0.5);
		CHECK(h.maxStep <= 20.0f + 1e-3f);
	}
}

static void
TestMovingVoiceFollowsAtLargeBlocks(void)
{
	BlockRun ref = RunBlocks(Circling, kSpatialUseOutputType, 512, 3.0);
	CHECK(ref.dips == 0);
	for(int b = 3; b < 5; b++){
		uint32_t block = kDeviceBlocks[b];
		BlockRun r = RunBlocks(Circling, kSpatialUseOutputType, block, 3.0);
		CHECK(r.dips == 0);
		float worst = 0.0f;
		for(size_t f = 0; f < r.azimuth.size(); f++)
			worst = fmaxf(worst, AngleBetween(D(r.azimuth[f], r.elevation[f]), D(ref.azimuth[f], ref.elevation[f])));
		CHECK(worst <= 120.0f * block / 48000.0f);
	}
}

int
main(void)
{
	TestDirection();
	TestDirectionNotFinite();
	Test2DVoice();
	TestVoiceGain();
	TestDip();
	TestDipRequestDuringDip();
	TestCalibAndLimits();
	TestOutputRouting();
	TestAngleBetween();
	TestVoiceStart();
	TestSlew();
	TestSlewShorterWay();
	TestJumpUseOutputType();
	TestJumpHRTFHQ();
	TestJumpDuringDip();
	TestDirectionNotFiniteHolds();
	TestJumpThreshold();
	TestChunkFrames();
	TestElapsedCountsStepFrames();
	TestElapsedCountsFromPublish();
	TestFastTurnNeverDips();
	TestJumpDipsOnce();
	TestFrameRateTurnNeverDips();
	TestFrameRateJumpDipsOnce();
	TestSlewStepWithinCleanStep();
	TestMovingVoiceFollowsAtLargeBlocks();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all audio_spatial tests passed\n");
	return 0;
}
