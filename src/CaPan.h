#pragma once

#include <math.h>
#include <stdint.h>
#include "CaRules.h"
#include "OalPanTable.h"

namespace ca {

inline StereoGain
OalPanGains(float gx, float gy, float gz)
{
	float x = -gx, y = gy, z = gz;
	float m = fmaxf(fabsf(x), fmaxf(fabsf(y), fabsf(z)));
	if(m == 0.0f){
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

inline const float *
OalStreamPanGains(uint32_t pan)
{
	return kOalStreamPan[pan > 127 ? 127 : pan];
}

}
