#pragma once

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>

struct Wave
{
	std::vector<float> l, r;
	uint32_t rate = 0;
};

inline bool
WriteWav(const char *path, const Wave &w)
{
	FILE *f = fopen(path, "wb");
	if(f == NULL)
		return false;
	uint32_t frames = (uint32_t)w.l.size(), data = frames * 8, riff = 36 + data, fmtSize = 16, rate = w.rate, byteRate = w.rate * 8;
	uint16_t fmt = 3, ch = 2, align = 8, bits = 32;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtSize, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	for(uint32_t i = 0; i < frames; i++){
		fwrite(&w.l[i], 4, 1, f);
		fwrite(&w.r[i], 4, 1, f);
	}
	bool ok = !ferror(f);
	return fclose(f) == 0 && ok;
}

inline bool
ReadWav(const char *path, Wave &w)
{
	w.rate = 0;
	w.l.clear();
	w.r.clear();
	FILE *f = fopen(path, "rb");
	if(f == NULL)
		return false;
	char hdr[44];
	bool ok = fread(hdr, 1, 44, f) == 44 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 36, "data", 4) == 0;
	uint32_t rate = 0, data = 0;
	if(ok){
		memcpy(&rate, hdr + 24, 4);
		memcpy(&data, hdr + 40, 4);
		w.rate = rate;
		w.l.resize(data / 8);
		w.r.resize(data / 8);
		for(uint32_t i = 0; ok && i < data / 8; i++)
			ok = fread(&w.l[i], 4, 1, f) == 1 && fread(&w.r[i], 4, 1, f) == 1;
	}
	fclose(f);
	return ok;
}

inline double
WindowDb(const std::vector<float> &x, size_t from, size_t len, float floorDb, const std::vector<uint8_t> *mask = NULL)
{
	double s = 0.0;
	size_t n = 0;
	for(size_t i = from; i < from + len && i < x.size(); i++)
		if(mask == NULL || i >= mask->size() || !(*mask)[i]){
			s += (double)x[i] * x[i];
			n++;
		}
	if(mask == NULL)
		n = len;
	double db = 10.0 * log10(s / (n ? n : 1) + 1e-20);
	return db < floorDb ? floorDb : db;
}

struct LevelResult
{
	float worstDb;
	int windows;
	int worstWindow;
	int worstChannel;
	int over;
};

static const float kLevelBoundDb = 0.5f;

inline LevelResult
CompareLevels(const Wave &ref, const Wave &test, float windowMs, float floorDb, const std::vector<uint8_t> *mask = NULL)
{
	LevelResult r = { 0.0f, 0, -1, -1, 0 };
	size_t wa = (size_t)(ref.rate * windowMs / 1000.0f), wb = (size_t)(test.rate * windowMs / 1000.0f);
	if(wa == 0 || wb == 0)
		return r;
	size_t count = ref.l.size() / wa < test.l.size() / wb ? ref.l.size() / wa : test.l.size() / wb;
	for(size_t k = 0; k < count; k++){
		bool over = false;
		for(int c = 0; c < 2; c++){
			double da = WindowDb(c ? ref.r : ref.l, k * wa, wa, floorDb, mask);
			double db = WindowDb(c ? test.r : test.l, k * wb, wb, floorDb, mask);
			if(da <= floorDb && db <= floorDb)
				continue;
			r.windows++;
			float diff = (float)fabs(da - db);
			over |= diff > kLevelBoundDb;
			if(diff > r.worstDb){
				r.worstDb = diff;
				r.worstWindow = (int)k;
				r.worstChannel = c;
			}
		}
		r.over += over;
	}
	return r;
}

