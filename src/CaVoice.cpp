#include <math.h>
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
	}else if(idx < 0 || idx >= (int64_t)sn.length)
		return 0.0f;
	return sn.data[idx] * (1.0f / 32768.0f);
}

uint32_t
RampFrames(float outRate)
{
	uint32_t n = (uint32_t)(outRate * 0.005f + 0.5f);
	return n ? n : 1;
}

void
StartVoice(VoiceState &st, const VoiceSnap &sn)
{
	st.playId = sn.playId;
	st.active = sn.playing && sn.data != 0 && sn.length > 0;
	st.wrapped = false;
	st.pos = 0.0;
	st.step = -1.0;
	st.stepTarget = -1.0;
	st.stepInc = 0.0;
	st.stepLeft = 0;
	st.gainL = st.targetL = sn.gainL;
	st.gainR = st.targetR = sn.gainR;
	st.send = st.targetSend = sn.send;
	st.incL = st.incR = st.incSend = 0.0f;
	st.gainLeft = 0;
}

bool
RenderVoice(VoiceState &st, const VoiceSnap &sn, float outRate, float *accL, float *accR, float *accSend, uint32_t frames)
{
	if(!st.active)
		return false;
	uint32_t ramp = RampFrames(outRate);
	double target = sn.freq > 0.0f ? sn.freq / outRate : 0.0;
	if(st.step < 0.0){
		st.step = st.stepTarget = target;
	}else if(target != st.stepTarget){
		st.stepTarget = target;
		st.stepInc = (target - st.step) / ramp;
		st.stepLeft = ramp;
	}
	float tl = sn.playing ? sn.gainL : 0.0f;
	float tr = sn.playing ? sn.gainR : 0.0f;
	float ts = sn.playing ? sn.send : 0.0f;
	if(tl != st.targetL || tr != st.targetR || ts != st.targetSend){
		st.targetL = tl;
		st.targetR = tr;
		st.targetSend = ts;
		st.incL = (tl - st.gainL) / ramp;
		st.incR = (tr - st.gainR) / ramp;
		st.incSend = (ts - st.send) / ramp;
		st.gainLeft = ramp;
	}
	Loop lp = LoopOf(sn);
	float w[kSincMaxTaps];
	bool ended = false;
	for(uint32_t n = 0; n < frames; n++){
		if(!sn.playing && st.gainLeft == 0){
			st.active = false;
			break;
		}
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
		if(st.stepLeft)
			st.step = --st.stepLeft ? st.step + st.stepInc : st.stepTarget;
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
		accL[n] += s * st.gainL;
		accR[n] += s * st.gainR;
		accSend[n] += s * st.send;
		st.pos += st.step;
		if(lp.on){
			if(st.pos >= lp.end){
				st.pos = lp.start + fmod(st.pos - lp.start, (double)(lp.end - lp.start));
				st.wrapped = true;
			}
		}else if(st.pos >= sn.length){
			ended = true;
			st.active = false;
			break;
		}
	}
	return ended;
}

}
