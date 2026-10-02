#pragma once

#include <stdint.h>

namespace ca {

enum
{
	kSincHalf = 16,
	kSincPhases = 512,
	kSincMaxStretch = 4,
	kSincMaxTaps = 2 * kSincHalf * kSincMaxStretch
};

void InitResampler(void);
int SincHalfFor(double step);
int SincWeights(double frac, double step, float *w, int *first);
float SincDot16(const int16_t *center, double frac, double step);

}
