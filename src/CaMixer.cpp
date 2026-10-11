#include <string.h>
#include "CaMixer.h"

namespace ca {

Shared::Shared(void) : latchedSerial(0), activeVoices(0), underruns(0), streamLatency(0), renders(0), envPlace(ENV_OPEN)
{
	for(int c = 0; c < kChannels; c++){
		finishedId[c].store(0);
		readerSlot[c].store(-1);
	}
}

Mixer::Mixer(Shared *shared) : mShared(shared), mRouteSpatial(false), mHeadSpatial(false), mSeenSerial(0), mShelfRate(0.0f)
{
	mDip.phase = kDipIdle;
	mDip.frame = 0;
	mChunks = 0;
	mRoute.before = mRoute.after = false;
	mRoute.split = 0;
	memset(&mEnv, 0, sizeof(mEnv));
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

static inline VoiceSnap
RoutedSnap(const VoiceSnap &v, bool spatial, bool mono)
{
	VoiceSnap r = v;
	if(mono){
		r.gainL = r.monoGain;
		r.gainR = 0.0f;
	}
	if(spatial){
		r.send = r.envSend;
		r.sendHF = r.envSendHF;
	}
	return r;
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
	mRoute.before = route0;
	mRoute.after = route1;
	mRoute.split = split;
	mEnv = s.env;
	bool spatialMode = (route0 && split > 0) || (route1 && split < frames);
	uint32_t chunk = SpatialChunkFrames(outRate);
	mChunks = spatialMode ? (frames + chunk - 1) / chunk : 0;
	if(spatialMode)
		for(int c = 0; c < kChannels; c++)
			memset(mMono[c], 0, frames * sizeof(float));
	uint32_t active = 0;
	bool fresh = s.serial != mSeenSerial;
	mSeenSerial = s.serial;
	for(int c = 0; c < kChannels; c++){
		bool toMono = route0 && !s.voice[c].routeBypass;
		bool toMonoLater = route1 && !s.voice[c].routeBypass;
		bool moves = toMono != toMonoLater;
		bool switches = route0 != route1;
		VoiceSnap later;
		if(switches)
			later = RoutedSnap(s.voice[c], route1, toMonoLater);
		VoiceSnap first;
		if(route0)
			first = RoutedSnap(s.voice[c], true, toMono);
		const VoiceSnap &sn = route0 ? first : s.voice[c];
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
			mDirChunks[c] = VoiceDirectionBlock(mDir[c], target, started || !mDirLive[c], fresh, spatial->algorithm, frames, outRate, mChunkDir[c], mDipGain, &dipped);
			mDirLive[c] = toMonoLater;
		}else
			mDirLive[c] = false;
		Biquad &shelf = mShelf[c];
		const VoiceSnap *cur = &sn;
		bool mono = toMono;
		for(uint32_t done = 0; done < frames && (st.active || st.fadeLeft || mTailLeft[c]);){
			if(switches && done == split){
				if(moves)
					MoveRoute(c, later);
				mLast[c] = later;
				cur = &later;
				mono = toMonoLater;
				moves = switches = false;
			}
			if(cur->sendHF != mShelfHF[c]){
				HighShelf(shelf, 5000.0f, outRate, cur->sendHF);
				mShelfHF[c] = cur->sendHF;
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
				bool flat = (done < split ? route0 : route1) && cur->sendHF == 1.0f && shelf.z1 == 0.0f && shelf.z2 == 0.0f;
				if(!flat)
					RunBiquad(shelf, mVoiceSend, n);
				if(flat || (isfinite(shelf.z1) && isfinite(shelf.z2)))
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

void
EnvBank::init(int place)
{
	EnvInit(mState, place);
	mSerial = 0;
	mFresh = mFreshGain = true;
	memset(mGain, 0, sizeof(mGain));
	memset(mTarget, 0, sizeof(mTarget));
	memset(mInc, 0, sizeof(mInc));
	memset(mGainLeft, 0, sizeof(mGainLeft));
	memset(mShelf, 0, sizeof(mShelf));
	for(int u = 0; u < NUM_ENVS; u++)
		mShelfHF[u] = -1.0f;
}

bool
EnvBank::observe(const EnvSnap &e)
{
	if(!mFresh && e.serial == mSerial)
		return false;
	mFresh = false;
	mSerial = e.serial;
	return EnvObserve(mState, e.place);
}

void
EnvBank::settle(void)
{
	EnvSettle(mState);
}

void
EnvBank::split(const float *send, uint32_t from, uint32_t to, const EnvSnap &e, float *const *out, uint32_t frames, float rate)
{
	for(int u = 0; u < NUM_ENVS; u++){
		if(mFreshGain){
			mGain[u] = mTarget[u] = e.gain[u];
			mInc[u] = 0.0f;
			mGainLeft[u] = 0;
		}else if(e.gain[u] != mTarget[u]){
			mTarget[u] = e.gain[u];
			mInc[u] = (e.gain[u] - mGain[u]) / kOalFadeFrames;
			mGainLeft[u] = kOalFadeFrames;
		}
		if(e.gainHF[u] != mShelfHF[u]){
			HighShelf(mShelf[u], 5000.0f, rate, e.gainHF[u]);
			mShelfHF[u] = e.gainHF[u];
		}
	}
	mFreshGain = false;
	bool live[NUM_ENVS] = {};
	float w[NUM_ENVS];
	for(uint32_t i = 0; i < frames;){
		uint32_t n = mState.fadeLeft != 0 ? 1 : frames - i;
		EnvFadeAdvance(mState, n, rate, w);
		for(uint32_t end = i + n; i < end; i++){
			bool on = i >= from && i < to;
			for(int u = 0; u < NUM_ENVS; u++){
				if(mGainLeft[u])
					mGain[u] = --mGainLeft[u] ? mGain[u] + mInc[u] : mTarget[u];
				float v = on ? send[i] * w[u] * mGain[u] : 0.0f;
				out[u][i] = v;
				live[u] = live[u] || v != 0.0f;
			}
		}
	}
	for(int u = 0; u < NUM_ENVS; u++){
		bool ringing = mShelf[u].z1 != 0.0f || mShelf[u].z2 != 0.0f;
		if((live[u] && mShelfHF[u] != 1.0f) || ringing)
			RunBiquad(mShelf[u], out[u], frames);
	}
}

}
