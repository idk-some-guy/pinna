#pragma once

#include <stdint.h>
#include <math.h>
#include "CaConfig.h"
#include "CaRules.h"

namespace ca {

static const uint32_t kModeDipFrames = 1024;

enum
{
	kDipIdle = 0,
	kDipOut,
	kDipIn
};

struct Direction
{
	float azimuth, elevation;
};

struct DipState
{
	int32_t phase;
	uint32_t frame;
};

inline Direction
SpatialDirection(float gx, float gy, float gz)
{
	Direction d = { 0.0f, 0.0f };
	if(!isfinite(gx) || !isfinite(gy) || !isfinite(gz))
		return d;
	const float deg = 180.0f / 3.14159265358979f;
	// 0 - gx is never -0, so straight behind stays at +180.
	d.azimuth = atan2f(0.0f - gx, gy) * deg;
	d.elevation = atan2f(gz, hypotf(gx, gy)) * deg;
	return d;
}

inline bool
Is2DVoice(float gx, float gy, float gz, float maxDist)
{
	if(!isfinite(gx) || !isfinite(gy) || !isfinite(gz) || !isfinite(maxDist))
		return true;
	return maxDist == 100000.0f && gy == 0.0f && gz == 0.0f;
}

inline float
SpatialVoiceGain(float vol, float dist, float ref, float maxDist, float calib)
{
	if(!isfinite(vol) || !isfinite(dist) || !isfinite(ref) || !isfinite(maxDist) || !isfinite(calib))
		return 0.0f;
	return vol * OalInverseClampedGain(dist, ref, maxDist) * calib;
}

inline void
ModeDipSettle(DipState &s)
{
	if(s.phase == kDipOut && s.frame >= kModeDipFrames){
		s.phase = kDipIn;
		s.frame -= kModeDipFrames;
	}
	if(s.phase == kDipIn && s.frame >= kModeDipFrames){
		s.phase = kDipIdle;
		s.frame = 0;
	}
}

inline float
ModeDipLevel(const DipState &s)
{
	float c = cosf(3.14159265358979f * (float)s.frame / (float)kModeDipFrames);
	if(s.phase == kDipOut)
		return 0.5f + 0.5f * c;
	if(s.phase == kDipIn)
		return 0.5f - 0.5f * c;
	return 1.0f;
}

inline void
ModeDipStart(DipState &s)
{
	if(s.phase == kDipIn){
		s.phase = kDipOut;
		s.frame = kModeDipFrames - s.frame;
	}else if(s.phase != kDipOut){
		s.phase = kDipOut;
		s.frame = 0;
	}
}

inline float
ModeDipGain(DipState &s, uint32_t frames)
{
	ModeDipSettle(s);
	if(s.phase == kDipIdle)
		return 1.0f;
	float g = ModeDipLevel(s);
	s.frame += frames < 2 * kModeDipFrames ? frames : 2 * kModeDipFrames;
	ModeDipSettle(s);
	return g;
}

static const float kSpatialCalibHeadphones = 0.8128f;
static const float kSpatialCalibBuiltInSpeakers = 0.8442f;
static const float kSpatialCalibMultichannel = 0.8424f;
static const uint32_t kJumpRampFrames = 256;

inline bool
OutputRoutesSpatial(int32_t type)
{
	return type == OUTPUT_HEADPHONES || type == OUTPUT_BUILTIN || type == OUTPUT_MULTICHANNEL;
}

inline float
SpatialCalibFor(int32_t type)
{
	return type == OUTPUT_BUILTIN ? kSpatialCalibBuiltInSpeakers : type == OUTPUT_MULTICHANNEL ? kSpatialCalibMultichannel : kSpatialCalibHeadphones;
}

enum
{
	kSpatialUseOutputType = 0,
	kSpatialHRTFHQ
};

enum
{
	kJumpIdle = 0,
	kJumpDown,
	kJumpWait,
	kJumpHold,
	kJumpUp
};

struct SpatialLimits
{
	float azPerSecond, elPerSecond, azPerUpdate, elPerUpdate, jumpDegrees, jumpCap;
	uint32_t holdFrames;
};

struct VoiceDirection
{
	float azimuth, elevation;
	float targetAz, targetEl;
	int32_t phase;
	uint32_t frame;
	uint32_t elapsed;
};

inline SpatialLimits
SpatialLimitsFor(int32_t algorithm)
{
	if(algorithm == kSpatialHRTFHQ){
		SpatialLimits l = { 0.0f, 0.0f, 20.0f, 5.0f, 120.0f, 170.0f, 0 };
		return l;
	}
	SpatialLimits l = { 900.0f, 450.0f, 0.0f, 0.0f, 45.0f, 150.0f, 1024 };
	return l;
}

inline float
JumpThreshold(const SpatialLimits &l, uint32_t elapsed, float rate)
{
	if(elapsed == 0 || !(rate > 0.0f) || !isfinite(rate))
		return l.jumpDegrees;
	float t = l.jumpDegrees * ((float)elapsed * 48000.0f / (512.0f * rate));
	return t < l.jumpDegrees ? l.jumpDegrees : t > l.jumpCap ? l.jumpCap : t;
}

inline uint32_t
SpatialChunkFrames(float rate)
{
	if(!(rate > 0.0f) || !isfinite(rate))
		return 512;
	float n = rate * 512.0f / 48000.0f;
	return n < 64.0f ? 64 : n > (float)kMaxBlock ? (uint32_t)kMaxBlock : (uint32_t)n;
}

inline float
WrapAzimuth(float a)
{
	a = fmodf(a, 360.0f);
	if(a > 180.0f)
		a -= 360.0f;
	else if(a <= -180.0f)
		a += 360.0f;
	return a;
}

inline float
AngleBetween(Direction a, Direction b)
{
	const double rad = 3.14159265358979 / 180.0;
	double ax = cos(a.elevation * rad) * cos(a.azimuth * rad), ay = cos(a.elevation * rad) * sin(a.azimuth * rad), az = sin(a.elevation * rad);
	double bx = cos(b.elevation * rad) * cos(b.azimuth * rad), by = cos(b.elevation * rad) * sin(b.azimuth * rad), bz = sin(b.elevation * rad);
	double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
	return (float)(atan2(sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz) / rad);
}

inline void
VoiceDirectionStart(VoiceDirection &v, Direction target)
{
	if(!isfinite(target.azimuth) || !isfinite(target.elevation))
		target.azimuth = target.elevation = 0.0f;
	v.azimuth = v.targetAz = target.azimuth;
	v.elevation = v.targetEl = target.elevation;
	v.phase = kJumpIdle;
	v.frame = 0;
	v.elapsed = 0;
}

inline float
SlewToward(float from, float to, float limit)
{
	float d = to - from;
	return from + (d > limit ? limit : d < -limit ? -limit : d);
}

inline bool
VoiceDirectionTarget(VoiceDirection &v, Direction target, int32_t algorithm, float rate)
{
	if(!isfinite(target.azimuth) || !isfinite(target.elevation) || !(rate > 0.0f) || !isfinite(rate))
		return false;
	SpatialLimits l = SpatialLimitsFor(algorithm);
	Direction last = { v.targetAz, v.targetEl };
	bool jump = AngleBetween(last, target) > JumpThreshold(l, v.elapsed, rate);
	v.elapsed = 0;
	v.targetAz = target.azimuth;
	v.targetEl = target.elevation;
	if(jump){
		if(v.phase == kJumpIdle){
			v.phase = kJumpDown;
			v.frame = 0;
		}else if(v.phase == kJumpUp){
			v.phase = kJumpDown;
			v.frame = kJumpRampFrames - v.frame;
		}else if(v.phase == kJumpHold)
			v.phase = kJumpWait;
	}
	return true;
}

// Returns false and leaves gain alone when every frame of the step has gain 1.
inline bool
VoiceDirectionStep(VoiceDirection &v, int32_t algorithm, uint32_t frames, float rate, float *gain)
{
	if(!(rate > 0.0f) || !isfinite(rate))
		return false;
	SpatialLimits l = SpatialLimitsFor(algorithm);
	v.elapsed = frames < 0x40000000u - v.elapsed ? v.elapsed + frames : 0x40000000u;
	if(v.phase == kJumpWait){
		v.azimuth = v.targetAz;
		v.elevation = v.targetEl;
		v.phase = l.holdFrames ? kJumpHold : kJumpUp;
		v.frame = 0;
	}else if(v.phase != kJumpDown){
		float az = l.azPerSecond > 0.0f ? l.azPerSecond * (float)frames / rate : l.azPerUpdate;
		float el = l.elPerSecond > 0.0f ? l.elPerSecond * (float)frames / rate : l.elPerUpdate;
		v.azimuth = WrapAzimuth(SlewToward(0.0f, WrapAzimuth(v.targetAz - v.azimuth), az) + v.azimuth);
		v.elevation = SlewToward(v.elevation, v.targetEl, el);
	}
	if(v.phase == kJumpIdle)
		return false;
	const float pi = 3.14159265358979f;
	for(uint32_t i = 0; i < frames; i++){
		float g = 1.0f;
		switch(v.phase){
		case kJumpDown:
			g = 0.5f + 0.5f * cosf(pi * (float)v.frame / (float)kJumpRampFrames);
			if(++v.frame >= kJumpRampFrames){
				v.phase = kJumpWait;
				v.frame = 0;
			}
			break;
		case kJumpWait:
			g = 0.0f;
			break;
		case kJumpHold:
			g = 0.0f;
			if(++v.frame >= l.holdFrames){
				v.phase = kJumpUp;
				v.frame = 0;
			}
			break;
		case kJumpUp:
			g = 0.5f - 0.5f * cosf(pi * (float)v.frame / (float)kJumpRampFrames);
			if(++v.frame >= kJumpRampFrames){
				v.phase = kJumpIdle;
				v.frame = 0;
			}
			break;
		}
		gain[i] = g;
	}
	return true;
}

inline bool
VoiceDirectionAdvance(VoiceDirection &v, Direction target, int32_t algorithm, uint32_t frames, float rate, float *gain)
{
	return VoiceDirectionTarget(v, target, algorithm, rate) && VoiceDirectionStep(v, algorithm, frames, rate, gain);
}

// Fresh is true when the game published since the last block, so elapsed counts from the previous publish; stepped in chunks, dirs gets each chunk's direction. With dipped set, gain covers every frame.
inline uint32_t
VoiceDirectionBlock(VoiceDirection &v, Direction target, bool start, bool fresh, int32_t algorithm, uint32_t frames, float rate, Direction *dirs, float *gain, bool *dipped)
{
	if(start)
		VoiceDirectionStart(v, target);
	else if(fresh)
		VoiceDirectionTarget(v, target, algorithm, rate);
	uint32_t chunk = SpatialChunkFrames(rate), k = 0;
	*dipped = false;
	for(uint32_t done = 0; done < frames; done += chunk, k++){
		uint32_t n = frames - done < chunk ? frames - done : chunk;
		bool d = VoiceDirectionStep(v, algorithm, n, rate, gain + done);
		if(d && !*dipped){
			for(uint32_t i = 0; i < done; i++)
				gain[i] = 1.0f;
			*dipped = true;
		}else if(!d && *dipped)
			for(uint32_t i = 0; i < n; i++)
				gain[done + i] = 1.0f;
		dirs[k].azimuth = v.azimuth;
		dirs[k].elevation = v.elevation;
	}
	return k;
}

inline int32_t
ModeDipFill(DipState &s, float *gain, uint32_t frames)
{
	int32_t switchAt = -1;
	for(uint32_t i = 0; i < frames; i++){
		int32_t before = s.phase;
		ModeDipSettle(s);
		if(before == kDipOut && s.phase != kDipOut && switchAt < 0)
			switchAt = (int32_t)i;
		gain[i] = ModeDipLevel(s);
		if(s.phase != kDipIdle)
			s.frame++;
	}
	return switchAt;
}

}
