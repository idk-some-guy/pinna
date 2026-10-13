#pragma once

#include <stdint.h>
#include <math.h>
#include "CaConfig.h"
#include "CaRules.h"
#include "CaSpatial.h"

namespace ca {

enum
{
	kEchoTaps = 8,
	kEchoFadeFrames = 2048
};

static const float kSpeedOfSound = 343.3f;

inline uint8_t
EchoComputeVolume(uint8_t emitting, float maxDist, float dist)
{
	if(maxDist <= 0.0f)
		return 0;
	float minDist = maxDist / 5.0f;
	if(minDist > dist)
		return emitting;
	uint8_t v = emitting * (((maxDist - minDist - (dist - minDist)) / (maxDist - minDist)) *
		((maxDist - minDist - (dist - minDist)) / (maxDist - minDist)));
	return v < 127 ? v : 127;
}

inline uint8_t
EchoComputeEmittingVolume(uint8_t emitting, float maxDist, float dist)
{
	float minDist = maxDist / 4.0f;
	float diffDist = maxDist - minDist;
	if(dist > diffDist)
		return (minDist - (dist - diffDist)) * (float)emitting / minDist;
	return emitting;
}

inline bool
EchoTapEligible(float d, float maxDist)
{
	if(!isfinite(d) || !isfinite(maxDist))
		return false;
	if(!(d > 0.0f && d < 100.0f && d < maxDist / 2.0f))
		return false;
	return (uint8_t)(d * 500.0f / 1029.0f) > 3;
}

inline uint32_t
EchoTapVolume(uint32_t volume, float maxDist, float d)
{
	if(volume == 0 || volume > 255 || !EchoTapEligible(d, maxDist))
		return 0;
	uint8_t c = (9 * volume) >> 4;
	if(!(EchoComputeVolume(c, maxDist / 2.0f, d) > (c >> 4)))
		return 0;
	return EchoComputeEmittingVolume(c, maxDist / 2.0f, d);
}

inline uint32_t
EchoTapScaled(uint32_t volume, uint32_t scale, float maxDist, float d)
{
	return EchoTapVolume(volume, maxDist, d) * scale / 127;
}

inline float
EchoTapGain(uint32_t emitting, float srcDist, float maxDist, float d)
{
	if(emitting == 0 || emitting > 255 || !isfinite(srcDist))
		return 0.0f;
	uint32_t faded = EchoTapVolume(EchoComputeVolume((uint8_t)emitting, maxDist, srcDist), maxDist, d);
	return ((float)faded / (float)emitting) * OalInverseClampedGain(100.0f, maxDist / 8.0f, maxDist / 2.0f);
}

inline uint32_t
EchoDelayFrames(float d, float rate)
{
	if(!isfinite(d) || !isfinite(rate) || !(d > 0.0f) || !(rate > 0.0f))
		return 0;
	double frames = 2.0 * (double)d / (double)kSpeedOfSound * (double)rate;
	if(frames >= 4294967295.0)
		return 0xFFFFFFFFu;
	return (uint32_t)(frames + 0.5);
}

inline Direction
EchoTapDirection(int tap, const float camRight[3], const float camForward[3], const float camUp[3])
{
	static const float axes[kEchoTaps][3] = {
		{ 0, 1, 0 }, { 0, -1, 0 }, { -1, 0, 0 }, { 1, 0, 0 },
		{ 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }
	};
	if(tap < 0 || tap >= kEchoTaps)
		return SpatialDirection(0.0f, 0.0f, 0.0f);
	const float *a = axes[tap];
	float r = a[0] * camRight[0] + a[1] * camRight[1] + a[2] * camRight[2];
	float f = a[0] * camForward[0] + a[1] * camForward[1] + a[2] * camForward[2];
	float u = a[0] * camUp[0] + a[1] * camUp[1] + a[2] * camUp[2];
	return SpatialDirection(-r, f, u);
}

struct EchoHead
{
	uint32_t current, target, fadeLeft, pending;
	bool hasPending;
};

inline void
EchoHeadSet(EchoHead &h, uint32_t frames)
{
	if(h.fadeLeft){
		if(frames == h.target)
			h.hasPending = false;
		else{
			h.pending = frames;
			h.hasPending = true;
		}
		return;
	}
	if(frames == h.target)
		return;
	h.current = h.target;
	h.target = frames;
	h.fadeLeft = kEchoFadeFrames;
}

inline void
EchoHeadStep(EchoHead &h)
{
	if(h.fadeLeft == 0 || --h.fadeLeft != 0)
		return;
	h.current = h.target;
	if(h.hasPending && h.pending != h.target){
		h.target = h.pending;
		h.fadeLeft = kEchoFadeFrames;
	}
	h.hasPending = false;
}

inline float
EchoFadeWeight(uint32_t done)
{
	if(done >= kEchoFadeFrames)
		return 1.0f;
	return 0.5f - 0.5f * cosf(3.14159265358979f * (float)done / (float)kEchoFadeFrames);
}

inline const float *
EchoFadeTable(void)
{
	struct Table
	{
		float w[kEchoFadeFrames + 1];
		Table(void)
		{
			for(uint32_t i = 0; i <= kEchoFadeFrames; i++)
				w[i] = EchoFadeWeight(i);
		}
	};
	static const Table t;
	return t.w;
}

inline float
EchoFadeWeightAt(uint32_t done)
{
	return EchoFadeTable()[done < kEchoFadeFrames ? done : kEchoFadeFrames];
}

inline bool
RendersEchoFor(int32_t mode, bool unitOk)
{
	return mode == MODE_SPATIAL && unitOk;
}

struct EchoQueue
{
	bool flag;
	bool copies;
};

inline EchoQueue
EchoQueueFor(bool reflections, bool carried, bool renders)
{
	EchoQueue q;
	q.flag = reflections ? renders : carried;
	q.copies = reflections && !renders;
	return q;
}

}
