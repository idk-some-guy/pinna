// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -Wall -Wextra -I src tests/audio_oal_spline_test.cpp tests/support/OalSplineResampler.cpp -o build/tests/audio_oal_spline_test && build/tests/audio_oal_spline_test
#include <stdio.h>
#include <math.h>
#include "CaResampler.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

// OpenAL Soft 1.25.2 core/cubic_tables.cpp, SplineTable: coefficients at mu = pi / 32, in float
static void
Coeffs(int pi, float c[4])
{
	double mu = pi / 32.0, mu2 = mu * mu, mu3 = mu * mu2;
	c[0] = (float)(-(1.0 / 3.0) * mu + 0.5 * mu2 - (1.0 / 6.0) * mu3);
	c[1] = (float)(1.0 - 0.5 * mu - mu2 + 0.5 * mu3);
	c[2] = (float)(mu + 0.5 * mu2 - 0.5 * mu3);
	c[3] = (float)(-(1.0 / 6.0) * mu + (1.0 / 6.0) * mu3);
}

static void
TestShape(void)
{
	float w[kSincMaxTaps];
	int first = 0;
	for(double step = 0.25; step <= 10.0; step *= 2.0){
		CHECK(SincHalfFor(step) == 2);
		CHECK(SincWeights(0.3, step, w, &first) == 4);
		CHECK(first == -1);
	}
	SincWeights(0.0, 1.0, w, &first);
	CHECK(w[0] == 0.0f && w[1] == 1.0f && w[2] == 0.0f && w[3] == 0.0f);
	SincWeights(0.5, 1.0, w, &first);
	CHECK(w[0] == -0.0625f && w[1] == 0.5625f && w[2] == 0.5625f && w[3] == -0.0625f);
}

static void
TestPhases(void)
{
	float w[kSincMaxTaps], c[4], n[4];
	int first, worst = 0;
	for(int pi = 0; pi < 32; pi++){
		Coeffs(pi, c);
		SincWeights(pi / 32.0, 2.5, w, &first);
		for(int k = 0; k < 4; k++)
			if(w[k] != c[k])
				worst++;
	}
	CHECK(worst == 0);
	int off = 0;
	for(int pi = 0; pi < 32; pi++){
		Coeffs(pi, c);
		if(pi < 31)
			Coeffs(pi + 1, n);
		else{
			float z[4];
			Coeffs(0, z);
			n[0] = 0.0f;
			n[1] = z[0];
			n[2] = z[1];
			n[3] = z[2];
		}
		for(int sub = 1; sub < 2048; sub += 97){
			unsigned frac = (unsigned)pi << 11 | (unsigned)sub;
			float pf = (float)sub * (1.0f / 2048.0f);
			SincWeights(frac / 65536.0, 1.0, w, &first);
			for(int k = 0; k < 4; k++)
				if(w[k] != c[k] + pf * (n[k] - c[k]))
					off++;
		}
	}
	CHECK(off == 0);
	float a[4], b[4];
	SincWeights(0.5 + 0.4 / 65536.0, 1.0, a, &first);
	SincWeights(0.5, 1.0, b, &first);
	CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

static void
TestDot(void)
{
	int16_t s[8] = { 1200, -3000, 32767, -32768, 15000, 7, -9, 22222 };
	float w[kSincMaxTaps];
	int first;
	double worst = 0.0;
	for(int i = 0; i < 64; i++){
		double frac = i / 64.0 + 0.003;
		SincWeights(frac, 1.7, w, &first);
		float ref = 0.0f;
		for(int k = 0; k < 4; k++)
			ref += w[k] * s[3 + first + k];
		worst = fmax(worst, fabs(SincDot16(s + 3, frac, 1.7) - ref));
	}
	CHECK(worst <= 0.01);
}

static double
NyquistDb(void)
{
	float w[kSincMaxTaps];
	int first;
	double p = 0.0;
	const int n = 4096;
	for(int i = 0; i < n; i++){
		double mu = (i + 0.5) / n;
		SincWeights(mu, 1.0, w, &first);
		double re = 0.0, im = 0.0;
		for(int k = 0; k < 4; k++){
			double ph = -M_PI * (k + first - mu);
			re += w[k] * cos(ph);
			im += w[k] * sin(ph);
		}
		p += re * re + im * im;
	}
	return 10.0 * log10(p / n);
}

int
main(void)
{
	InitResampler();
	TestShape();
	TestPhases();
	TestDot();
	double db = NyquistDb();
	printf("MEASURED spline power response at the source's Nyquist: %.2f dB\n", db);
	CHECK(fabs(db + 4.2) < 0.1);
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all oal spline tests passed\n");
	return 0;
}
