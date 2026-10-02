#pragma once

#include <stdint.h>
#include <math.h>
#include "CaConfig.h"

namespace ca {

inline float
OalInverseClampedGain(float dist, float ref, float maxDist)
{
	if(!(ref > 0.0f))
		return 1.0f;
	if(maxDist < ref)
		maxDist = ref;
	float d = dist < ref ? ref : dist > maxDist ? maxDist : dist;
	return ref / d;
}

inline uint32_t
ScaledVolume(uint32_t fade, uint32_t vol, uint32_t master)
{
	return (fade * vol * master) >> 14;
}

inline uint32_t
CutsceneVolume(uint32_t vol, bool cutscene, bool finale)
{
	if(!cutscene)
		return vol;
	return finale ? 0 : vol >> 2;
}

inline float
GainFromVolume(uint32_t vol)
{
	return (float)vol / 127.0f;
}

struct LoopRange
{
	uint32_t start, end;
};

inline LoopRange
OalLoopRange(uint32_t startBytes, int32_t endBytes, uint32_t lengthSamples)
{
	int32_t s = (int32_t)(startBytes / 2);
	int32_t e = endBytes / 2;
	LoopRange whole = { 0, lengthSamples };
	if(s == 0 || s == -1 || s < 0 || s >= e || (uint32_t)e > lengthSamples)
		return whole;
	LoopRange r = { (uint32_t)s, (uint32_t)e };
	return r;
}

inline float
ReverbPart(float value, float maxDist, float maxRatio)
{
	return value > 0.5f && value < maxDist ? value / maxDist * maxRatio : 0.0f;
}

inline float
ReverbRatio(const float ceil[4], float north, float south, float west, float east)
{
	float r = 0.0f;
	for(int i = 0; i < 4; i++)
		r += ReverbPart(ceil[i], 10.0f, 1 / 2.f);
	r += ReverbPart((north + south) / 2.f, 4.0f, 1 / 3.f);
	r += ReverbPart((west + east) / 2.f, 4.0f, 1 / 3.f);
	return r < 0.0f ? 0.0f : r > 0.6f ? 0.6f : r;
}

inline float
EfxSendLevel(float ratio)
{
	float level = ratio * 0.8f;
	return level > 1.0f ? 1.0f : level;
}

struct StereoGain
{
	float l, r;
};

inline StereoGain
PoliceBakedPan(uint32_t pan)
{
	int lv = pan <= 63 ? 128 : (int)((127 - (int)pan) / 64.0f * 128);
	int rv = pan >= 63 ? 128 : (int)((int)pan / 64.0f * 128);
	StereoGain g = { lv / 128.0f, rv / 128.0f };
	return g;
}

inline uint32_t
StreamEffectVolume(uint8_t stream, uint32_t vol, uint32_t effects, uint32_t effectsFade)
{
	if(stream == 1 || stream == 2)
		return (128 * vol * effects) >> 14;
	return (effectsFade * vol * effects) >> 14;
}

inline uint32_t
StreamMusicVolume(uint32_t vol, uint32_t music, uint32_t musicFade, float boost)
{
	return (musicFade * vol * (uint32_t)(music * boost + music)) >> 14;
}

inline float
Mp3BoostMult(uint32_t boost, bool userTrack, bool interrupted)
{
	return userTrack && !interrupted ? boost / 64.f : 0.0f;
}

}
