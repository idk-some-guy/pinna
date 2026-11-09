#pragma once

#include <float.h>
#include <math.h>
#include <stdint.h>
#include "CaRules.h"
#include "OalPanTable.h"

namespace ca {

inline bool
OalFinitePosition(float gx, float gy, float gz)
{
	return isfinite(gx) && isfinite(gy) && isfinite(gz);
}

inline bool
OalDistanceValid(float d)
{
	return d >= 0.0f && isfinite(d);
}

// Raw SetChannel3DPosition args: OpenAL ignores the listener orientation for relative sources, so +y pans as the up pole and -z as front.
inline StereoGain
OalPanGains(float gx, float gy, float gz)
{
	float x = -gx, y = gy, z = gz;
	float m = fmaxf(fabsf(x), fmaxf(fabsf(y), fabsf(z)));
	if(!OalFinitePosition(gx, gy, gz) || !(x * x + y * y + z * z > FLT_EPSILON)){
		StereoGain c = { kOalPanCentre[0], kOalPanCentre[1] };
		return c;
	}
	x /= m;
	y /= m;
	z /= m;
	const float halfPi = 1.57079633f;
	float h = sqrtf(x * x + z * z) / sqrtf(x * x + y * y + z * z);
	float az = atan2f(x, -z);
	if(fabsf(az) < halfPi)
		az = copysignf(fminf(fabsf(az) * kOalPanWarp, halfPi), az);
	float right = sinf(az) * h, front = cosf(az) * h;
	StereoGain g = { kOalPanK0 - kOalPanSide * right + kOalPanFront * front, kOalPanK0 + kOalPanSide * right + kOalPanFront * front };
	return g;
}

inline StereoGain
OalSourceGains(float gx, float gy, float gz, float ref, float maxDist)
{
	float att = OalInverseClampedGain(sqrtf(gx * gx + gy * gy + gz * gz), ref, maxDist);
	StereoGain p = OalPanGains(gx, gy, gz);
	StereoGain g = { att * p.l, att * p.r };
	return g;
}

inline const float *
OalStreamPanGains(uint32_t pan)
{
	return kOalStreamPan[pan > 127 ? 127 : pan];
}

}
