#include <string.h>
#include "CaMixer.h"

namespace ca {

Shared::Shared(void) : latchedSerial(0), activeVoices(0), underruns(0), streamLatency(0), renders(0)
{
	for(int c = 0; c < kChannels; c++){
		finishedId[c].store(0);
		readerSlot[c].store(-1);
	}
}

Mixer::Mixer(Shared *shared) : mShared(shared), mRouteSpatial(false), mHeadSpatial(false), mShelfRate(0.0f)
{
	mDip.phase = kDipIdle;
	mDip.frame = 0;
	mChunks = 0;
	memset(mChunkDir, 0, sizeof(mChunkDir));
	memset(mDirChunks, 0, sizeof(mDirChunks));
	memset(mVoice, 0, sizeof(mVoice));
	memset(mLast, 0, sizeof(mLast));
	memset(mShelf, 0, sizeof(mShelf));
	memset(mDir, 0, sizeof(mDir));
	memset(mRouteMono, 0, sizeof(mRouteMono));
	memset(mTail, 0, sizeof(mTail));
	memset(mTailLeft, 0, sizeof(mTailLeft));
	for(int c = 0; c < kChannels; c++){
		mShelfHF[c] = -1.0f;
		mMono[c] = new float[kMaxBlock]();
		mDirLive[c] = false;
	}
}

Mixer::~Mixer(void)
{
	for(int c = 0; c < kChannels; c++)
		delete[] mMono[c];
}

void
Mixer::SwapRoute(int c)
{
	VoiceState &st = mVoice[c];
	for(int b = 0; b < 3; b++)
		for(uint32_t k = 0; k < kOalFadeFrames; k++){
			float t = st.fade[b][k];
			st.fade[b][k] = mTail[c][b][k];
			mTail[c][b][k] = t;
		}
	uint32_t left = st.fadeLeft;
	st.fadeLeft = mTailLeft[c];
	mTailLeft[c] = left;
	mRouteMono[c] = !mRouteMono[c];
}

void
Mixer::MoveRoute(int c, const VoiceSnap &to)
{
	VoiceState &st = mVoice[c];
	st.fadeLeft = 0;
	mTailLeft[c] = 0;
	if(st.active){
		st.gainL = st.targetL = to.gainL;
		st.gainR = st.targetR = to.gainR;
		st.incL = st.incR = 0.0f;
	}
	mRouteMono[c] = !mRouteMono[c];
}

bool
Mixer::Silent(void) const
{
	for(int c = 0; c < kChannels; c++)
		if(mVoice[c].active || mVoice[c].fadeLeft || mTailLeft[c])
			return false;
	return true;
}

bool
Mixer::render(float *dryL, float *dryR, float *send, float *strL, float *strR, uint32_t frames, float outRate, const SpatialRender *spatial)
{
	memset(dryL, 0, frames * sizeof(float));
	memset(dryR, 0, frames * sizeof(float));
	memset(send, 0, frames * sizeof(float));
	memset(strL, 0, frames * sizeof(float));
	memset(strR, 0, frames * sizeof(float));
	const Snapshot &s = mShared->snapshot.read();
	if(outRate != mShelfRate)
		for(int c = 0; c < kChannels; c++)
			mShelfHF[c] = -1.0f;
	bool usable = spatial && frames <= (uint32_t)kMaxBlock;
	bool want = usable && s.mode == MODE_SPATIAL;
	if(!usable){
		mRouteSpatial = mHeadSpatial = false;
		if(frames > (uint32_t)kMaxBlock)
			mDip.phase = kDipIdle;
	}else if(mDip.phase == kDipOut)
		mHeadSpatial = want;
	else if(want != mRouteSpatial){
		mHeadSpatial = want;
		if(mDip.phase == kDipIdle && Silent())
			mRouteSpatial = want;
		else
			ModeDipStart(mDip);
	}
	bool dipping = mDip.phase != kDipIdle;
	int32_t switchAt = dipping ? ModeDipFill(mDip, mModeGain, frames) : -1;
	bool route0 = mRouteSpatial, route1 = switchAt >= 0 ? mHeadSpatial : route0;
	uint32_t split = switchAt >= 0 ? (uint32_t)switchAt : frames;
	mRouteSpatial = route1;
	bool spatialMode = (route0 && split > 0) || (route1 && split < frames);
	uint32_t chunk = SpatialChunkFrames(outRate);
	mChunks = spatialMode ? (frames + chunk - 1) / chunk : 0;
	if(spatialMode)
		for(int c = 0; c < kChannels; c++)
			memset(mMono[c], 0, frames * sizeof(float));
	uint32_t active = 0;
	for(int c = 0; c < kChannels; c++){
		bool toMono = route0 && !s.voice[c].routeBypass;
		bool toMonoLater = route1 && !s.voice[c].routeBypass;
		bool moves = toMono != toMonoLater;
		VoiceSnap later;
		if(moves){
			later = s.voice[c];
			if(toMonoLater){
				later.gainL = later.monoGain;
				later.gainR = 0.0f;
			}
		}
		VoiceSnap monoSnap;
		if(toMono){
			monoSnap = s.voice[c];
			monoSnap.gainL = monoSnap.monoGain;
			monoSnap.gainR = 0.0f;
		}
		const VoiceSnap &sn = toMono ? monoSnap : s.voice[c];
		VoiceState &st = mVoice[c];
		bool started = false;
		if(sn.playId != st.playId){
			started = true;
			StopVoice(st, mLast[c]);
			if(toMono != mRouteMono[c])
				SwapRoute(c);
			StartVoice(st, sn);
			if(!st.fadeLeft && !mTailLeft[c])
				mShelf[c].z1 = mShelf[c].z2 = 0.0f;
			mShared->readerSlot[c].store(st.active ? sn.poolSlot : -1, std::memory_order_release);
			if(!st.active)
				mShared->finishedId[c].store(st.playId, std::memory_order_release);
		}else if(toMono != mRouteMono[c]){
			if(st.active && !sn.playing)
				StopVoice(st, mLast[c]);
			else if(st.active){
				float send = st.send;
				st.send = 0.0f;
				StopVoice(st, mLast[c]);
				st.send = send;
				st.active = true;
				st.gainL = st.targetL = sn.gainL;
				st.gainR = st.targetR = sn.gainR;
				st.incL = st.incR = 0.0f;
			}
			SwapRoute(c);
		}
		if(!spatialMode)
			mTailLeft[c] = 0;
		mLast[c] = sn;
		mDirChunks[c] = 0;
		if(!sn.playing)
			StopVoice(st, sn);
		if(!st.active && !st.fadeLeft && !mTailLeft[c]){
			if(moves){
				mRouteMono[c] = toMonoLater;
				mLast[c] = later;
			}
			mDirLive[c] = false;
			continue;
		}
		if(st.active)
			active++;
		bool dipped = false;
		if(toMono || toMonoLater){
			Direction target = { sn.azimuth, sn.elevation };
			mDirChunks[c] = VoiceDirectionBlock(mDir[c], target, started || !mDirLive[c], spatial->algorithm, frames, outRate, mChunkDir[c], mDipGain, &dipped);
			mDirLive[c] = toMonoLater;
		}else
			mDirLive[c] = false;
		Biquad &shelf = mShelf[c];
		if(sn.sendHF != mShelfHF[c]){
			HighShelf(shelf, 5000.0f, outRate, sn.sendHF);
			mShelfHF[c] = sn.sendHF;
		}
		const VoiceSnap *cur = &sn;
		bool mono = toMono;
		for(uint32_t done = 0; done < frames && (st.active || st.fadeLeft || mTailLeft[c]);){
			if(moves && done == split){
				MoveRoute(c, later);
				mLast[c] = later;
				cur = &later;
				mono = toMonoLater;
				moves = false;
			}
			uint32_t end = done < split ? split : frames;
			uint32_t n = end - done < (uint32_t)kMaxBlock ? end - done : (uint32_t)kMaxBlock;
			bool sending = st.send != 0.0f || cur->send != 0.0f || st.fadeLeft != 0 || mTailLeft[c] != 0 || shelf.z1 != 0.0f || shelf.z2 != 0.0f;
			if(sending)
				memset(mVoiceSend, 0, n * sizeof(float));
			bool ended = mono ? RenderVoice(st, *cur, outRate, mMono[c] + done, NULL, mVoiceSend, n) : RenderVoice(st, *cur, outRate, dryL + done, dryR + done, mVoiceSend, n);
			if(dipped && mono)
				for(uint32_t i = 0; i < n; i++)
					mMono[c][done + i] *= mDipGain[done + i];
			if(mTailLeft[c]){
				uint32_t k = kOalFadeFrames - mTailLeft[c], m = n < mTailLeft[c] ? n : mTailLeft[c];
				for(uint32_t i = 0; i < m; i++, k++){
					if(mRouteMono[c]){
						dryL[done + i] += mTail[c][0][k];
						dryR[done + i] += mTail[c][1][k];
					}else
						mMono[c][done + i] += mTail[c][0][k];
					mVoiceSend[i] += mTail[c][2][k];
				}
				mTailLeft[c] -= m;
			}
			if(sending){
				RunBiquad(shelf, mVoiceSend, n);
				if(isfinite(shelf.z1) && isfinite(shelf.z2))
					for(uint32_t i = 0; i < n; i++)
						send[done + i] += mVoiceSend[i];
				else
					shelf.z1 = shelf.z2 = 0.0f;
			}
			if(ended)
				mShared->finishedId[c].store(st.playId, std::memory_order_release);
			done += n;
		}
		if(moves){
			mRouteMono[c] = toMonoLater;
			mLast[c] = later;
		}
		if(!st.active)
			mShared->readerSlot[c].store(-1, std::memory_order_release);
	}
	if(dipping){
		for(uint32_t i = 0; i < frames; i++){
			dryL[i] *= mModeGain[i];
			dryR[i] *= mModeGain[i];
			send[i] *= mModeGain[i];
		}
		if(spatialMode)
			for(int c = 0; c < kChannels; c++)
				for(uint32_t i = 0; i < frames; i++)
					mMono[c][i] *= mModeGain[i];
	}
	uint32_t latency = mShared->streamLatency.load(std::memory_order_relaxed);
	for(int st = 0; st < kStreams; st++)
		mReader[st].render(s.stream[st], mShared->ring[st], mShared->status[st], mShared->underruns, outRate, latency, strL, strR, frames);
	mShelfRate = outRate;
	mShared->latchedSerial.store(s.serial, std::memory_order_release);
	mShared->activeVoices.store(active, std::memory_order_relaxed);
	mShared->renders.fetch_add(1, std::memory_order_release);
	return spatialMode;
}

}
