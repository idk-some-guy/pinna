#pragma once

#include <stdint.h>
#include <math.h>

namespace pinna {

enum
{
	ENV_OPEN,
	ENV_NARROW_STREET,
	ENV_TUNNEL,
	ENV_GARAGE,
	ENV_INTERIOR,
	NUM_ENVS
};

struct PlaceInput
{
	float north, south, west, east;
	float ceil[4];
	bool enclosed, roomed;
	float lowCeiling;
};

struct EnvPreset
{
	float decay0, decayNyquist, minDelay, maxDelay, gainDb, base, rayScale;
};

static const EnvPreset kEnvPresets[NUM_ENVS] = {
	{ 0.9f, 0.35f, 0.012f, 0.060f, -12.0f, 0.10f, 0.0f },
	{ 1.3f, 0.55f, 0.006f, 0.035f, -9.0f, 0.15f, 0.8f },
	{ 2.91f, 3.783f, 0.015f, 0.037f, -16.6f, 0.35f, 0.8f },
	{ 1.8f, 0.9f, 0.004f, 0.025f, -10.0f, 0.30f, 0.8f },
	{ 0.7f, 0.35f, 0.003f, 0.018f, -8.0f, 0.25f, 0.5f },
};

static const float kEnvFadeSeconds = 1.0f;
static const float kEnvRayMax = 100.0f;
static const float kEnvCeilCovered = 20.0f;
static const float kEnvWallClose = 15.0f;
static const float kEnvTunnelOpen = 30.0f;
static const float kEnvNarrow = 12.0f;
static const int kEnvConfirm = 2;
static const uint32_t kEnvFadePending = 0xFFFFFFFFu;

struct EnvState
{
	int current, candidate, seen;
	float weight[NUM_ENVS], start[NUM_ENVS];
	float alpha0, remain;
	uint32_t fadeLeft;
};

inline int
EnvClampPlace(int place)
{
	return place >= 0 && place < NUM_ENVS ? place : ENV_OPEN;
}

inline float
EnvRay(float d)
{
	return d > 0.0f ? d : kEnvRayMax;
}

inline int
ClassifyPlace(const PlaceInput &p)
{
	if(p.enclosed)
		return ENV_INTERIOR;
	float n = EnvRay(p.north), s = EnvRay(p.south), w = EnvRay(p.west), e = EnvRay(p.east);
	int covered = 0;
	float sum = 0.0f;
	for(int i = 0; i < 4; i++){
		float c = EnvRay(p.ceil[i]);
		if(c < kEnvCeilCovered){
			covered++;
			sum += c;
		}
	}
	float ns = (n + s) / 2.0f, ew = (w + e) / 2.0f;
	if(covered >= 3){
		bool nsClose = n < kEnvWallClose && s < kEnvWallClose;
		bool ewClose = w < kEnvWallClose && e < kEnvWallClose;
		if((nsClose && !ewClose && ew > kEnvTunnelOpen) || (ewClose && !nsClose && ns > kEnvTunnelOpen))
			return ENV_TUNNEL;
		if(sum / (float)covered < p.lowCeiling || p.roomed)
			return ENV_GARAGE;
		return ENV_INTERIOR;
	}
	return (ns < ew ? ns : ew) < kEnvNarrow ? ENV_NARROW_STREET : ENV_OPEN;
}

inline float
EnvAmount(int env, float rayRatio, float reverbAmount)
{
	if(!isfinite(rayRatio) || !isfinite(reverbAmount))
		return 0.0f;
	const EnvPreset &p = kEnvPresets[EnvClampPlace(env)];
	float a = p.base + p.rayScale * rayRatio;
	a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
	return a * reverbAmount;
}

inline uint32_t
EnvFrames(float seconds, float rate)
{
	float n = ceilf(seconds * rate);
	return isfinite(n) && n >= 1.0f && n <= 1e8f ? (uint32_t)n : 1;
}

inline uint32_t
EnvDecayFrames(int env, float rate)
{
	const EnvPreset &p = kEnvPresets[EnvClampPlace(env)];
	return EnvFrames(p.decay0 > p.decayNyquist ? p.decay0 : p.decayNyquist, rate);
}

inline void
EnvSettle(EnvState &s)
{
	for(int k = 0; k < NUM_ENVS; k++)
		s.weight[k] = s.start[k] = k == s.current ? 1.0f : 0.0f;
	s.fadeLeft = 0;
}

inline void
EnvInit(EnvState &s, int place)
{
	place = EnvClampPlace(place);
	s.current = place;
	s.candidate = place;
	s.seen = 0;
	s.alpha0 = s.remain = 0.0f;
	EnvSettle(s);
}

inline bool
EnvObserve(EnvState &s, int place)
{
	place = EnvClampPlace(place);
	if(place == s.current){
		s.candidate = place;
		return false;
	}
	if(place != s.candidate){
		s.candidate = place;
		s.seen = 0;
	}
	if(s.seen < kEnvConfirm)
		s.seen++;
	if(s.seen < kEnvConfirm)
		return false;
	float rest = 0.0f;
	for(int k = 0; k < NUM_ENVS; k++){
		s.start[k] = s.weight[k];
		if(k != place)
			rest += s.weight[k] * s.weight[k];
	}
	s.remain = sqrtf(rest);
	s.alpha0 = atan2f(s.weight[place], s.remain);
	s.current = place;
	s.seen = 0;
	if(s.remain > 1e-6f)
		s.fadeLeft = kEnvFadePending;
	else
		EnvSettle(s);
	return true;
}

inline void
EnvFadeAdvance(EnvState &s, uint32_t frames, float rate, float *weights)
{
	uint32_t total = EnvFrames(kEnvFadeSeconds, rate);
	if(s.fadeLeft > total)
		s.fadeLeft = total;
	uint32_t step = frames < s.fadeLeft ? frames : s.fadeLeft;
	s.fadeLeft -= step;
	if(s.fadeLeft == 0)
		for(int k = 0; k < NUM_ENVS; k++)
			s.weight[k] = k == s.current ? 1.0f : 0.0f;
	else{
		float x = (float)(total - s.fadeLeft) / (float)total;
		float alpha = s.alpha0 + x * (1.57079632679f - s.alpha0);
		float in = sinf(alpha), out = cosf(alpha) / s.remain;
		for(int k = 0; k < NUM_ENVS; k++)
			s.weight[k] = k == s.current ? in : s.start[k] * out;
	}
	for(int k = 0; k < NUM_ENVS; k++)
		weights[k] = s.weight[k];
}

inline uint32_t
EnvLongestDecayFrames(float rate)
{
	uint32_t longest = 1;
	for(int k = 0; k < NUM_ENVS; k++){
		uint32_t n = EnvDecayFrames(k, rate);
		longest = n > longest ? n : longest;
	}
	return longest;
}

inline bool
EnvPullBlock(uint32_t &hold, uint32_t spatialFrames, uint32_t frames, float rate)
{
	if(spatialFrames != 0){
		hold = EnvLongestDecayFrames(rate);
		return true;
	}
	if(hold == 0)
		return false;
	hold = hold > frames ? hold - frames : 0;
	return true;
}

}
