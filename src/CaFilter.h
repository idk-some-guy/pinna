#pragma once

#include <math.h>
#include <stdint.h>

namespace pinna {

struct Biquad
{
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

inline void
HighShelf(Biquad &b, float f0, float rate, float gain)
{
	float A = gain < 0.001f ? 0.001f : gain;
	if(f0 > 0.49f * rate)
		f0 = 0.49f * rate;
	float w0 = 2.0f * 3.14159265f * f0 / rate;
	float cw = cosf(w0), sw = sinf(w0);
	float alpha = sw / 2.0f * sqrtf(2.0f);
	float sa = 2.0f * sqrtf(A) * alpha;
	float a0 = (A + 1) - (A - 1) * cw + sa;
	b.b0 = A * ((A + 1) + (A - 1) * cw + sa) / a0;
	b.b1 = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
	b.b2 = A * ((A + 1) + (A - 1) * cw - sa) / a0;
	b.a1 = 2 * ((A - 1) - (A + 1) * cw) / a0;
	b.a2 = ((A + 1) - (A - 1) * cw - sa) / a0;
}

inline float
FlushTiny(float v)
{
	return fabsf(v) < 1e-15f ? 0.0f : v;
}

inline void
RunBiquad(Biquad &b, float *x, uint32_t n)
{
	for(uint32_t i = 0; i < n; i++){
		float in = x[i];
		float out = b.b0 * in + b.z1;
		b.z1 = FlushTiny(b.b1 * in - b.a1 * out + b.z2);
		b.z2 = FlushTiny(b.b2 * in - b.a2 * out);
		x[i] = out;
	}
}

}
