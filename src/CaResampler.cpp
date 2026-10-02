#include <math.h>
#include <stdlib.h>
#include "CaResampler.h"

namespace ca {

enum { kTableLen = (kSincHalf + 1) * kSincPhases + 2 };
static_assert(kTableLen >= (kSincHalf + 1) * kSincPhases, "a stretched tap indexes below (kSincHalf + 1) * kSincPhases");

static float sTable[kTableLen];
static float sSlope[kTableLen];
static float sRows[kSincPhases + 1][2 * kSincHalf];
static const double kCutoff = 0.95;
static const double kBeta = 7.0;

static double
BesselI0(double x)
{
	double sum = 1.0, term = 1.0;
	for(int k = 1; k < 32; k++){
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
	}
	return sum;
}

void
InitResampler(void)
{
	double norm = BesselI0(kBeta);
	for(int i = 0; i < kTableLen; i++){
		double t = (double)i / kSincPhases;
		double x = kCutoff * t;
		double sinc = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
		double w = t / kSincHalf;
		double win = w >= 1.0 ? 0.0 : BesselI0(kBeta * sqrt(1.0 - w * w)) / norm;
		sTable[i] = (float)(sinc * win);
	}
	for(int i = 0; i < kTableLen - 1; i++)
		sSlope[i] = sTable[i + 1] - sTable[i];
	for(int p = 0; p <= kSincPhases; p++){
		double sum = 0.0;
		for(int k = 0; k < 2 * kSincHalf; k++){
			int i = abs((k - kSincHalf + 1) * kSincPhases - p);
			sRows[p][k] = i < kTableLen ? sTable[i] : 0.0f;
			sum += sRows[p][k];
		}
		for(int k = 0; k < 2 * kSincHalf; k++)
			sRows[p][k] = (float)(sRows[p][k] / sum);
	}
}

static inline float
StretchScale(double step)
{
	return (float)(kSincPhases / step);
}

int
SincHalfFor(double step)
{
	if(step <= 1.0)
		return kSincHalf;
	double stretch = step > kSincMaxStretch ? kSincMaxStretch : step;
	return (int)ceil(kSincHalf * stretch);
}

int
SincWeights(double frac, double step, float *w, int *first)
{
	int half = SincHalfFor(step);
	int taps = 2 * half;
	*first = 1 - half;
	if(step <= 1.0){
		double ph = frac * kSincPhases;
		int p = (int)ph;
		float pf = (float)(ph - p);
		const float *h0 = sRows[p], *h1 = sRows[p + 1];
		for(int k = 0; k < taps; k++)
			w[k] = h0[k] + (h1[k] - h0[k]) * pf;
		return taps;
	}
	float scale = StretchScale(step);
	float f = (float)frac;
	float sum = 0.0f;
	for(int k = 0; k < taps; k++){
		float a = fabsf((float)(k - half + 1) - f) * scale;
		int i = (int)a;
		float v = sTable[i] + sSlope[i] * (a - (float)i);
		w[k] = v;
		sum += v;
	}
	float inv = 1.0f / sum;
	for(int k = 0; k < taps; k++)
		w[k] *= inv;
	return taps;
}

float
SincDot16(const int16_t *center, double frac, double step)
{
	int half = SincHalfFor(step);
	const int16_t *d = center + 1 - half;
	if(step <= 1.0){
		double ph = frac * kSincPhases;
		int p = (int)ph;
		float pf = (float)(ph - p);
		const float *h0 = sRows[p], *h1 = sRows[p + 1];
		float acc = 0.0f;
		for(int k = 0; k < 2 * kSincHalf; k++)
			acc += d[k] * (h0[k] + (h1[k] - h0[k]) * pf);
		return acc;
	}
	float scale = StretchScale(step);
	float f = (float)frac;
	float acc = 0.0f, sum = 0.0f;
	for(int k = 0; k < 2 * half; k++){
		float a = fabsf((float)(k - half + 1) - f) * scale;
		int i = (int)a;
		float v = sTable[i] + sSlope[i] * (a - (float)i);
		acc += d[k] * v;
		sum += v;
	}
	return acc / sum;
}

}