inline std::vector<double>
Onsets(const Wave &w)
{
	std::vector<double> out;
	size_t hop = w.rate / 200;
	if(hop == 0)
		return out;
	std::vector<double> e;
	for(size_t h = 0; (h + 1) * hop <= w.l.size(); h++){
		double s = 0.0;
		for(size_t i = h * hop; i < (h + 1) * hop; i++){
			double m = 0.5 * (w.l[i] + w.r[i]);
			s += m * m;
		}
		e.push_back(10.0 * log10(s / hop + 1e-20));
	}
	double last = -1.0;
	for(size_t h = 0; h < e.size(); h++){
		double floor = 0.0;
		for(size_t k = h >= 10 ? h - 10 : 0; k < h; k++)
			floor += pow(10.0, e[k] / 10.0);
		floor = 10.0 * log10(floor / 10.0 + 1e-20);
		double t = (double)h * hop / w.rate;
		if(e[h] > -50.0 && e[h] > floor + 12.0 && (last < 0.0 || t - last > 0.05)){
			float peak = 0.0f;
			for(size_t i = h * hop; i < (h + 1) * hop; i++)
				peak = fmaxf(peak, fabsf(0.5f * (w.l[i] + w.r[i])));
			size_t at = h * hop;
			while(at < (h + 1) * hop && fabsf(0.5f * (w.l[at] + w.r[at])) < 0.5f * peak)
				at++;
			last = (double)at / w.rate;
			out.push_back(last);
		}
	}
	return out;
}

struct OnsetResult
{
	int refCount, testCount;
	float worstMs;
};

inline OnsetResult
CompareOnsets(const Wave &ref, const Wave &test)
{
	std::vector<double> a = Onsets(ref), b = Onsets(test);
	OnsetResult r = { (int)a.size(), (int)b.size(), 0.0f };
	for(size_t i = 0; i < a.size() && i < b.size(); i++)
		r.worstMs = fmaxf(r.worstMs, (float)fabs(a[i] - b[i]) * 1000.0f);
	if(a.size() != b.size())
		r.worstMs = 1e9f;
	return r;
}

inline double
PeriodSeconds(const std::vector<float> &x, uint32_t rate, size_t from, size_t len, double minS, double maxS)
{
	int minLag = (int)(minS * rate), maxLag = (int)(maxS * rate);
	double best = 0.0;
	int bestLag = 0;
	std::vector<double> r(maxLag + 2, 0.0);
	for(int lag = minLag - 1; lag <= maxLag + 1; lag++){
		if(lag < 1 || from + len + lag > x.size())
			continue;
		double s = 0.0, e0 = 0.0, e1 = 0.0;
		for(size_t i = from; i < from + len; i++){
			s += (double)x[i] * x[i + lag];
			e0 += (double)x[i] * x[i];
			e1 += (double)x[i + lag] * x[i + lag];
		}
		r[lag] = s / sqrt(e0 * e1 + 1e-20);
		if(lag >= minLag && lag <= maxLag && r[lag] > best){
			best = r[lag];
			bestLag = lag;
		}
	}
	for(int lag = minLag + 1; lag < maxLag; lag++)
		if(r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]){
			bestLag = lag;
			break;
		}
	if(best < 0.7 || bestLag <= minLag || bestLag >= maxLag)
		return 0.0;
	double y0 = r[bestLag - 1], y1 = r[bestLag], y2 = r[bestLag + 1];
	double shift = 0.5 * (y0 - y2) / (y0 - 2.0 * y1 + y2);
	return (bestLag + shift) / rate;
}

struct PitchResult
{
	int windows;
	float worstRatio;
};

inline PitchResult
ComparePitch(const Wave &ref, const Wave &test, float windowMs, double minS, double maxS)
{
	PitchResult r = { 0, 0.0f };
	size_t wa = (size_t)(ref.rate * windowMs / 1000.0f), wb = (size_t)(test.rate * windowMs / 1000.0f);
	if(wa == 0 || wb == 0)
		return r;
	size_t count = ref.l.size() / wa < test.l.size() / wb ? ref.l.size() / wa : test.l.size() / wb;
	for(size_t k = 0; k + 1 < count; k++){
		double pa = PeriodSeconds(ref.l, ref.rate, k * wa, wa, minS, maxS);
		double pb = PeriodSeconds(test.l, test.rate, k * wb, wb, minS, maxS);
		if(pa == 0.0 || pb == 0.0)
			continue;
		r.windows++;
		r.worstRatio = fmaxf(r.worstRatio, (float)fabs(pa / pb - 1.0));
	}
	return r;
}

inline float
PeakAbs(const Wave &w)
{
	float p = 0.0f;
	for(size_t i = 0; i < w.l.size(); i++)
		p = fmaxf(p, fmaxf(fabsf(w.l[i]), fabsf(w.r[i])));
	return p;
}

