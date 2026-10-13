#pragma once

#include <stdint.h>
#include <math.h>
#include <vector>

namespace ca {

namespace seam {

struct Kernel
{
	uint32_t window, lo, hi;
	std::vector<double> hann, cosT, sinT;

	explicit Kernel(uint32_t w) : window(w), lo(0), hi(0)
	{
		lo = (uint32_t)ceil(2000.0 * w / 22050.0);
		hi = (uint32_t)floor(8000.0 * w / 22050.0);
		hann.resize(w);
		for(uint32_t n = 0; n < w; n++)
			hann[n] = 0.5 - 0.5 * cos(2.0 * M_PI * n / w);
		uint32_t bins = hi >= lo ? hi - lo + 1 : 0;
		cosT.resize((size_t)bins * w);
		sinT.resize((size_t)bins * w);
		for(uint32_t b = 0; b < bins; b++)
			for(uint32_t n = 0; n < w; n++){
				double a = 2.0 * M_PI * (double)((uint64_t)(lo + b) * n % w) / w;
				cosT[(size_t)b * w + n] = cos(a);
				sinT[(size_t)b * w + n] = sin(a);
			}
	}
};

struct Loop
{
	const int16_t *s;
	uint32_t start, len;

	double at(int64_t j) const
	{
		int64_t m = (j - (int64_t)start) % (int64_t)len;
		if(m < 0)
			m += len;
		return s[start + m];
	}
};

inline double
BandEnergy(const Kernel &k, const Loop &l, int64_t first)
{
	double e = 0.0;
	uint32_t bins = k.hi >= k.lo ? k.hi - k.lo + 1 : 0;
	std::vector<double> x(k.window);
	for(uint32_t n = 0; n < k.window; n++)
		x[n] = l.at(first + n) * k.hann[n];
	for(uint32_t b = 0; b < bins; b++){
		double re = 0.0, im = 0.0;
		const double *c = &k.cosT[(size_t)b * k.window], *s = &k.sinT[(size_t)b * k.window];
		for(uint32_t n = 0; n < k.window; n++){
			re += x[n] * c[n];
			im -= x[n] * s[n];
		}
		e += re * re + im * im;
	}
	return e;
}

inline float
Score(const Kernel &k, const Loop &l, uint32_t at)
{
	int64_t a = at, w = k.window;
	double before = BandEnergy(k, l, a - w);
	double after = BandEnergy(k, l, a);
	double across = BandEnergy(k, l, a - w / 2);
	return (float)(across / (0.5 * (before + after) + 256.0));
}

inline bool
Usable(const int16_t *s, uint32_t loopStart, uint32_t loopEnd, uint32_t window)
{
	return s && window >= 8 && loopEnd > loopStart && loopEnd - loopStart >= window;
}

}

inline float
SeamScore(const int16_t *s, uint32_t loopStart, uint32_t loopEnd, uint32_t at, uint32_t window)
{
	if(!seam::Usable(s, loopStart, loopEnd, window))
		return 0.0f;
	seam::Kernel k(window);
	seam::Loop l = { s, loopStart, loopEnd - loopStart };
	return seam::Score(k, l, at);
}

inline float
SeamPercentile(const int16_t *s, uint32_t loopStart, uint32_t loopEnd, uint32_t at, uint32_t window)
{
	if(!seam::Usable(s, loopStart, loopEnd, window))
		return 0.0f;
	seam::Kernel k(window);
	seam::Loop l = { s, loopStart, loopEnd - loopStart };
	float v = seam::Score(k, l, at);
	uint32_t below = 0;
	for(uint32_t p = loopStart; p < loopEnd; p++)
		if(seam::Score(k, l, p) < v)
			below++;
	return 100.0f * below / (loopEnd - loopStart);
}

inline uint32_t
BuildCrossfadedLoop(const int16_t *src, uint32_t length, uint32_t fadeFrames, int16_t *dst, uint32_t *loopStart)
{
	for(uint32_t i = 0; i < length; i++)
		dst[i] = src[i];
	*loopStart = 0;
	if(fadeFrames > length / 2)
		return length;
	for(uint32_t i = 0; i < fadeFrames; i++){
		double w = 0.5 - 0.5 * cos(M_PI * (i + 0.5) / fadeFrames);
		double v = src[length - fadeFrames + i] * (1.0 - w) + src[i] * w;
		long r = lround(v);
		dst[length - fadeFrames + i] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
	*loopStart = fadeFrames;
	return length;
}

}
