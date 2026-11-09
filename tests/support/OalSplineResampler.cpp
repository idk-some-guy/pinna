// Test-only stand-in for CaResampler.cpp: OpenAL Soft 1.25.2's SplineTable (core/cubic_tables.cpp) applied as do_cubic (core/mixer/mixer_c.cpp).
#include <stdint.h>
#include "CaConfig.h"
#include "CaResampler.h"

namespace pinna {

enum { kPhaseBits = 5, kPhases = 1 << kPhaseBits, kPhaseDiffBits = 16 - kPhaseBits, kPhaseDiffMask = (1 << kPhaseDiffBits) - 1 };

static float sCoeffs[kPhases][4], sDeltas[kPhases][4];

void
InitResampler(void)
{
	const double third = 1.0 / 3.0, sixth = 1.0 / 6.0;
	for(int pi = 0; pi < kPhases; pi++){
		double mu = (double)pi / kPhases, mu2 = mu * mu, mu3 = mu * mu2;
		sCoeffs[pi][0] = (float)(-third * mu + 0.5 * mu2 - sixth * mu3);
		sCoeffs[pi][1] = (float)(1.0 - 0.5 * mu - mu2 + 0.5 * mu3);
		sCoeffs[pi][2] = (float)(mu + 0.5 * mu2 - 0.5 * mu3);
		sCoeffs[pi][3] = (float)(-sixth * mu + sixth * mu3);
	}
	for(int pi = 0; pi < kPhases - 1; pi++)
		for(int k = 0; k < 4; k++)
			sDeltas[pi][k] = sCoeffs[pi + 1][k] - sCoeffs[pi][k];
	const int last = kPhases - 1;
	sDeltas[last][0] = 0.0f - sCoeffs[last][0];
	sDeltas[last][1] = sCoeffs[0][0] - sCoeffs[last][1];
	sDeltas[last][2] = sCoeffs[0][1] - sCoeffs[last][2];
	sDeltas[last][3] = sCoeffs[0][2] - sCoeffs[last][3];
}

int
SincHalfFor(double)
{
	return 2;
}

static inline void
Weights(double frac, float *w)
{
	unsigned f = (unsigned)(frac * kOalStepOne);
	unsigned pi = f >> kPhaseDiffBits;
	float pf = (float)(f & kPhaseDiffMask) * (1.0f / (1 << kPhaseDiffBits));
	for(int k = 0; k < 4; k++)
		w[k] = sCoeffs[pi][k] + pf * sDeltas[pi][k];
}

int
SincWeights(double frac, double, float *w, int *first)
{
	*first = -1;
	Weights(frac, w);
	return 4;
}

float
SincDot16(const int16_t *center, double frac, double)
{
	float w[4];
	Weights(frac, w);
	return w[0] * center[-1] + w[1] * center[0] + w[2] * center[1] + w[3] * center[2];
}

}
