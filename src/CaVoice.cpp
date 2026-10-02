#include <math.h>
#include <string.h>
#include "CaVoice.h"

namespace ca {

struct Loop
{
	bool on;
	int64_t start, end;
};

static inline Loop
LoopOf(const VoiceSnap &sn)
{
	Loop lp = { sn.loop != 0, sn.loopStart, sn.loopEnd };
	if(lp.on && (lp.end <= lp.start || lp.end > (int64_t)sn.length)){
		lp.start = 0;
		lp.end = sn.length;
	}
	return lp;
}

static inline float
Fetch(const VoiceSnap &sn, const Loop &lp, bool wrapped, int64_t idx)
{
	if(lp.on){
		int64_t len = lp.end - lp.start;
		if(idx >= lp.end)
			idx = lp.start + (idx - lp.start) % len;
		else if(idx < lp.start && wrapped)
			idx = lp.end - 1 - (lp.start - 1 - idx) % len;
		else if(idx < 0)
			return 0.0f;
	}else if(idx < 0)
		return 0.0f;
	else if(idx >= (int64_t)sn.length)
		idx = sn.length - 1;
	return sn.data[idx] * (1.0f / 32768.0f);
}

static double
OalStep(const VoiceSnap &sn, float outRate)
{
	if(!(sn.freq >= 0.0f))
		return 0.0;
	float pitch = sn.baseRate ? sn.freq / (float)sn.baseRate * ((float)sn.baseRate / outRate) : sn.freq / outRate;
	if(pitch > (float)kOalMaxPitch)
		return kOalMaxPitch;
	uint32_t fixed = (uint32_t)(pitch * (float)kOalStepOne);
	return (double)(fixed ? fixed : 1) / kOalStepOne;
}

void
StartVoice(VoiceState &st, const VoiceSnap &sn)
{
	st.playId = sn.playId;
	st.active = sn.playing && sn.data != 0 && sn.length > 0;
	st.wrapped = false;
	st.pos = 0.0;
	st.step = -1.0;
	st.gainL = st.targetL = sn.gainL;
	st.gainR = st.targetR = sn.gainR;
	st.send = st.targetSend = sn.send;
	st.incL = st.incR = st.incSend = 0.0f;
	st.gainLeft = 0;
}

void
StopVoice(VoiceState &st, const VoiceSnap &sn)
{
	if(!st.active)
		return;
	st.active = false;
	if(st.step <= 0.0)
		return;
	Loop lp = LoopOf(sn);
	int64_t i = (int64_t)st.pos, heldAt = i;
	float held = Fetch(sn, lp, st.wrapped, i);
	for(int k = 1; k < kOalResamplerEdge; k++){
		float v = Fetch(sn, lp, st.wrapped, i + k);
		if(fabsf(v) < fabsf(held)){
			held = v;
			heldAt = i + k;
		}
	}
	float next[3][kOalFadeFrames], w[kSincMaxTaps];
	uint32_t at = kOalFadeFrames - st.fadeLeft;
	double p = st.pos;
	bool flat = false;
	for(uint32_t k = 0; k < kOalFadeFrames; k++, p += st.step){
		float r = held;
		if(!flat){
			int64_t j = (int64_t)p;
			int first;
			int taps = SincWeights(p - (double)j, st.step, w, &first);
			if(j + first > heldAt)
				flat = true;
			else{
				r = 0.0f;
				for(int t = 0; t < taps; t++)
					r += (j + first + t > heldAt ? held : Fetch(sn, lp, st.wrapped, j + first + t)) * w[t];
			}
		}
		float g = (float)(kOalFadeFrames - k) / kOalFadeFrames;
		bool old = k < st.fadeLeft;
		next[0][k] = r * st.gainL * g + (old ? st.fade[0][at + k] : 0.0f);
		next[1][k] = r * st.gainR * g + (old ? st.fade[1][at + k] : 0.0f);
		next[2][k] = r * st.send * g + (old ? st.fade[2][at + k] : 0.0f);
	}
	memcpy(st.fade, next, sizeof(next));
	st.fadeLeft = kOalFadeFrames;
}

bool
RenderVoice(VoiceState &st, const VoiceSnap &sn, float outRate, float *accL, float *accR, float *accSend, uint32_t frames)
{
	if(st.active && !sn.playing)
		StopVoice(st, sn);
	if(!st.active && !st.fadeLeft)
		return false;
	if(st.active){
		st.step = OalStep(sn, outRate);
		if(sn.gainL != st.targetL || sn.gainR != st.targetR || sn.send != st.targetSend){
			st.targetL = sn.gainL;
			st.targetR = sn.gainR;
			st.targetSend = sn.send;
			st.incL = (sn.gainL - st.gainL) / kOalFadeFrames;
			st.incR = (sn.gainR - st.gainR) / kOalFadeFrames;
			st.incSend = (sn.send - st.send) / kOalFadeFrames;
			st.gainLeft = kOalFadeFrames;
		}
	}
	Loop lp = LoopOf(sn);
	float w[kSincMaxTaps];
	bool ended = false;
	for(uint32_t n = 0; n < frames; n++){
		if(st.fadeLeft){
			uint32_t k = kOalFadeFrames - st.fadeLeft--;
			accL[n] += st.fade[0][k];
			accR[n] += st.fade[1][k];
			accSend[n] += st.fade[2][k];
		}
		if(!st.active){
			if(!st.fadeLeft)
				break;
			continue;
		}
		float gl = st.gainL, gr = st.gainR, gs = st.send;
		if(st.gainLeft){
			if(--st.gainLeft){
				st.gainL += st.incL;
				st.gainR += st.incR;
				st.send += st.incSend;
			}else{
				st.gainL = st.targetL;
				st.gainR = st.targetR;
				st.send = st.targetSend;
			}
		}
		if(st.step == 0.0)
			continue;
		int64_t i = (int64_t)st.pos;
		double frac = st.pos - (double)i;
		int half = SincHalfFor(st.step);
		int64_t lo = i + 1 - half, hi = i + 1 + half;
		int64_t okLo = lp.on && st.wrapped ? lp.start : 0;
		int64_t okHi = lp.on ? lp.end : sn.length;
		float s;
		if(lo >= okLo && hi <= okHi)
			s = SincDot16(sn.data + i, frac, st.step) * (1.0f / 32768.0f);
		else{
			int first;
			int taps = SincWeights(frac, st.step, w, &first);
			s = 0.0f;
			for(int k = 0; k < taps; k++)
				s += Fetch(sn, lp, st.wrapped, i + first + k) * w[k];
		}
		accL[n] += s * gl;
		accR[n] += s * gr;
		accSend[n] += s * gs;
		st.pos += st.step;
		if(lp.on){
			if(st.pos >= lp.end){
				st.pos = lp.start + fmod(st.pos - lp.start, (double)(lp.end - lp.start));
				st.wrapped = true;
			}
		}else if(st.pos >= sn.length){
			StopVoice(st, sn);
			ended = true;
		}
	}
	return ended;
}

}
