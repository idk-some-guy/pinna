#pragma once

#include <stdint.h>
#include <math.h>
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