inline double
EnergyDb(const Wave &w, double fromS, double lenS)
{
	size_t from = (size_t)(fromS * w.rate), len = (size_t)(lenS * w.rate);
	double s = 0.0;
	for(size_t i = from; i < from + len && i < w.l.size(); i++)
		s += (double)w.l[i] * w.l[i] + (double)w.r[i] * w.r[i];
	return 10.0 * log10(s / (len ? len : 1) + 1e-20);
}

inline double
DecaySeconds(const Wave &w, double fromS, double toS)
{
	size_t from = (size_t)(fromS * w.rate), to = (size_t)(toS * w.rate);
	if(to > w.l.size())
		to = w.l.size();
	if(to <= from + 1)
		return 0.0;
	std::vector<double> sch(to - from);
	double acc = 0.0;
	for(size_t i = to; i-- > from;){
		acc += (double)w.l[i] * w.l[i] + (double)w.r[i] * w.r[i];
		sch[i - from] = acc;
	}
	if(acc <= 0.0)
		return 0.0;
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	size_t n = 0;
	for(size_t i = 0; i < sch.size(); i++){
		double db = 10.0 * log10(sch[i] / acc + 1e-30);
		if(db > -5.0 || db < -25.0)
			continue;
		double t = (double)i / w.rate;
		sx += t; sy += db; sxx += t * t; sxy += t * db;
		n++;
	}
	if(n < 2)
		return 0.0;
	double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
	return slope < 0.0 ? -60.0 / slope : 0.0;
}

inline int
FadeOutFrames(const std::vector<float> &x, uint32_t rate, double atS)
{
	size_t at = (size_t)(atS * rate), before = rate / 20, hold = rate / 100;
	if(at < before || at > x.size())
		return -1;
	double e = 0.0;
	for(size_t i = at - before; i < at; i++)
		e += (double)x[i] * x[i];
	double thr = 1e-3 * sqrt(e / before);
	size_t quiet = 0;
	for(size_t i = at; i < x.size(); i++){
		quiet = fabs(x[i]) < thr ? quiet + 1 : 0;
		if(quiet == hold)
			return (int)(i + 1 - hold - at);
	}
	return -1;
}

inline std::vector<float>
LowPass(const std::vector<float> &x, double cutoffHz, uint32_t rate)
{
	const int half = 511;
	std::vector<double> h(2 * half + 1);
	double sum = 0.0, fc = cutoffHz / rate;
	for(int k = -half; k <= half; k++){
		double s = k == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * k) / (M_PI * k);
		double win = 0.42 + 0.5 * cos(M_PI * k / (half + 1)) + 0.08 * cos(2.0 * M_PI * k / (half + 1));
		h[k + half] = s * win;
		sum += h[k + half];
	}
	std::vector<float> y(x.size());
	for(size_t i = 0; i < x.size(); i++){
		double acc = 0.0;
		for(int k = -half; k <= half; k++){
			long j = (long)i - k;
			if(j >= 0 && j < (long)x.size())
				acc += h[k + half] * x[j];
		}
		y[i] = (float)(acc / sum);
	}
	return y;
}

inline Wave
LowPassWave(const Wave &w, double cutoffHz)
{
	Wave out;
	out.rate = w.rate;
	out.l = LowPass(w.l, cutoffHz, w.rate);
	out.r = LowPass(w.r, cutoffHz, w.rate);
	return out;
}

struct HoldSpan
{
	size_t from, to;
	size_t boundary;
	float held;
};

static const float kHoldSilence = 1e-3f;
static const float kHoldVariation = 0.0095f;
static const size_t kHoldRamp = 64;

inline bool
IsUpdateBoundary(size_t at, uint32_t rate, float fps, uint32_t updateFrames)
{
	size_t g = (size_t)(at * (double)fps / rate) + 1;
	while(g > 0 && (size_t)floor(g * (double)rate / fps) > at)
		g--;
	return (at - (size_t)floor(g * (double)rate / fps)) % updateFrames == 0;
}

// A hold: OpenAL minus the engine stays constant about a non-zero mean for at most one update, up to an update boundary, then ramps to zero over 64 frames.
inline std::vector<HoldSpan>
FindHolds(const std::vector<float> &ref, const std::vector<float> &test, uint32_t rate, float fps, uint32_t updateFrames)
{
	std::vector<HoldSpan> out;
	size_t n = ref.size() < test.size() ? ref.size() : test.size();
	size_t covered = 0;
	for(size_t b = 1; b + kHoldRamp < n; b++){
		if(!IsUpdateBoundary(b, rate, fps, updateFrames))
			continue;
		double sum = ref[b] - test[b], lo = sum, hi = sum;
		if(fabs(sum) < kHoldSilence)
			continue;
		size_t s = b;
		while(s > covered && b + 1 - s <= updateFrames){
			double d = ref[s - 1] - test[s - 1], mean = (sum + d) / (b + 2 - s);
			double nlo = d < lo ? d : lo, nhi = d > hi ? d : hi;
			if(nhi - mean > kHoldVariation * fabs(mean) || mean - nlo > kHoldVariation * fabs(mean))
				break;
			sum += d;
			lo = nlo;
			hi = nhi;
			s--;
		}
		size_t len = b + 1 - s;
		float held = (float)(sum / len);
		if(len < kHoldRamp || len > updateFrames || fabsf(held) < kHoldSilence)
			continue;
		bool ramp = true;
		for(size_t k = 0; k <= kHoldRamp && ramp; k++)
			ramp = fabsf(ref[b + k] - test[b + k] - held * (1.0f - (float)k / kHoldRamp)) <= kHoldVariation * fabsf(held);
		if(!ramp)
			continue;
		bool ended = false;
		for(size_t i = s > kHoldRamp ? s - kHoldRamp : 0; i < s && !ended; i++)
			ended = fabsf(test[i]) >= kHoldSilence;
		if(!ended)
			continue;
		HoldSpan h = { s, b + kHoldRamp + 1, b, held };
		out.push_back(h);
		covered = h.to;
	}
	return out;
}

inline std::vector<uint8_t>
HoldMask(const Wave &ref, const Wave &test, float fps, uint32_t updateFrames, std::vector<HoldSpan> *spans)
{
	std::vector<uint8_t> mask(ref.l.size(), 0);
	for(int c = 0; c < 2; c++){
		std::vector<HoldSpan> s = FindHolds(c ? ref.r : ref.l, c ? test.r : test.l, ref.rate, fps, updateFrames);
		for(size_t k = 0; k < s.size(); k++){
			for(size_t i = s[k].from; i < s[k].to && i < mask.size(); i++)
				mask[i] = 1;
			if(spans)
				spans->push_back(s[k]);
		}
	}
	return mask;
}

inline double
OalSplineResponseDb(double cyclesPerSample)
{
	const int phases = 256;
	double p = 0.0;
	for(int i = 0; i < phases; i++){
		double mu = (i + 0.5) / phases, mu2 = mu * mu, mu3 = mu2 * mu;
		double c[4] = { -mu / 3.0 + 0.5 * mu2 - mu3 / 6.0, 1.0 - 0.5 * mu - mu2 + 0.5 * mu3, mu + 0.5 * mu2 - 0.5 * mu3, -mu / 6.0 + mu3 / 6.0 };
		double re = 0.0, im = 0.0;
		for(int k = 0; k < 4; k++){
			double ph = -2.0 * M_PI * cyclesPerSample * (k - 1 - mu);
			re += c[k] * cos(ph);
			im += c[k] * sin(ph);
		}
		p += re * re + im * im;
	}
	return 10.0 * log10(p / phases);
}

struct ShapeDiff
{
	float worst, peakRef, peakTest;
};

inline ShapeDiff
CompareShape(const Wave &ref, const Wave &test, size_t from, size_t len)
{
	ShapeDiff d = { 0.0f, 0.0f, 0.0f };
	for(size_t i = from; i < from + len && i < ref.l.size() && i < test.l.size(); i++){
		d.worst = fmaxf(d.worst, fmaxf(fabsf(ref.l[i] - test.l[i]), fabsf(ref.r[i] - test.r[i])));
		d.peakRef = fmaxf(d.peakRef, fmaxf(fabsf(ref.l[i]), fabsf(ref.r[i])));
		d.peakTest = fmaxf(d.peakTest, fmaxf(fabsf(test.l[i]), fabsf(test.r[i])));
	}
	return d;
}

static const double kOalSplineFlatFraction = 0.15;
static const float kRecordToleranceDb = 0.3f;

enum { LEVELS_PASS, LEVELS_RESAMPLER, LEVELS_HOLD, LEVELS_FAIL };

struct LevelRecord
{
	float worstDb;
	int over;
};

struct HoldRecord
{
	float worstDb;
	int segments;
};

struct LevelRecords
{
	const LevelRecord *resampler;
	const HoldRecord *fullHold, *bandHold;
};

struct HeldLevels
{
	LevelResult full, band;
	int segments;
	size_t frames;
	std::vector<HoldSpan> spans;
};

struct BandLevels
{
	LevelResult full, band;
	double cutoffHz;
	bool bandOk, fullOk, onRecord;
	int verdict, bandVerdict;
	HeldLevels held;
};

inline bool
OnRecord(const LevelResult &r, const LevelRecord *record)
{
	return record != NULL && fabsf(r.worstDb - record->worstDb) <= kRecordToleranceDb && r.over == record->over;
}

inline bool
WithinBound(const LevelResult &r)
{
	return r.windows > 0 && r.worstDb <= kLevelBoundDb;
}

inline bool
HoldOnRecord(const LevelResult &unmasked, const HeldLevels &h, const HoldRecord *record)
{
	return record != NULL && fabsf(unmasked.worstDb - record->worstDb) <= kRecordToleranceDb && h.segments == record->segments;
}

inline int
HoldVerdict(const LevelResult &unmasked, const LevelResult &masked, const HeldLevels &h, const HoldRecord *record)
{
	return h.segments > 0 && WithinBound(masked) && HoldOnRecord(unmasked, h, record) ? LEVELS_HOLD : LEVELS_FAIL;
}

inline BandLevels
CompareLevelsByBand(const Wave &ref, const Wave &test, uint32_t lowestRate, const LevelRecords &records, float fps, uint32_t updateFrames, const std::vector<uint8_t> *mask = NULL)
{
	BandLevels b;
	b.full = CompareLevels(ref, test, 50.0f, -60.0f, mask);
	b.cutoffHz = lowestRate != 0 && lowestRate != UINT32_MAX ? kOalSplineFlatFraction * lowestRate : 0.0;
	LevelResult none = { 0.0f, 0, -1, -1, 0 };
	Wave lowRef, lowTest;
	if(b.cutoffHz > 0.0){
		lowRef = LowPassWave(ref, b.cutoffHz);
		lowTest = LowPassWave(test, b.cutoffHz);
	}
	b.band = b.cutoffHz > 0.0 ? CompareLevels(lowRef, lowTest, 50.0f, -60.0f, mask) : none;
	b.bandOk = WithinBound(b.band);
	b.fullOk = WithinBound(b.full);
	b.onRecord = OnRecord(b.full, records.resampler);
	b.held.full = b.held.band = none;
	b.held.segments = 0;
	b.held.frames = 0;
	if(fps > 0.0f && (!b.fullOk || !b.bandOk)){
		std::vector<uint8_t> held = HoldMask(ref, test, fps, updateFrames, &b.held.spans);
		b.held.segments = (int)b.held.spans.size();
		for(size_t i = 0; i < held.size(); i++){
			b.held.frames += held[i];
			if(mask && i < mask->size())
				held[i] |= (*mask)[i];
		}
		if(b.held.segments){
			b.held.full = CompareLevels(ref, test, 50.0f, -60.0f, &held);
			if(b.cutoffHz > 0.0)
				b.held.band = CompareLevels(lowRef, lowTest, 50.0f, -60.0f, &held);
		}
	}
	if(b.bandOk)
		b.bandVerdict = records.bandHold ? LEVELS_FAIL : LEVELS_PASS;
	else
		b.bandVerdict = HoldVerdict(b.band, b.held.band, b.held, records.bandHold);
	if(b.fullOk)
		b.verdict = records.resampler || records.fullHold ? LEVELS_FAIL : LEVELS_PASS;
	else if(b.held.segments && WithinBound(b.held.full))
		b.verdict = HoldVerdict(b.full, b.held.full, b.held, records.fullHold);
	else
		b.verdict = b.bandOk && b.onRecord && !records.fullHold ? LEVELS_RESAMPLER : LEVELS_FAIL;
	return b;
}

inline BandLevels
CompareLevelsByBand(const Wave &ref, const Wave &test, uint32_t lowestRate, const LevelRecord *record, const std::vector<uint8_t> *mask = NULL)
{
	LevelRecords records = { record, NULL, NULL };
	return CompareLevelsByBand(ref, test, lowestRate, records, 0.0f, 0, mask);
}
